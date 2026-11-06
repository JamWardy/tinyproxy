#include "chunked.h"

// A chunk's size has at most this many hex digits, so it fits in a size_t.
#define CHUNK_SIZE_DIGITS 15
// nginx refuses longer size and trailer lines too.
#define CHUNK_LINE_LIMIT 4096

void chunk_decoder_init(ChunkDecoder *decoder) {
    decoder->state = CHUNK_SIZE;
    decoder->size = 0;
    decoder->digits = 0;
    decoder->line_length = 0;
}

static int hex_value(char c) {
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

// The size line is over: the data comes next, or the trailers after the
// last chunk, which has size 0.
static void end_size_line(ChunkDecoder *decoder) {
    decoder->state = decoder->size == 0 ? CHUNK_TRAILER : CHUNK_DATA;
    decoder->line_length = 0;
}

static void start_next_chunk(ChunkDecoder *decoder) {
    decoder->state = CHUNK_SIZE;
    decoder->size = 0;
    decoder->digits = 0;
    decoder->line_length = 0;
}

// Moves on by one byte of anything but the data. Fails if it's out of place.
static int take_framing_byte(ChunkDecoder *decoder, char c) {
    switch (decoder->state) {
        case CHUNK_SIZE: {
            int value = hex_value(c);
            if (value >= 0) {
                if (decoder->digits == CHUNK_SIZE_DIGITS) {
                    return 0;
                }
                decoder->size = decoder->size * 16 + (size_t)value;
                decoder->digits++;
                return 1;
            }
            if (decoder->digits == 0) {
                return 0;
            }
            if (c == ';' || c == ' ' || c == '\t') {
                decoder->state = CHUNK_EXTENSION;
            } else if (c == '\r') {
                decoder->state = CHUNK_SIZE_LF;
            } else if (c == '\n') {
                end_size_line(decoder);
            } else {
                return 0;
            }
            return 1;
        }
        case CHUNK_EXTENSION:
            if (c == '\r') {
                decoder->state = CHUNK_SIZE_LF;
            } else if (c == '\n') {
                end_size_line(decoder);
            } else if (++decoder->line_length > CHUNK_LINE_LIMIT) {
                return 0;
            }
            return 1;
        case CHUNK_SIZE_LF:
            if (c != '\n') {
                return 0;
            }
            end_size_line(decoder);
            return 1;
        case CHUNK_DATA_CR:
            if (c == '\r') {
                decoder->state = CHUNK_DATA_LF;
            } else if (c == '\n') {
                start_next_chunk(decoder);
            } else {
                return 0;
            }
            return 1;
        case CHUNK_DATA_LF:
            if (c != '\n') {
                return 0;
            }
            start_next_chunk(decoder);
            return 1;
        case CHUNK_TRAILER:
            if (c == '\n') {
                if (decoder->line_length == 0) {
                    decoder->state = CHUNK_DONE;
                }
                decoder->line_length = 0;
            } else if (c != '\r' &&
                       ++decoder->line_length > CHUNK_LINE_LIMIT) {
                return 0;
            }
            return 1;
        case CHUNK_DATA:
        case CHUNK_DONE:
            return 1;  // chunk_decode handles these itself.
    }
    return 0;
}

ChunksResult chunk_decode(ChunkDecoder *decoder, const char *data,
                          size_t size, Buffer *out) {
    size_t i = 0;

    while (i < size && decoder->state != CHUNK_DONE) {
        if (decoder->state != CHUNK_DATA) {
            if (!take_framing_byte(decoder, data[i])) {
                return CHUNKS_INVALID;
            }
            i++;
            continue;
        }
        size_t run = size - i < decoder->size ? size - i : decoder->size;
        if (!buffer_append(out, data + i, run)) {
            return CHUNKS_OUT_OF_MEMORY;
        }
        i += run;
        decoder->size -= run;
        if (decoder->size == 0) {
            decoder->state = CHUNK_DATA_CR;
        }
    }
    return CHUNKS_OK;
}
