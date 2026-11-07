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

// Appends the data in the next piece of the body to out, or drops it if
// out is NULL, as when the chunks are passed on as they came and only their
// end is wanted. Sets *used to how much of the piece was the body's: all of
// it unless the body ended in it, in state CHUNK_DONE.
ChunksResult chunk_decode(ChunkDecoder *decoder, const char *data,
                          size_t size, Buffer *out, size_t *used);

// Appends data to out as one chunk, or nothing if size is 0, as an empty
// chunk would end the body. Fails only when out of memory.
int chunk_encode(Buffer *out, const char *data, size_t size);

// Appends the last chunk, which ends the body.
int chunk_encode_end(Buffer *out);

#endif
