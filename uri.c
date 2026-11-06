#include "uri.h"

#include <stdlib.h>
#include <string.h>

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Decodes path's %XX escapes into decoded, up to any query. decoded needs
// room for the whole path.
static int decode(const char *path, char *decoded) {
    size_t length = 0;

    for (const char *c = path; *c && *c != '?'; c++) {
        if (*c != '%') {
            decoded[length++] = *c;
            continue;
        }
        int high = hex_digit(c[1]);
        int low = high < 0 ? -1 : hex_digit(c[2]);
        // A decoded NUL would end the string early, hiding what's after it.
        if (low < 0 || (high == 0 && low == 0)) {
            return 400;
        }
        decoded[length++] = (char)(high * 16 + low);
        c += 2;
    }
    decoded[length] = '\0';
    return 0;
}

// Drops the last segment from the normalised path in out, for a "..".
static int climb(char *out, size_t *length) {
    if (*length <= 1) {
        return 0;  // Already at "/".
    }
    // out ends in "/", after the segment to drop.
    (*length)--;
    while (*length > 0 && out[*length - 1] != '/') {
        (*length)--;
    }
    return 1;
}

// Walks decoded's segments into out. out always ends in "/" between
// segments, so it's built as "/a/b/" and the last "/" is dropped at the end
// unless the path had one.
static int resolve_segments(const char *decoded, char *out) {
    size_t length = 0;
    int ends_in_slash = 1;
    const char *segment = decoded;

    out[length++] = '/';
    while (*segment) {
        while (*segment == '/') {
            segment++;  // nginx's merge_slashes.
        }
        size_t size = strcspn(segment, "/");
        if (size == 0) {
            break;
        }

        int is_last = segment[size] == '\0';
        if (size == 1 && segment[0] == '.') {
            ends_in_slash = 1;
        } else if (size == 2 && segment[0] == '.' && segment[1] == '.') {
            if (!climb(out, &length)) {
                return 400;
            }
            ends_in_slash = 1;
        } else {
            memcpy(out + length, segment, size);
            length += size;
            out[length++] = '/';
            ends_in_slash = !is_last;
        }
        segment += size;
    }

    if (!ends_in_slash && length > 1) {
        length--;
    }
    out[length] = '\0';
    return 0;
}

int normalise_uri(const char *path, char **uri) {
    size_t size = strlen(path) + 2;
    char *decoded = malloc(size);
    char *out = malloc(size);

    if (!decoded || !out) {
        free(decoded);
        free(out);
        return 500;
    }
    int status = decode(path, decoded);
    if (!status) {
        status = resolve_segments(decoded, out);
    }
    free(decoded);
    if (status) {
        free(out);
        return status;
    }
    *uri = out;
    return 0;
}

// nginx's NGX_ESCAPE_URI: control bytes, space, '#', '%', '?', DEL, and
// anything past ASCII.
static int needs_escape(unsigned char c) {
    return c <= 0x20 || c == '#' || c == '%' || c == '?' || c >= 0x7f;
}

int append_escaped_uri(Buffer *out, const char *uri) {
    static const char HEX[] = "0123456789ABCDEF";

    for (const unsigned char *c = (const unsigned char *)uri; *c; c++) {
        if (!needs_escape(*c)) {
            if (!buffer_append(out, c, 1)) {
                return 0;
            }
            continue;
        }
        char escaped[3] = {'%', HEX[*c >> 4], HEX[*c & 0xf]};
        if (!buffer_append(out, escaped, sizeof(escaped))) {
            return 0;
        }
    }
    return 1;
}
