// The request object's attributes. Python calls each function here when a
// hook reads or sets request.<name>, finding it by name in
// REQUEST_ATTRIBUTES at the end.

#include "request_object.h"

#include "convert.h"

static PyObject *get_method(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    return to_str_or_none(object->conn->request.method);
}

static PyObject *get_path(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    return to_str_or_none(object->conn->request.path);
}

static PyObject *get_query(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    const Request *request = &object->conn->request;
    if (!request->path) {
        Py_RETURN_NONE;
    }
    return to_str_or_none(request->query ? request->query : "");
}

static PyObject *get_remote_addr(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    return to_str_or_none(object->conn->remote_addr);
}

static PyObject *get_headers(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    const Request *request = &object->conn->request;
    Headers headers = {request->headers, request->header_count};
    PyObject *list = header_pairs(&headers);
    PyObject *tuple = list ? PyList_AsTuple(list) : NULL;

    Py_XDECREF(list);
    return tuple;
}

static PyObject *get_status(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    if (object->conn->status == 0) {
        Py_RETURN_NONE;
    }
    return PyLong_FromLong(object->conn->status);
}

static int set_status(PyObject *self, PyObject *value, void *closure) {
    RequestObject *object = (RequestObject *)self;
    int status;

    (void)closure;
    if (!check_phase(object, PHASE_HEADER_FILTER, "changing request.status")) {
        return -1;
    }
    if (!value || !read_status(value, &status)) {
        if (!value) {
            PyErr_SetString(PyExc_TypeError,
                            "request.status can't be deleted");
        }
        return -1;
    }
    object->conn->status = status;
    return 0;
}

static PyObject *get_settings(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    return Py_NewRef(object->settings);
}

static PyObject *get_state(PyObject *self, void *closure) {
    (void)closure;
    return Py_NewRef(((RequestObject *)self)->state);
}

static PyObject *get_reply_headers(PyObject *self, void *closure) {
    RequestObject *object = (RequestObject *)self;

    (void)closure;
    if (!check_not_done(object)) {
        return NULL;
    }
    return Py_NewRef(object->reply_headers);
}

static PyObject *get_responded(PyObject *self, void *closure) {
    (void)closure;
    return PyBool_FromLong(((RequestObject *)self)->responded);
}

PyGetSetDef REQUEST_ATTRIBUTES[] = {
    {"method", get_method, NULL, NULL, NULL},
    {"path", get_path, NULL, "$path: normalised, without the query.", NULL},
    {"query", get_query, NULL, NULL, NULL},
    {"remote_addr", get_remote_addr, NULL, NULL, NULL},
    {"headers", get_headers, NULL, "The request's (name, value) pairs.",
     NULL},
    {"status", get_status, set_status, NULL, NULL},
    {"settings", get_settings, NULL, "python_setting's values.", NULL},
    {"state", get_state, NULL, "For the hooks, from one phase to the next.",
     NULL},
    {"reply_headers", get_reply_headers, NULL, NULL, NULL},
    {"responded", get_responded, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};
