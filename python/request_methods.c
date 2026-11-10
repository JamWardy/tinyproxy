// The request object's methods. Python calls each function here when a hook
// calls request.<name>(...), finding it by name in REQUEST_METHODS at the
// end.

#include "request_object.h"

#include "convert.h"

static PyObject *set_header(PyObject *self, PyObject *args) {
    RequestObject *object = (RequestObject *)self;
    PyObject *name;
    PyObject *value;
    PyObject *name_bytes;
    PyObject *value_bytes;

    if (!PyArg_ParseTuple(args, "UO:set_header", &name, &value) ||
        !check_phase(object, PHASE_ACCESS, "request.set_header")) {
        return NULL;
    }
    // An empty value removes it, as in proxy_set_header.
    PyObject *empty = PyUnicode_FromString("");
    if (!empty) {
        return NULL;
    }
    int ok = read_header(name, value == Py_None ? empty : value, &name_bytes,
                         &value_bytes);
    Py_DECREF(empty);
    if (ok && !headers_set_copy(&object->conn->headers_from_access,
                                PyBytes_AS_STRING(name_bytes),
                                PyBytes_AS_STRING(value_bytes))) {
        PyErr_NoMemory();
        ok = 0;
    }
    Py_XDECREF(name_bytes);
    Py_XDECREF(value_bytes);
    if (!ok) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *respond(PyObject *self, PyObject *args, PyObject *keywords) {
    static char *names[] = {"status", "headers", "body", NULL};
    RequestObject *object = (RequestObject *)self;
    PyObject *status;
    PyObject *headers = NULL;
    PyObject *body = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, keywords, "O|OO:respond", names,
                                     &status, &headers, &body) ||
        !check_phase(object, PHASE_ACCESS, "request.respond")) {
        return NULL;
    }
    if (object->responded) {
        PyErr_SetString(PyExc_RuntimeError,
                        "request.respond was already called");
        return NULL;
    }
    PyObject *no_headers = PyTuple_New(0);
    PyObject *no_body = PyBytes_FromStringAndSize("", 0);
    int ok = no_headers && no_body &&
             fill_reply(object->reply, status, headers ? headers : no_headers,
                        body ? body : no_body);
    Py_XDECREF(no_headers);
    Py_XDECREF(no_body);
    if (!ok) {
        return NULL;
    }
    object->responded = 1;
    Py_RETURN_NONE;
}

PyMethodDef REQUEST_METHODS[] = {
    {"set_header", set_header, METH_VARARGS,
     "In access: sets a header that goes upstream; None removes it."},
    {"respond", (PyCFunction)(void (*)(void))respond,
     METH_VARARGS | METH_KEYWORDS,
     "In access: answers here, with (status, headers=(), body=b'')."},
    {NULL, NULL, 0, NULL},
};
