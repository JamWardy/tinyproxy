#include "error_page.h"

#include <assert.h>

#include <string.h>
#include <strings.h>

typedef struct {
    int status;
    const char *status_line;
    const char *title;
    const char *detail;  // A line under the heading, or NULL.
} ErrorPage;

static const ErrorPage ERROR_PAGES[] = {
    {301, "301 Moved Permanently", "301 Moved Permanently", NULL},
    {400, "400 Bad Request", "400 Bad Request", NULL},
    {404, "404 Not Found", "404 Not Found", NULL},
    {408, "408 Request Time-out", "408 Request Time-out", NULL},
    {411, "411 Length Required", "411 Length Required", NULL},
    {413, "413 Request Entity Too Large", "413 Request Entity Too Large",
     NULL},
    {414, "414 Request-URI Too Large", "414 Request-URI Too Large", NULL},
    {STATUS_HEADER_TOO_LARGE, "400 Bad Request",
     "400 Request Header Or Cookie Too Large",
     "Request Header Or Cookie Too Large"},
    {500, "500 Internal Server Error", "500 Internal Server Error", NULL},
    {502, "502 Bad Gateway", "502 Bad Gateway", NULL},
    {504, "504 Gateway Time-out", "504 Gateway Time-out", NULL},
    {505, "505 HTTP Version Not Supported", "505 HTTP Version Not Supported",
     NULL},
};

static const ErrorPage *find_error_page(int status) {
    for (size_t i = 0; i < sizeof(ERROR_PAGES) / sizeof(ERROR_PAGES[0]);
         i++) {
        if (ERROR_PAGES[i].status == status) {
            return &ERROR_PAGES[i];
        }
    }
    return NULL;
}

int own_page_reply(HookReply *reply, int status, const char *location) {
    const ErrorPage *page = find_error_page(status);

    assert(page);
    reply->status = status == STATUS_HEADER_TOO_LARGE ? 400 : status;
    reply->status_line = page->status_line;
    return headers_add_copy(&reply->headers, "Content-Type", "text/html") &&
           (!location ||
            headers_add_copy(&reply->headers, "Location", location)) &&
           buffer_appendf(&reply->body,
                          "<html>\r\n"
                          "<head><title>%s</title></head>\r\n"
                          "<body>\r\n"
                          "<center><h1>%s</h1></center>\r\n",
                          page->title, page->status_line) &&
           (!page->detail ||
            buffer_appendf(&reply->body, "<center>%s</center>\r\n",
                           page->detail)) &&
           buffer_append_text(&reply->body, "<hr><center>tinyproxy</center>\r\n"
                                            "</body>\r\n"
                                            "</html>\r\n");
}

void hook_reply_init(HookReply *reply) {
    reply->status = 0;
    reply->status_line = NULL;
    reply->headers.items = NULL;
    reply->headers.count = 0;
    buffer_init(&reply->body);
}

void hook_reply_free(HookReply *reply) {
    headers_free_copies(&reply->headers);
    buffer_free(&reply->body);
}

// What tinyproxy writes itself, so a hook's would be a second one.
static int is_own_header(const char *name) {
    return strcasecmp(name, "Server") == 0 || strcasecmp(name, "Date") == 0 ||
           strcasecmp(name, "Content-Length") == 0 ||
           strcasecmp(name, "Transfer-Encoding") == 0 ||
           strcasecmp(name, "Connection") == 0 ||
           strcasecmp(name, "Keep-Alive") == 0;
}

int build_hook_reply(Buffer *out, const HookReply *reply, int with_body,
                     const char *connection) {
    char date[40];

    format_http_date(date, sizeof(date));
    int ok = (reply->status_line
                  ? buffer_appendf(out, "HTTP/1.1 %s\r\n", reply->status_line)
                  : buffer_appendf(out, "HTTP/1.1 %d %s\r\n", reply->status,
                                   status_reason(reply->status))) &&
             buffer_appendf(out,
                            "Server: tinyproxy\r\n"
                            "Date: %s\r\n",
                            date);
    for (size_t i = 0; ok && i < reply->headers.count; i++) {
        const Header *header = &reply->headers.items[i];

        if (!is_own_header(header->name)) {
            ok = buffer_appendf(out, "%s: %s\r\n", header->name,
                                header->value);
        }
    }
    ok = ok &&
         buffer_appendf(out, "Content-Length: %zu\r\n",
                        buffer_length(&reply->body)) &&
         buffer_append_text(out, connection) && buffer_append(out, "\r\n", 2);
    if (ok && with_body) {
        ok = buffer_append(out, buffer_bytes(&reply->body),
                           buffer_length(&reply->body));
    }
    return ok;
}
