#ifndef TINYPROXY_BUFFER_H
#define TINYPROXY_BUFFER_H

#include <stddef.h>

// Bytes on their way somewhere. Consuming from the front only moves start, so
// sending a large reply a piece at a time doesn't copy the rest each time.
typedef struct {
    char *data;
    size_t start;  // The first byte not yet consumed.
    size_t end;
    size_t capacity;
} Buffer;

void buffer_init(Buffer *buffer);
void buffer_free(Buffer *buffer);

const char *buffer_bytes(const Buffer *buffer);
size_t buffer_length(const Buffer *buffer);

// These fail only when out of memory, leaving the buffer as it was.
int buffer_append(Buffer *buffer, const void *data, size_t size);
int buffer_append_text(Buffer *buffer, const char *text);
int buffer_appendf(Buffer *buffer, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

void buffer_consume(Buffer *buffer, size_t size);
void buffer_clear(Buffer *buffer);

#endif
