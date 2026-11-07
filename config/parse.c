#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "internal.h"

typedef enum {
    TOKEN_WORD,
    TOKEN_OPEN,
    TOKEN_CLOSE,
    TOKEN_SEMICOLON,
    TOKEN_END_OF_FILE,
} TokenType;

typedef struct {
    TokenType type;
    char *word;  // TOKEN_WORD's, in the arena.
    size_t line;
} Token;

int parse_error(Parser *parser, size_t line, const char *format, ...) {
    char message[512];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    snprintf(parser->error, parser->error_size, "%s in %s:%zu", message,
             parser->path, line);
    return 0;
}

int duplicate(Parser *parser, const Directive *directive) {
    return parse_error(parser, directive->line,
                       "\"%s\" directive is duplicate", directive->name);
}

int invalid_value(Parser *parser, const Directive *directive) {
    return parse_error(parser, directive->line,
                       "\"%s\" directive invalid value", directive->name);
}

int out_of_memory(Parser *parser, const Directive *directive) {
    return parse_error(parser, directive->line, "out of memory");
}

int invalid_parameter(Parser *parser, const Directive *directive,
                      const char *option) {
    return parse_error(parser, directive->line, "invalid parameter \"%s\"",
                       option);
}

// ---- Tokens ----

static int is_special(char c) {
    return c == '{' || c == '}' || c == ';';
}

static int is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_blanks_and_comments(Parser *parser) {
    const char *text = parser->text;

    while (text[parser->position]) {
        char c = text[parser->position];

        if (c == '#') {
            while (text[parser->position] && text[parser->position] != '\n') {
                parser->position++;
            }
        } else if (is_blank(c)) {
            parser->line += c == '\n';
            parser->position++;
        } else {
            return;
        }
    }
}

// What a backslash and c stand for, as in nginx.
static char unescape(char c) {
    switch (c) {
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        default:
            return c;
    }
}

// Copies a word into the arena, quoted or not, without its quotes and with
// its escapes done. The arena is twice the file's size, so it always fits.
static int read_word(Parser *parser, Token *token) {
    const char *text = parser->text;
    char quote = 0;
    char *out = parser->config->arena + parser->arena_used;

    token->type = TOKEN_WORD;
    token->word = out;
    token->line = parser->line;

    if (text[parser->position] == '"' || text[parser->position] == '\'') {
        quote = text[parser->position++];
    }
    while (1) {
        char c = text[parser->position];

        if (c == '\0') {
            if (quote) {
                return parse_error(parser, parser->line,
                                   "unexpected end of file, expecting "
                                   "\";\" or \"}\"");
            }
            break;
        }
        if (quote ? c == quote : (is_blank(c) || is_special(c))) {
            parser->position += quote ? 1 : 0;
            break;
        }
        parser->line += c == '\n';
        if (c == '\\' && text[parser->position + 1]) {
            parser->position++;
            c = unescape(text[parser->position]);
        }
        *out++ = c;
        parser->position++;
    }
    *out++ = '\0';
    parser->arena_used = (size_t)(out - parser->config->arena);
    return 1;
}

static int next_token(Parser *parser, Token *token) {
    skip_blanks_and_comments(parser);
    token->line = parser->line;

    switch (parser->text[parser->position]) {
        case '\0':
            token->type = TOKEN_END_OF_FILE;
            return 1;
        case '{':
            token->type = TOKEN_OPEN;
            break;
        case '}':
            token->type = TOKEN_CLOSE;
            break;
        case ';':
            token->type = TOKEN_SEMICOLON;
            break;
        default:
            return read_word(parser, token);
    }
    parser->position++;
    return 1;
}

// ---- Values ----

// nginx's times: a number with ms, s, m, h, d, w, M or y after it, and
// several of those, such as "1m30s". A bare number is seconds.
int parse_seconds(const char *text, double *seconds) {
    *seconds = 0;
    if (*text == '\0') {
        return 0;
    }
    while (*text) {
        char *end;
        long number = strtol(text, &end, 10);
        double unit;

        if (end == text || number < 0 || *text == '-' || *text == '+') {
            return 0;
        }
        text = end;
        if (strncmp(text, "ms", 2) == 0) {
            unit = 0.001;
            text += 2;
        } else {
            switch (*text) {
                case '\0':
                case 's':
                    unit = 1;
                    break;
                case 'm':
                    unit = 60;
                    break;
                case 'h':
                    unit = 60 * 60;
                    break;
                case 'd':
                    unit = 24 * 60 * 60;
                    break;
                case 'w':
                    unit = 7 * 24 * 60 * 60;
                    break;
                case 'M':
                    unit = 30 * 24 * 60 * 60;
                    break;
                case 'y':
                    unit = 365 * 24 * 60 * 60;
                    break;
                default:
                    return 0;
            }
            text += *text ? 1 : 0;
        }
        *seconds += (double)number * unit;
    }
    return 1;
}

