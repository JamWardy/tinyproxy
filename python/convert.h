#ifndef TINYPROXY_CONVERT_H
#define TINYPROXY_CONVERT_H

// Values on their way between C and Python, checked when they come from a
// hook. For python/ only, as it brings in Python.h, which must come before
// any system header.

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "embed.h"

// ---- From C to Python ----

// A str, read as Latin-1 as WSGI reads headers, or None for NULL.
PyObject *to_str_or_none(const char *text);

// A list of (name, value) tuples.
PyObject *header_pairs(const Headers *headers);

// ---- From Python to C, checked ----

// A str as Latin-1. *bytes holds the result; NULL, with an exception set,
// if it isn't a str or has other characters. what names it in the error.
const char *to_latin1(PyObject *text, PyObject **bytes, Py_ssize_t *length,
                      const char *what);

// A header's name and value, checked, as C strings in *name_bytes and
// *value_bytes. Fails with an exception set.
int read_header(PyObject *name, PyObject *value, PyObject **name_bytes,
                PyObject **value_bytes);

// Copies an iterable of (name, value) pairs onto headers, checking each is
// a header. Fails with an exception set.
int copy_header_pairs(PyObject *pairs, Headers *headers);

// An int from 200 to 999, as tinyproxy has no use for an informational
// reply. Fails with an exception set.
int read_status(PyObject *status, int *out);

// A reply given as its status, (name, value) pairs and body. Fails with an
// exception set.
int fill_reply(HookReply *reply, PyObject *status, PyObject *headers,
               PyObject *body);

#endif
