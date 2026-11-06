#ifndef TINYPROXY_CHUNKED_H
#define TINYPROXY_CHUNKED_H

#include <stddef.h>

#include "buffer.h"

typedef enum {
    CHUNK_SIZE,         // The hex digits of the next chunk's size.
    CHUNK_EXTENSION,    // ";name=value" after them, which is ignored.
    CHUNK_SIZE_LF,      // The '\n' after the size line's '\r'.
    CHUNK_DATA,
    CHUNK_DATA_CR,      // The line end after the data.
    CHUNK_DATA_LF,
    CHUNK_TRAILER,      // Lines after the last chunk, which are dropped.
    CHUNK_DONE,
} ChunkState;

// Decodes a chunked body as it comes, in pieces of any size.
typedef struct {
    ChunkState state;
    size_t size;          // CHUNK_SIZE's so far, then CHUNK_DATA's left.
    size_t digits;        // In CHUNK_SIZE.
    size_t line_length;   // In CHUNK_EXTENSION and CHUNK_TRAILER.
} ChunkDecoder;

typedef enum {
    CHUNKS_OK,
    CHUNKS_INVALID,
    CHUNKS_OUT_OF_MEMORY,
} ChunksResult;

void chunk_decoder_init(ChunkDecoder *decoder);

// Appends the data in the next piece of the body to out. Anything after the
// last chunk and its trailers is ignored.
ChunksResult chunk_decode(ChunkDecoder *decoder, const char *data,
                          size_t size, Buffer *out);

#endif
