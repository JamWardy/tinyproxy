#include "buffer.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 1024

void buffer_init(Buffer *buffer) {
    buffer->data = NULL;
    buffer->start = 0;
    buffer->end = 0;
    buffer->capacity = 0;
}

void buffer_free(Buffer *buffer) {
    free(buffer->data);
    buffer_init(buffer);
}

const char *buffer_bytes(const Buffer *buffer) {
    return buffer->data ? buffer->data + buffer->start : "";
}

size_t buffer_length(const Buffer *buffer) {
    return buffer->end - buffer->start;
}

// Makes room for size more bytes at the end, first by moving what's left to
// the front, then by growing.
static int reserve(Buffer *buffer, size_t size) {
    if (buffer->capacity - buffer->end >= size) {
        return 1;
    }
    if (buffer->start > 0) {
        memmove(buffer->data, buffer->data + buffer->start,
                buffer->end - buffer->start);
        buffer->end -= buffer->start;
        buffer->start = 0;
        if (buffer->capacity - buffer->end >= size) {
            return 1;
        }
    }

    if (size > SIZE_MAX - buffer->end) {
        return 0;
    }
    size_t needed = buffer->end + size;
    size_t capacity = buffer->capacity ? buffer->capacity : INITIAL_CAPACITY;
    while (capacity < needed) {
        capacity = capacity > SIZE_MAX / 2 ? needed : capacity * 2;
    }

    char *data = realloc(buffer->data, capacity);
    if (!data) {
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

int buffer_append(Buffer *buffer, const void *data, size_t size) {
    if (size == 0) {
        return 1;
    }
    if (!reserve(buffer, size)) {
        return 0;
    }
    memcpy(buffer->data + buffer->end, data, size);
    buffer->end += size;
    return 1;
}

int buffer_append_text(Buffer *buffer, const char *text) {
    return buffer_append(buffer, text, strlen(text));
}

int buffer_appendf(Buffer *buffer, const char *format, ...) {
    va_list args;
    va_list again;

    va_start(args, format);
    va_copy(again, args);
    int length = vsnprintf(NULL, 0, format, args);
    va_end(args);

    // vsnprintf writes a terminating NUL, so it needs one byte more.
    int ok = length >= 0 && reserve(buffer, (size_t)length + 1);
    if (ok) {
        vsnprintf(buffer->data + buffer->end, (size_t)length + 1, format,
                  again);
        buffer->end += (size_t)length;
    }
    va_end(again);
    return ok;
}

void buffer_consume(Buffer *buffer, size_t size) {
    buffer->start += size;
    if (buffer->start == buffer->end) {
        buffer_clear(buffer);
    }
}

void buffer_clear(Buffer *buffer) {
    buffer->start = 0;
    buffer->end = 0;
}
