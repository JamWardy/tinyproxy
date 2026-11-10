#include "http.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static char *copy_string(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);

    if (copy) {
        memcpy(copy, text, size);
    }
    return copy;
}

int headers_add_copy(Headers *headers, const char *name, const char *value) {
    Header *items =
        realloc(headers->items, (headers->count + 1) * sizeof(Header));
    char *name_copy = copy_string(name);
    char *value_copy = copy_string(value);

    if (items) {
        headers->items = items;
    }
    if (!items || !name_copy || !value_copy) {
        free(name_copy);
        free(value_copy);
        return 0;
    }
    items[headers->count].name = name_copy;
    items[headers->count].value = value_copy;
    headers->count++;
    return 1;
}

int headers_set_copy(Headers *headers, const char *name, const char *value) {
    for (size_t i = 0; i < headers->count; i++) {
        Header *header = &headers->items[i];

        if (strcasecmp(header->name, name) == 0) {
            char *value_copy = copy_string(value);

            if (!value_copy) {
                return 0;
            }
            free((char *)header->value);
            header->value = value_copy;
            return 1;
        }
    }
    return headers_add_copy(headers, name, value);
}

void headers_free_copies(Headers *headers) {
    for (size_t i = 0; i < headers->count; i++) {
        free((char *)headers->items[i].name);
        free((char *)headers->items[i].value);
    }
    free(headers->items);
    headers->items = NULL;
    headers->count = 0;
}

const char *status_reason(int status) {
    static const struct {
        int status;
        const char *reason;
    } REASONS[] = {
        {200, "OK"},
        {201, "Created"},
        {202, "Accepted"},
        {204, "No Content"},
        {301, "Moved Permanently"},
        {302, "Found"},
        {303, "See Other"},
        {304, "Not Modified"},
        {307, "Temporary Redirect"},
        {308, "Permanent Redirect"},
        {400, "Bad Request"},
        {401, "Unauthorized"},
        {402, "Payment Required"},
        {403, "Forbidden"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {408, "Request Timeout"},
        {409, "Conflict"},
        {410, "Gone"},
        {411, "Length Required"},
        {413, "Content Too Large"},
        {414, "URI Too Long"},
        {415, "Unsupported Media Type"},
        {422, "Unprocessable Content"},
        {429, "Too Many Requests"},
        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {502, "Bad Gateway"},
        {503, "Service Unavailable"},
        {504, "Gateway Timeout"},
        {505, "HTTP Version Not Supported"},
    };

    for (size_t i = 0; i < sizeof(REASONS) / sizeof(REASONS[0]); i++) {
        if (REASONS[i].status == status) {
            return REASONS[i].reason;
        }
    }
    return "Unknown";
}

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

int contains_ignoring_case(const char *text, const char *word) {
    size_t length = strlen(word);

    for (; *text; text++) {
        if (strncasecmp(text, word, length) == 0) {
            return 1;
        }
    }
    return 0;
}
