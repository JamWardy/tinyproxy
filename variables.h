#ifndef TINYPROXY_VARIABLES_H
#define TINYPROXY_VARIABLES_H

#include <stddef.h>

#include "buffer.h"

// The values proxy_set_header and hash can use.
typedef struct {
    const char *host;
    const char *remote_addr;
    const char *proxy_host;
    const char *proxy_add_x_forwarded_for;
    const char *request_uri;  // The path and query as the client sent them.
    const char *uri;          // The normalised path.
    const char *args;         // The query, or "" if there's none.
} Variables;

// At a '$': finds the name in $name or ${name}. Returns how many bytes the
// whole reference takes, or 0 if it isn't one.
size_t variable_reference(const char *at, const char **name, size_t *length);

// NULL if there's no such variable.
const char *variable_value(const Variables *variables, const char *name,
                           size_t length);

// Appends text with each $variable's value in its place. Every variable in
// it must already have been checked. Fails only when out of memory.
int expand_variables(Buffer *out, const char *text,
                     const Variables *variables);

#endif