// nginx's sizes: a number of bytes, or of k, m or g.
int parse_size(const char *text, size_t *bytes) {
    char *end;
    unsigned long long number;
    unsigned long long unit = 1;

    if (*text < '0' || *text > '9') {
        return 0;
    }
    errno = 0;
    number = strtoull(text, &end, 10);
    if (errno == ERANGE) {
        return 0;
    }
    switch (*end) {
        case '\0':
            break;
        case 'k':
        case 'K':
            unit = 1024;
            break;
        case 'm':
        case 'M':
            unit = 1024 * 1024;
            break;
        case 'g':
        case 'G':
            unit = 1024 * 1024 * 1024;
            break;
        default:
            return 0;
    }
    if (*end && end[1] != '\0') {
        return 0;
    }
    if (number > (unsigned long long)SIZE_MAX / unit) {
        return 0;
    }
    *bytes = (size_t)(number * unit);
    return 1;
}

// A whole number, as nginx's ngx_atoi.
int parse_count(const char *text, size_t *count) {
    *count = 0;
    if (*text == '\0') {
        return 0;
    }
    for (const char *c = text; *c; c++) {
        size_t digit = (size_t)(*c - '0');

        if (*c < '0' || *c > '9' || *count > (SIZE_MAX - digit) / 10) {
            return 0;
        }
        *count = *count * 10 + digit;
    }
    return 1;
}

int parse_port(const char *text, unsigned short *port) {
    char *end;
    long number = strtol(text, &end, 10);

    if (*text < '0' || *text > '9' || *end != '\0' || number < 1 ||
        number > 65535) {
        return 0;
    }
    *port = (unsigned short)number;
    return 1;
}

// Looks a host's IPv4 address up once, at startup, as nginx does.
int resolve(const char *host, unsigned short port,
            struct sockaddr_in *address) {
    struct addrinfo hints;
    struct addrinfo *found;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0) {
        return 0;
    }
    memcpy(address, found->ai_addr, sizeof(*address));
    address->sin_port = htons(port);
    freeaddrinfo(found);
    return 1;
}

// Splits "host:port" at its last ':', into host, which must have room for
// all of text. Leaves port alone if there's no ':'.
int split_host_port(const char *text, char *host,
                    unsigned short *port) {
    const char *colon = strrchr(text, ':');

    if (!colon) {
        strcpy(host, text);
        return 1;
    }
    memcpy(host, text, (size_t)(colon - text));
    host[colon - text] = '\0';
    return parse_port(colon + 1, port);
}

char *copy_text(const char *text, size_t length) {
    char *copy = malloc(length + 1);

    if (copy) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

// ---- Blocks ----

// Reads a directive's arguments, up to its ";" or "{".
static int read_arguments(Parser *parser, Directive *directive) {
    Token token;

    while (1) {
        if (!next_token(parser, &token)) {
            return 0;
        }
        switch (token.type) {
            case TOKEN_WORD:
                if (directive->argument_count == MAX_ARGUMENTS) {
                    return parse_error(parser, token.line,
                                       "invalid number of arguments in "
                                       "\"%s\" directive", directive->name);
                }
                directive->arguments[directive->argument_count++] =
                    token.word;
                break;
            case TOKEN_SEMICOLON:
                directive->opens_block = 0;
                return 1;
            case TOKEN_OPEN:
                directive->opens_block = 1;
                return 1;
            case TOKEN_CLOSE:
                return parse_error(parser, token.line, "unexpected \"}\"");
            case TOKEN_END_OF_FILE:
                return parse_error(parser, token.line,
                                   "unexpected end of file, expecting "
                                   "\";\" or \"}\"");
        }
    }
}

// Reads directives up to the block's "}", or at the top, the end of the file.
int parse_block(Parser *parser, const Scope *scope) {
    int at_top = scope->context == CONTEXT_MAIN;
    Token token;

    while (1) {
        if (!next_token(parser, &token)) {
            return 0;
        }
        switch (token.type) {
            case TOKEN_WORD:
                break;
            case TOKEN_END_OF_FILE:
                return at_top ? 1
                              : parse_error(parser, token.line,
                                            "unexpected end of file, "
                                            "expecting \"}\"");
            case TOKEN_CLOSE:
                return at_top ? parse_error(parser, token.line,
                                            "unexpected \"}\"")
                              : 1;
            case TOKEN_OPEN:
                return parse_error(parser, token.line, "unexpected \"{\"");
            case TOKEN_SEMICOLON:
                return parse_error(parser, token.line, "unexpected \";\"");
        }

        Directive directive;
        directive.name = token.word;
        directive.line = token.line;
        directive.argument_count = 0;
        if (!read_arguments(parser, &directive) ||
            !run_directive(parser, scope, &directive)) {
            return 0;
        }
    }
}
