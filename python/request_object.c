// The request object itself: its type, which Python needs before making one,
// and its life, one per request.

#include "request_object.h"

#include "convert.h"

int check_not_done(const RequestObject *object) {
    if (!object->conn) {
        PyErr_SetString(PyExc_RuntimeError, "the request is done");
        return 0;
    }
    return 1;
}

int check_phase(const RequestObject *object, Phase phase,
                       const char *what) {
    if (!check_not_done(object)) {
        return 0;
    }
    if (object->phase != phase) {
        PyErr_Format(PyExc_RuntimeError, "%s works only in %s", what,
                     phase_name(phase));
        return 0;
    }
    return 1;
}

// ---- Its life ----

static int traverse(PyObject *self, visitproc visit, void *arg) {
    RequestObject *object = (RequestObject *)self;

    Py_VISIT(object->state);
    Py_VISIT(object->settings);
    Py_VISIT(object->reply_headers);
    return 0;
}

static int clear(PyObject *self) {
    RequestObject *object = (RequestObject *)self;

    Py_CLEAR(object->state);
    Py_CLEAR(object->settings);
    Py_CLEAR(object->reply_headers);
    return 0;
}

static void dealloc(PyObject *self) {
    PyObject_GC_UnTrack(self);
    clear(self);
    PyObject_GC_Del(self);
}

static PyTypeObject REQUEST_OBJECT_TYPE = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tinyproxy.RequestObject",
    .tp_basicsize = sizeof(RequestObject),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "The request, as the hooks see it.",
    .tp_traverse = traverse,
    .tp_clear = clear,
    .tp_dealloc = dealloc,
    .tp_methods = REQUEST_METHODS,
    .tp_getset = REQUEST_ATTRIBUTES,
};

int request_object_ready(void) {
    return PyType_Ready(&REQUEST_OBJECT_TYPE) == 0;
}

static PyObject *python_settings(const Headers *settings) {
    PyObject *dict = PyDict_New();

    for (size_t i = 0; dict && i < settings->count; i++) {
        PyObject *value = to_str_or_none(settings->items[i].value);

        if (!value || PyDict_SetItemString(dict, settings->items[i].name,
                                           value) != 0) {
            Py_CLEAR(dict);
        }
        Py_XDECREF(value);
    }
    return dict;
}

RequestObject *request_object_for(Connection *conn) {
    if (conn->request_object) {
        return conn->request_object;
    }
    RequestObject *object =
        PyObject_GC_New(RequestObject, &REQUEST_OBJECT_TYPE);
    if (!object) {
        return NULL;
    }
    object->conn = conn;
    object->phase = PHASE_NONE;
    object->reply = NULL;
    object->responded = 0;
    object->state = PyDict_New();
    object->settings = python_settings(&conn->settings->python_settings);
    object->reply_headers = Py_NewRef(Py_None);
    PyObject_GC_Track((PyObject *)object);
    if (!object->state || !object->settings) {
        Py_DECREF(object);
        return NULL;
    }
    conn->request_object = object;
    return object;
}

void request_object_done(Connection *conn) {
    RequestObject *object = conn->request_object;

    if (!object) {
        return;
    }
    object->conn = NULL;
    conn->request_object = NULL;
    Py_DECREF(object);
}

