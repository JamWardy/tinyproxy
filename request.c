#include "request.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---- Finding the head ----

static HeadScan head_complete(size_t length) {
    HeadScan scan = {HEAD_COMPLETE, length, 0};
    return scan;
}

static HeadScan head_rejected(int status) {
    HeadScan scan = {HEAD_REJECTED, 0, status};
    return scan;
}

static HeadScan line_too_long(int is_request_line) {
    return head_rejected(is_request_line ? 414 : STATUS_HEADER_TOO_LARGE);
}

HeadScan scan_request_head(const char *data, size_t size) {
    size_t line_start = 0;
    int is_request_line = 1;

    for (size_t i = 0; i < size; i++) {
        if (i >= REQUEST_HEAD_LIMIT) {
            return head_rejected(STATUS_HEADER_TOO_LARGE);
        }
        if (data[i] != '\n') {
            continue;
        }

        size_t length = i - line_start;
        if (length > 0 && data[i - 1] == '\r') {
            length--;
        }
        if (length > (is_request_line ? REQUEST_LINE_LIMIT
                                       : HEADER_LINE_LIMIT)) {
            return line_too_long(is_request_line);
        }
        if (length == 0) {
            return is_request_line ? head_rejected(400) : head_complete(i + 1);
        }
        is_request_line = 0;
        line_start = i + 1;
    }

    // The line still coming in is already too long.
    if (size - line_start > (is_request_line ? REQUEST_LINE_LIMIT
                                             : HEADER_LINE_LIMIT)) {
        return line_too_long(is_request_line);
    }
    HeadScan scan = {HEAD_INCOMPLETE, 0, 0};
    return scan;
}

// ---- Parsing ----

// RFC 9110's tchar, what a header's name is made of.
static int is_token_char(char c) {
    return is_letter_or_digit(c) || (c != '\0' && strchr("!#$%&'*+-.^_`|~", c));
}

