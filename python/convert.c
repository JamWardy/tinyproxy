#include "convert.h"

#include <string.h>

// ---- From Python to C, checked ----

// As tinyproxy passes on a client's header names: letters, digits and '-'.
static int is_header_name(const char *name) {
    if (!*name) {
        return 0;
    }
    for (const char *c = name; *c; c++) {
        if (!is_letter_or_digit(*c) && *c != '-') {
            return 0;
        }
    }
    return 1;
}

// A line break would let a value start a header of its own.
static int is_header_value(const char *value, Py_ssize_t length) {
    for (Py_ssize_t i = 0; i < length; i++) {
        if (value[i] == '\r' || value[i] == '\n' || value[i] == '\0') {
            return 0;
        }
    }
    return 1;
}

const char *to_latin1(PyObject *text, PyObject **bytes,
                             Py_ssize_t *length, const char *what) {
    if (!PyUnicode_Check(text)) {
        PyErr_Format(PyExc_TypeError, "%s must be a str, not %.100s", what,
                     Py_TYPE(text)->tp_name);
        return NULL;
    }
    *bytes = PyUnicode_AsLatin1String(text);
    if (!*bytes) {
        return NULL;
    }
    *length = PyBytes_GET_SIZE(*bytes);
    return PyBytes_AS_STRING(*bytes);
}

int read_header(PyObject *name, PyObject *value, PyObject **name_bytes,
                       PyObject **value_bytes) {
    Py_ssize_t name_length;
    Py_ssize_t value_length;

    *name_bytes = NULL;
    *value_bytes = NULL;
    const char *name_text = to_latin1(name, name_bytes, &name_length,
                                      "a header's name");
    if (!name_text) {
        return 0;
    }
    if (!is_header_name(name_text)) {
        PyErr_Format(PyExc_ValueError, "invalid header name %R", name);
        return 0;
    }
    const char *value_text = to_latin1(value, value_bytes, &value_length,
                                       "a header's value");
    if (!value_text) {
        return 0;
    }
    if (!is_header_value(value_text, value_length)) {
        PyErr_Format(PyExc_ValueError, "invalid value for header %R", name);
        return 0;
    }
    return 1;
}

int copy_header_pairs(PyObject *pairs, Headers *headers) {
    PyObject *iterator = PyObject_GetIter(pairs);
    PyObject *item;
    int ok = iterator != NULL;

    while (ok && (item = PyIter_Next(iterator))) {
        PyObject *name_bytes = NULL;
        PyObject *value_bytes = NULL;

        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            PyErr_SetString(PyExc_TypeError,
                            "headers must be (name, value) pairs");
            ok = 0;
        } else if (!read_header(PyTuple_GET_ITEM(item, 0),
                                PyTuple_GET_ITEM(item, 1), &name_bytes,
                                &value_bytes)) {
            ok = 0;
        } else if (!headers_add_copy(headers, PyBytes_AS_STRING(name_bytes),
                                     PyBytes_AS_STRING(value_bytes))) {
            PyErr_NoMemory();
            ok = 0;
        }
        Py_XDECREF(name_bytes);
        Py_XDECREF(value_bytes);
        Py_DECREF(item);
    }
    Py_XDECREF(iterator);
    return ok && !PyErr_Occurred();
}

int read_status(PyObject *status, int *out) {
    if (!PyLong_Check(status)) {
        PyErr_SetString(PyExc_TypeError, "a status must be an int");
        return 0;
    }
    long value = PyLong_AsLong(status);
    if (value == -1 && PyErr_Occurred()) {
        return 0;
    }
    if (value < 200 || value > 999) {
        PyErr_Format(PyExc_ValueError, "status %ld isn't from 200 to 999",
                     value);
        return 0;
    }
    *out = (int)value;
    return 1;
}

int fill_reply(HookReply *reply, PyObject *status, PyObject *headers,
               PyObject *body) {
    if (!read_status(status, &reply->status) ||
        !copy_header_pairs(headers, &reply->headers)) {
        return 0;
    }
    if (PyUnicode_Check(body)) {
        Py_ssize_t length;
        const char *text = PyUnicode_AsUTF8AndSize(body, &length);

        if (!text) {
            return 0;
        }
        if (!buffer_append(&reply->body, text, (size_t)length)) {
            PyErr_NoMemory();
            return 0;
        }
        return 1;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(body, &view, PyBUF_SIMPLE) != 0) {
        return 0;
    }
    int ok = buffer_append(&reply->body, view.buf, (size_t)view.len);
    PyBuffer_Release(&view);
    if (!ok) {
        PyErr_NoMemory();
    }
    return ok;
}

// ---- From C to Python ----

PyObject *to_str_or_none(const char *text) {
    if (!text) {
        Py_RETURN_NONE;
    }
    return PyUnicode_DecodeLatin1(text, (Py_ssize_t)strlen(text), NULL);
}

PyObject *header_pairs(const Headers *headers) {
    PyObject *list = PyList_New((Py_ssize_t)headers->count);

    for (size_t i = 0; list && i < headers->count; i++) {
        PyObject *pair =
            Py_BuildValue("(NN)", to_str_or_none(headers->items[i].name),
                          to_str_or_none(headers->items[i].value));
        if (!pair) {
            Py_CLEAR(list);
            break;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, pair);
    }
    return list;
}
