#ifndef TINYPROXY_HTTP_H
#define TINYPROXY_HTTP_H

// What the request, the reply and the error pages share.

#include <stddef.h>

// nginx's own codes: 494 is sent as 400 with its own page, and 499 is only
// logged, as no one is left to send it to.
#define STATUS_HEADER_TOO_LARGE 494
#define STATUS_CLIENT_CLOSED 499

// Versions are nginx's major * 1000 + minor.
#define HTTP_1_0 1000
#define HTTP_1_1 1001

typedef struct {
    const char *name;
    const char *value;
} Header;

typedef struct {
    Header *items;
    size_t count;
} Headers;

// Cuts the next line off *text in place, at "\n" or "\r\n", and moves *text
// past it. NULL once there are none left.
char *next_line(char **text);

int is_space(char c);

// Drops the spaces and tabs around text, in place.
char *trim(char *text);

int is_letter_or_digit(char c);
int is_digit(char c);

// Reads "HTTP/major.minor" up to the end of text or a space.
int read_version(const char **text, int *major, int *minor);

// Now, as a Date header has it.
void format_http_date(char *out, size_t size);

#endif