// nginx's methods are capital letters, '_' and '-'.
static int is_method(const char *method) {
    if (*method == '\0') {
        return 0;
    }
    for (const char *c = method; *c; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || *c == '_' || *c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int parse_version(const char *text, int *version) {
    int major;
    int minor;

    if (!read_version(&text, &major, &minor) || *text != '\0' || major < 1) {
        return 400;
    }
    if (major > 1) {
        return 505;
    }
    *version = major * 1000 + minor;
    return 0;
}

// As nginx's ngx_http_validate_host: lowercased, without the port or a
// trailing dot, and never anything that could reach outside a path.
static int validate_host(const char *text, size_t length, char **host) {
    size_t host_length = length;
    int in_brackets = 0;

    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c == ':' && !in_brackets) {
            host_length = i;
            break;
        }
        if (c == '[' && i == 0) {
            in_brackets = 1;
        } else if (c == ']') {
            in_brackets = 0;
        } else if (c == '/' || c == '\\' || c == '@' || c <= 0x20 ||
                   c == 0x7f || (c == '.' && i > 0 && text[i - 1] == '.')) {
            return 400;
        }
    }
    while (host_length > 0 && text[host_length - 1] == '.') {
        host_length--;
    }
    if (host_length == 0) {
        return 400;
    }

    *host = malloc(host_length + 1);
    if (!*host) {
        return 500;
    }
    for (size_t i = 0; i < host_length; i++) {
        char c = text[i];
        (*host)[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    (*host)[host_length] = '\0';
    return 0;
}

// "http://host/path", which a client may send instead of "/path". Its host
// is $host, ahead of the Host header.
static int parse_absolute_target(Request *request, const char *authority) {
    const char *end = authority + strcspn(authority, "/?");

    if (*end == '?') {
        return 400;
    }
    request->path = *end == '/' ? end : "/";
    return validate_host(authority, (size_t)(end - authority), &request->host);
}

static int parse_request_line(Request *request, char *line) {
    char *space = strchr(line, ' ');
    if (!space) {
        return 400;
    }
    *space = '\0';
    char *target = space + 1;

    space = strchr(target, ' ');
    if (!space) {
        return 400;  // HTTP/0.9, with no version.
    }
    *space = '\0';
    const char *version = space + 1;

    if (!is_method(line) || *target == '\0') {
        return 400;
    }
    for (const unsigned char *c = (const unsigned char *)target; *c; c++) {
        if (*c <= 0x20 || *c == 0x7f) {
            return 400;
        }
    }
    int status = parse_version(version, &request->version);
    if (status) {
        return status;
    }

    request->method = line;
    request->target = target;
    if (target[0] == '/') {
        request->path = target;
        return 0;
    }
    if (strncasecmp(target, "http://", 7) == 0) {
        return parse_absolute_target(request, target + 7);
    }
    return 400;
}

static int parse_content_length(const char *text, size_t *length) {
    if (*text == '\0') {
        return 0;
    }
    *length = 0;
    for (const char *c = text; *c; c++) {
        if (!is_digit(*c)) {
            return 0;
        }
        size_t digit = (size_t)(*c - '0');
        if (*length > (SIZE_MAX - digit) / 10) {
            return 0;
        }
        *length = *length * 10 + digit;
    }
    return 1;
}

// nginx's ignore_invalid_headers and underscores_in_headers off: a header
// whose name has anything but letters, digits and '-' is dropped. So
// X_User_Id can't pass for X-User-Id, which a WSGI app sees the same way.
static int is_passed_on(const char *name) {
    for (const char *c = name; *c; c++) {
        if (!is_letter_or_digit(*c) && *c != '-') {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    int host;
    int content_length;
} SeenHeaders;

// Acts on the headers that change how the request is read.
static int take_special_header(Request *request, const char *name,
                               const char *value, SeenHeaders *seen) {
    if (strcasecmp(name, "Host") == 0) {
        char *host;

        if (seen->host) {
            return 400;
        }
        seen->host = 1;
        int status = validate_host(value, strlen(value), &host);
        if (status) {
            return status;
        }
        if (request->host) {
            free(host);  // The target's host came first.
        } else {
            request->host = host;
        }
    } else if (strcasecmp(name, "Content-Length") == 0) {
        if (seen->content_length ||
            !parse_content_length(value, &request->content_length)) {
            return 400;
        }
        seen->content_length = 1;
    } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
        // A request's transfer coding must end in chunked, which tinyproxy
        // can't read, so any is refused as nginx did before it could.
        return 411;
    } else if (strcasecmp(name, "Expect") == 0) {
        request->expects_continue = request->version >= HTTP_1_1 &&
                                    strcasecmp(value, "100-continue") == 0;
    }
    return 0;
}

static int parse_header_line(Request *request, char *line,
                             SeenHeaders *seen) {
    // A line starting with a space continues the last one, which RFC 9112
    // lets a server refuse.
    if (is_space(*line)) {
        return 400;
    }
    char *colon = strchr(line, ':');
    if (!colon || colon == line) {
        return 400;
    }
    *colon = '\0';
    for (const char *c = line; *c; c++) {
        if (!is_token_char(*c)) {
            return 400;
        }
    }
    char *value = trim(colon + 1);
    for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
        if ((*c < 0x20 && *c != '\t') || *c == 0x7f) {
            return 400;
        }
    }

    if (!is_passed_on(line)) {
        return 0;
    }
    int status = take_special_header(request, line, value, seen);
    if (status) {
        return status;
    }
    request->headers[request->header_count].name = line;
    request->headers[request->header_count].value = value;
    request->header_count++;
    return 0;
}

void request_init(Request *request) {
    request->line = NULL;
    request->head = NULL;
    request->method = NULL;
    request->target = NULL;
    request->path = NULL;
    request->version = 0;
    request->host = NULL;
    request->headers = NULL;
    request->header_count = 0;
    request->content_length = 0;
    request->expects_continue = 0;
}

void request_free(Request *request) {
    free(request->line);
    free(request->head);
    free(request->host);
    free(request->headers);
    request_init(request);
}

static size_t count_lines(const char *data, size_t length) {
    size_t count = 0;

    for (size_t i = 0; i < length; i++) {
        count += data[i] == '\n';
    }
    return count;
}

int request_parse(Request *request, const char *head, size_t length) {
    SeenHeaders seen = {0, 0};

    if (memchr(head, '\0', length)) {
        return 400;
    }
    request->head = malloc(length + 1);
    // Each header has a line of its own, so this has room for them all.
    request->headers = malloc(count_lines(head, length) * sizeof(Header));
    if (!request->head || !request->headers) {
        return 500;
    }
    memcpy(request->head, head, length);
    request->head[length] = '\0';

    // scan_request_head saw a request line, so there's one.
    char *cursor = request->head;
    char *line = next_line(&cursor);
    request->line = malloc(strlen(line) + 1);
    if (!request->line) {
        return 500;
    }
    strcpy(request->line, line);

    int status = parse_request_line(request, line);
    while (!status && (line = next_line(&cursor)) && *line) {
        status = parse_header_line(request, line, &seen);
    }
    if (status) {
        return status;
    }

    if (request->version >= HTTP_1_1 && !seen.host) {
        return 400;
    }
    if (!request->host) {
        request->host = malloc(1);
        if (!request->host) {
            return 500;
        }
        request->host[0] = '\0';
    }
    return 0;
}

const char *request_find_header(const Request *request, const char *name) {
    for (size_t i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}
