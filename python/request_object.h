#ifndef TINYPROXY_REQUEST_OBJECT_H
#define TINYPROXY_REQUEST_OBJECT_H

// The request object: the request as the hooks see it, as their request
// argument. For python/ only, as it brings in Python.h, which must come
// before any system header.

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "embed.h"

struct RequestObject {
    PyObject_HEAD
    Connection *conn;  // NULL once the request is done.
    Phase phase;       // The hook running, or PHASE_NONE.
    HookReply *reply;  // Where respond puts its reply, while access runs.
    int responded;
    PyObject *state;
    PyObject *settings;  // python_setting's values.
    PyObject *reply_headers;  // header_filter's list, then a tuple; or None.
};

typedef struct RequestObject RequestObject;

// Raises if the request is done, as the connection may already hold the
// next one. Everything but state checks this first.
int check_not_done(const RequestObject *object);

// Raises unless the hook for phase is running; what names the thing used.
int check_phase(const RequestObject *object, Phase phase, const char *what);

// Python looks up request.<name> in these, in request_attributes.c and
// request_methods.c.
extern PyGetSetDef REQUEST_ATTRIBUTES[];
extern PyMethodDef REQUEST_METHODS[];

int request_object_ready(void);

// conn's request object, made the first time a hook needs it. NULL, with an
// exception set, when out of memory.
RequestObject *request_object_for(Connection *conn);

#endif
