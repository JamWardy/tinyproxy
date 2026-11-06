#include "error_page.h"

#include <assert.h>

#include "http.h"

typedef struct {
    int status;
    const char *status_line;
    const char *title;
    const char *detail;  // A line under the heading, or NULL.
} ErrorPage;

static const ErrorPage ERROR_PAGES[] = {
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

int build_error_reply(Buffer *out, int status, int with_body) {
    const ErrorPage *page = find_error_page(status);
    char date[40];
    Buffer html;

    assert(page);
    buffer_init(&html);
    int ok = buffer_appendf(&html,
                            "<html>\r\n"
                            "<head><title>%s</title></head>\r\n"
                            "<body>\r\n"
                            "<center><h1>%s</h1></center>\r\n",
                            page->title, page->status_line) &&
             (!page->detail ||
              buffer_appendf(&html, "<center>%s</center>\r\n",
                             page->detail)) &&
             buffer_append_text(&html, "<hr><center>tinyproxy</center>\r\n"
                                       "</body>\r\n"
                                       "</html>\r\n");

    format_http_date(date, sizeof(date));
    ok = ok && buffer_appendf(out,
                              "HTTP/1.1 %s\r\n"
                              "Server: tinyproxy\r\n"
                              "Date: %s\r\n"
                              "Content-Type: text/html\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              page->status_line, date, buffer_length(&html));
    if (ok && with_body) {
        ok = buffer_append(out, buffer_bytes(&html), buffer_length(&html));
    }
    buffer_free(&html);
    return ok;
}
