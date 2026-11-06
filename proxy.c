#include "proxy.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

int build_upstream_request(Buffer *out, const Request *request,
                           int version, const Headers *proxy_headers,
                           const Variables *variables, const char *body) {
    if (!buffer_appendf(out, "%s %s HTTP/%d.%d\r\n", request->method,
                        request->path, version / 1000, version % 1000)) {
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
static int parse_status_line(const char *line, int *code,
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

static int copy_reply_headers(Buffer *out, char *cursor,
                              const char *status_text, int client_version,
                              ReplyHead *reply) {
    char date[40];
    char *line;
    int chunked = 0;
    int has_content_length = 0;

    format_http_date(date, sizeof(date));
    if (!buffer_appendf(out, "HTTP/1.1 %s\r\nServer: tinyproxy\r\n"
                             "Date: %s\r\n", status_text, date)) {
        return 500;
    }

    while ((line = next_line(&cursor)) && *line) {
        char *colon = strchr(line, ':');

        if (is_space(*line) || !colon || colon == line) {
            return 502;
        }
        *colon = '\0';
        char *value = trim(colon + 1);

        if (strcasecmp(line, "Transfer-Encoding") == 0) {
            // nginx takes only chunked from an upstream.
            if (chunked || strcasecmp(value, "chunked") != 0) {
                return 502;
            }
            chunked = 1;
            if (client_version < HTTP_1_1) {
                continue;
            }
        } else if (strcasecmp(line, "Content-Length") == 0) {
            has_content_length = 1;
        }
        if (!is_hidden(line) &&
            !append_header(out, line, value, strlen(value))) {
            return 500;
        }
    }
    // With both, the two ends could disagree on where the body ends.
    if (chunked && has_content_length) {
        return 502;
    }
    reply->decode_chunks = chunked && client_version < HTTP_1_1;
    return buffer_append_text(out, "Connection: close\r\n\r\n") ? 0 : 500;
}

int rewrite_reply_head(Buffer *out, const char *head, size_t length,
                       int client_version, ReplyHead *reply) {
    int code;
    const char *status_text;

    if (memchr(head, '\0', length)) {
        return 502;
    }
    char *copy = malloc(length + 1);
    if (!copy) {
        return 500;
    }
    memcpy(copy, head, length);
    copy[length] = '\0';

    char *cursor = copy;
    char *line = next_line(&cursor);
    int result = line && parse_status_line(line, &code, &status_text)
                     ? copy_reply_headers(out, cursor, status_text,
                                          client_version, reply)
                     : 502;
    if (result == 0) {
        reply->status = code;
    }
    free(copy);
    return result;
}
