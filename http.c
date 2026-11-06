#include "http.h"

#include <string.h>
#include <time.h>

char *next_line(char **text) {
    char *line = *text;

    if (*line == '\0') {
        return NULL;
    }
    char *end = strchr(line, '\n');
    if (end) {
        *end = '\0';
        *text = end + 1;
    } else {
        *text = line + strlen(line);
    }

    size_t length = strlen(line);
    if (length > 0 && line[length - 1] == '\r') {
        line[length - 1] = '\0';
    }
    return line;
}

int is_space(char c) {
    return c == ' ' || c == '\t';
}

char *trim(char *text) {
    while (is_space(*text)) {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && is_space(text[length - 1])) {
        text[--length] = '\0';
    }
    return text;
}

int is_letter_or_digit(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

int is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Reads 1 to 3 digits at *text, moving past them.
static int read_small_number(const char **text, int *number) {
    int digits = 0;

    *number = 0;
    while (is_digit(**text) && digits < 3) {
        *number = *number * 10 + (**text - '0');
        (*text)++;
        digits++;
    }
    return digits > 0 && !is_digit(**text);
}

int read_version(const char **text, int *major, int *minor) {
    if (strncmp(*text, "HTTP/", 5) != 0) {
        return 0;
    }
    *text += 5;
    if (!read_small_number(text, major) || **text != '.') {
        return 0;
    }
    (*text)++;
    return read_small_number(text, minor);
}

void format_http_date(char *out, size_t size) {
    time_t now = time(NULL);
    struct tm utc;

    gmtime_r(&now, &utc);
    strftime(out, size, "%a, %d %b %Y %H:%M:%S GMT", &utc);
}
