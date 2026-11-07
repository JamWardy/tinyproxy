#include "variables.h"

#include <assert.h>
#include <string.h>

#include "http.h"

static int is_name_char(char c) {
    return is_letter_or_digit(c) || c == '_';
}

size_t variable_reference(const char *at, const char **name,
                          size_t *length) {
    int braced = at[1] == '{';
    const char *start = at + 1 + braced;
    size_t count = 0;

    while (is_name_char(start[count])) {
        count++;
    }
    if (count == 0 || (braced && start[count] != '}')) {
        return 0;
    }
    *name = start;
    *length = count;
    return 1 + (size_t)braced + count + (size_t)braced;
}

static int name_is(const char *name, size_t length, const char *expected) {
    return strlen(expected) == length && memcmp(name, expected, length) == 0;
}

const char *variable_value(const Variables *variables, const char *name,
                           size_t length) {
    if (name_is(name, length, "host")) {
        return variables->host;
    }
    if (name_is(name, length, "remote_addr")) {
        return variables->remote_addr;
    }
    if (name_is(name, length, "proxy_host")) {
        return variables->proxy_host;
    }
    if (name_is(name, length, "proxy_add_x_forwarded_for")) {
        return variables->proxy_add_x_forwarded_for;
    }
    if (name_is(name, length, "request_uri")) {
        return variables->request_uri;
    }
    if (name_is(name, length, "uri")) {
        return variables->uri;
    }
    if (name_is(name, length, "args")) {
        return variables->args;
    }
    return NULL;
}

int expand_variables(Buffer *out, const char *text,
                     const Variables *variables) {
    while (*text) {
        if (*text != '$') {
            size_t run = strcspn(text, "$");
            if (!buffer_append(out, text, run)) {
                return 0;
            }
            text += run;
            continue;
        }

        const char *name;
        size_t length;
        size_t used = variable_reference(text, &name, &length);
        const char *value = used ? variable_value(variables, name, length)
                                 : NULL;
        assert(value);  // The config was checked as it loaded.
        if (!buffer_append_text(out, value)) {
            return 0;
        }
        text += used;
    }
    return 1;
}
