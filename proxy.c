#include "proxy.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "uri.h"

// ---- The request ----

static int append_header(Buffer *out, const char *name, const char *value,
                         size_t value_length) {
    return buffer_append_text(out, name) && buffer_append(out, ": ", 2) &&
           buffer_append(out, value, value_length) &&
           buffer_append(out, "\r\n", 2);
}

static int append_proxy_header(Buffer *out, const Header *header,
                               const Variables *variables) {
    Buffer value;

    buffer_init(&value);
    int ok = expand_variables(&value, header->value, variables);
    // As in nginx, a header that comes out empty isn't sent.
    if (ok && buffer_length(&value) > 0) {
        ok = append_header(out, header->name, buffer_bytes(&value),
                           buffer_length(&value));
    }
    buffer_free(&value);
    return ok;
}

static int is_replaced(const Headers *proxy_headers, const char *name) {
    for (size_t i = 0; i < proxy_headers->count; i++) {
        if (strcasecmp(proxy_headers->items[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

int build_upstream_uri(Buffer *out, const Request *request,
                       const Location *location) {
    const char *replacement = location->proxy_pass.uri;

    if (!replacement) {
        return buffer_append_text(out, request->path_and_query);
    }
    size_t matched = location->match == LOCATION_EXACT
                         ? strlen(request->path)
                         : strlen(location->path);
    int ok = buffer_append_text(out, replacement) &&
             append_escaped_uri(out, request->path + matched);
    if (ok && request->query && *request->query) {
        ok = buffer_append(out, "?", 1) &&
             buffer_append_text(out, request->query);
    }
    return ok;
}

int build_upstream_request(Buffer *out, const Request *request,
                           const char *uri, int version,
                           const Headers *proxy_headers,
                           const Variables *variables, const char *body) {
    if (!buffer_appendf(out, "%s %s HTTP/%d.%d\r\n", request->method, uri,
                        version / 1000, version % 1000)) {
        return 0;
    }
    for (size_t i = 0; i < proxy_headers->count; i++) {
        if (!append_proxy_header(out, &proxy_headers->items[i], variables)) {
            return 0;
        }
    }
    for (size_t i = 0; i < request->header_count; i++) {
        const Header *header = &request->headers[i];

        if (!is_replaced(proxy_headers, header->name) &&
            !append_header(out, header->name, header->value,
                           strlen(header->value))) {
            return 0;
        }
    }
    return buffer_append(out, "\r\n", 2) &&
           buffer_append(out, body, request->content_length);
}

// ---- The reply ----

size_t find_reply_head(const char *data, size_t size) {
    for (size_t i = 1; i < size; i++) {
        if (data[i] != '\n') {
            continue;
        }
        // An empty line: "\n\n" or "\n\r\n".
        if (data[i - 1] == '\n' ||
            (i >= 2 && data[i - 1] == '\r' && data[i - 2] == '\n')) {
            return i + 1;
        }
    }
    return 0;
}

// "HTTP/1.x 200 OK"; status_text is set to the "200 OK".
static int parse_status_line(const char *line, int *version, int *code,
                             const char **status_text) {
    int major;
    int minor;

    if (!read_version(&line, &major, &minor) || *line != ' ') {
        return 0;
    }
    line++;
    for (int i = 0; i < 3; i++) {
        if (!is_digit(line[i])) {
            return 0;
        }
    }
    if (line[3] != '\0' && line[3] != ' ') {
        return 0;
    }
    *version = major * 1000 + minor;
    *code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    *status_text = line;
    return *code >= 100;
}

// As nginx's proxy_hide_header defaults, and the hop-by-hop headers it drops.
static int is_hidden(const char *name) {
    return strcasecmp(name, "Date") == 0 || strcasecmp(name, "Server") == 0 ||
           strcasecmp(name, "Connection") == 0 ||
           strcasecmp(name, "Keep-Alive") == 0;
}

static int parse_length(const char *text, size_t *length) {
    *length = 0;
    if (*text == '\0') {
        return 0;
    }
    for (const char *c = text; *c; c++) {
        size_t digit = (size_t)(*c - '0');

        if (!is_digit(*c) || *length > (SIZE_MAX - digit) / 10) {
            return 0;
        }
        *length = *length * 10 + digit;
    }
    return 1;
}

// A copy of head as a string, with its status line read. Returns 0, or
// the status to answer with instead.
static int open_head(const char *head, size_t length, char **copy,
                     char **cursor, int *version, int *code,
                     const char **status_text) {
    *copy = NULL;
    if (memchr(head, '\0', length)) {
        return 502;
    }
    *copy = malloc(length + 1);
    if (!*copy) {
        return 500;
    }
    memcpy(*copy, head, length);
    (*copy)[length] = '\0';

    *cursor = *copy;
    char *line = next_line(cursor);
    return line && parse_status_line(line, version, code, status_text) ? 0
                                                                       : 502;
}

// The next header, cut into name and value. 0 at the blank line, -1 if it
// isn't a header.
static int next_header(char **cursor, char **name, char **value) {
    char *line = next_line(cursor);

    if (!line || !*line) {
        return 0;
    }
    char *colon = strchr(line, ':');
    if (is_space(*line) || !colon || colon == line) {
        return -1;
    }
    *colon = '\0';
    *name = line;
    *value = trim(colon + 1);
    return 1;
}

static int read_reply_headers(char *cursor, ReplyHead *reply) {
    char *name;
    char *value;
    int found;
    int chunked = 0;
    int has_length = 0;

    while ((found = next_header(&cursor, &name, &value)) == 1) {
        if (strcasecmp(name, "Transfer-Encoding") == 0) {
            // nginx takes only chunked from an upstream.
            if (chunked || strcasecmp(value, "chunked") != 0) {
                return 502;
            }
            chunked = 1;
        } else if (strcasecmp(name, "Content-Length") == 0) {
            if (has_length || !parse_length(value, &reply->length)) {
                return 502;
            }
            has_length = 1;
        } else if (strcasecmp(name, "Connection") == 0 &&
                   contains_ignoring_case(value, "close")) {
            reply->upstream_reusable = 0;
        }
    }
    if (found < 0) {
        return 502;
    }
    // With both, the two ends could disagree on where the body ends.
    if (chunked && has_length) {
        return 502;
    }
    if (reply->framing != REPLY_NO_BODY) {
        reply->framing = chunked      ? REPLY_CHUNKED
                         : has_length ? REPLY_LENGTH
                                      : REPLY_UNTIL_CLOSE;
    }
    return 0;
}

int parse_reply_head(const char *head, size_t length, int to_head,
                     ReplyHead *reply) {
    char *copy;
    char *cursor;
    int version;
    int code;
    const char *status_text;
    int result = open_head(head, length, &copy, &cursor, &version, &code,
                           &status_text);

    if (result == 0) {
        reply->status = code;
        reply->length = 0;
        reply->upstream_reusable = version >= HTTP_1_1;
        reply->framing = to_head || code < 200 || code == 204 || code == 304
                             ? REPLY_NO_BODY
                             : REPLY_UNTIL_CLOSE;
        result = read_reply_headers(cursor, reply);
    }
    free(copy);
    return result;
}

static int copy_reply_headers(Buffer *out, char *cursor,
                              const char *status_text, BodyRewrite rewrite,
                              const char *connection) {
    char date[40];
    char *name;
    char *value;

    format_http_date(date, sizeof(date));
    if (!buffer_appendf(out, "HTTP/1.1 %s\r\nServer: tinyproxy\r\n"
                             "Date: %s\r\n", status_text, date)) {
        return 0;
    }
    // parse_reply_head checked every line.
    while (next_header(&cursor, &name, &value) == 1) {
        int dropped = is_hidden(name) ||
                      (rewrite == REWRITE_DECHUNK &&
                       strcasecmp(name, "Transfer-Encoding") == 0);

        if (!dropped && !append_header(out, name, value, strlen(value))) {
            return 0;
        }
    }
    return (rewrite != REWRITE_CHUNK ||
            buffer_append_text(out, "Transfer-Encoding: chunked\r\n")) &&
           buffer_append_text(out, connection) &&
           buffer_append(out, "\r\n", 2);
}

int write_reply_head(Buffer *out, const char *head, size_t length,
                     BodyRewrite rewrite, const char *connection) {
    char *copy;
    char *cursor;
    int version;
    int code;
    const char *status_text;
    int ok = open_head(head, length, &copy, &cursor, &version, &code,
                       &status_text) == 0 &&
             copy_reply_headers(out, cursor, status_text, rewrite, connection);

    free(copy);
    return ok;
}
