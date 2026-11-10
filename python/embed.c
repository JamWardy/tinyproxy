#include "request_object.h"

#include "convert.h"

#include "log.h"

static const char *const PHASE_NAMES[PHASE_COUNT] = {
    "init", "access", "balancer", "header_filter", "body_filter", "log",
    "error",
};

static int started = 0;
static PyObject *hooks[PHASE_COUNT];  // NULL for a phase with none.
// What python_print_error writes.
static PyObject *saved_exception = NULL;
static PyTypeObject *endpoint_type = NULL;

const char *phase_name(Phase phase) {
    return phase < PHASE_COUNT ? PHASE_NAMES[phase] : "none";
}

int has_hook(Phase phase) {
    return phase < PHASE_COUNT && hooks[phase] != NULL;
}

static HookResult save_exception(void) {
    Py_XDECREF(saved_exception);
    saved_exception = PyErr_GetRaisedException();
    return HOOK_RAISED;
}

void python_print_error(void) {
    if (saved_exception) {
        PyErr_DisplayException(saved_exception);
        Py_CLEAR(saved_exception);
    }
}

// ---- Starting ----

static int add_path(const Config *config) {
    PyObject *path = PySys_GetObject("path");  // Borrowed.

    if (!config->python_path) {
        return 1;
    }
    PyObject *folder = PyUnicode_DecodeFSDefault(config->python_path);
    int ok = path && folder && PyList_Insert(path, 0, folder) == 0;

    Py_XDECREF(folder);
    return ok;
}

static PyStructSequence_Field ENDPOINT_FIELDS[] = {
    {"address", "\"127.0.0.1:8003\""},
    {"host", "Its host line's name and port, as written."},
    {"weight", NULL},
    {"backup", NULL},
    {"down", NULL},
    {NULL, NULL},
};

static PyStructSequence_Desc ENDPOINT_DESC = {
    "tinyproxy.Endpoint", "One of a pool's endpoints.", ENDPOINT_FIELDS, 5,
};

static int find_hooks(PyObject *module, const char *name) {
    for (int phase = 0; phase < PHASE_COUNT; phase++) {
        PyObject *hook;

        if (PyObject_GetOptionalAttrString(module, PHASE_NAMES[phase],
                                           &hook) < 0) {
            return 0;
        }
        if (hook && !PyCallable_Check(hook)) {
            error_log("emerg", "\"%s.%s\" isn't a function", name,
                      PHASE_NAMES[phase]);
            Py_DECREF(hook);
            return -1;
        }
        hooks[phase] = hook;
    }
    return 1;
}

int python_start(const Config *config) {
    const char *name = config->python_module;

    if (!name) {
        return 1;
    }
    // 0 leaves Python's signal handlers off, so SIGINT still stops the loop.
    Py_InitializeEx(0);
    started = 1;
    endpoint_type = PyStructSequence_NewType(&ENDPOINT_DESC);
    if (!add_path(config) || !request_object_ready() || !endpoint_type) {
        error_log("emerg", "python failed to start");
        save_exception();
        python_print_error();
        return 0;
    }

    PyObject *module = PyImport_ImportModule(name);
    if (!module) {
        error_log("emerg", "python_module \"%s\" failed to import", name);
        save_exception();
        python_print_error();
        return 0;
    }
    int found = find_hooks(module, name);
    Py_DECREF(module);
    if (found == 0) {
        error_log("emerg", "python_module \"%s\" failed", name);
        save_exception();
        python_print_error();
    }
    if (found != 1) {
        return 0;
    }

    if (hooks[PHASE_INIT]) {
        PyObject *result = PyObject_CallNoArgs(hooks[PHASE_INIT]);

        if (!result) {
            error_log("emerg", "python init hook raised");
            save_exception();
            python_print_error();
            return 0;
        }
        Py_DECREF(result);
    }
    return 1;
}

void python_stop(void) {
    if (!started) {
        return;
    }
    for (int phase = 0; phase < PHASE_COUNT; phase++) {
        Py_CLEAR(hooks[phase]);
    }
    Py_CLEAR(saved_exception);
    Py_CLEAR(endpoint_type);
    Py_FinalizeEx();
    started = 0;
}

// ---- Calling them ----

// Calls the phase's hook with the request object and what follows it,
// ending in NULL. NULL, with an exception set, if the object couldn't be
// made or the hook raised.
static PyObject *call_hook(Connection *conn, Phase phase, PyObject *first,
                           PyObject *second) {
    RequestObject *object = request_object_for(conn);

    if (!object) {
        return NULL;
    }
    object->phase = phase;
    PyObject *result = PyObject_CallFunctionObjArgs(
        hooks[phase], (PyObject *)object, first, second, NULL);
    object->phase = PHASE_NONE;
    return result;
}

HookResult run_access(Connection *conn, HookReply *reply) {
    if (!hooks[PHASE_ACCESS]) {
        return HOOK_DONE;
    }
    RequestObject *object = request_object_for(conn);
    if (!object) {
        return save_exception();
    }
    object->reply = reply;
    object->responded = 0;
    PyObject *result = call_hook(conn, PHASE_ACCESS, NULL, NULL);
    object->reply = NULL;
    if (!result) {
        return save_exception();
    }
    Py_DECREF(result);
    return object->responded ? HOOK_ANSWERED : HOOK_DONE;
}

static PyObject *new_endpoint(const Endpoint *endpoint) {
    PyObject *values[5] = {
        PyUnicode_FromString(endpoint->text),
        PyUnicode_FromString(endpoint->host),
        PyLong_FromSize_t(endpoint->weight),
        PyBool_FromLong(endpoint->backup),
        PyBool_FromLong(endpoint->down),
    };
    PyObject *item = PyStructSequence_New(endpoint_type);
    int ok = item != NULL;

    for (int i = 0; i < 5; i++) {
        ok = ok && values[i];
    }
    for (int i = 0; i < 5; i++) {
        if (ok) {
            PyStructSequence_SetItem(item, i, values[i]);  // Takes it.
        } else {
            Py_XDECREF(values[i]);
        }
    }
    if (!ok) {
        Py_XDECREF(item);
        return NULL;
    }
    return item;
}

HookResult run_balancer(Connection *conn, const Upstream *upstream,
                        const Endpoint **chosen) {
    if (!hooks[PHASE_BALANCER]) {
        return HOOK_DONE;
    }
    // A tuple, so the hook can't swap one for something else.
    PyObject *endpoints = PyTuple_New((Py_ssize_t)upstream->endpoint_count);
    for (size_t i = 0; endpoints && i < upstream->endpoint_count; i++) {
        PyObject *item = new_endpoint(&upstream->endpoints[i]);

        if (!item) {
            Py_CLEAR(endpoints);
            break;
        }
        PyTuple_SET_ITEM(endpoints, (Py_ssize_t)i, item);
    }
    if (!endpoints) {
        return save_exception();
    }

    PyObject *result = call_hook(conn, PHASE_BALANCER, endpoints, NULL);
    HookResult outcome = HOOK_DONE;
    if (!result) {
        outcome = save_exception();
    } else if (result != Py_None) {
        outcome = HOOK_RAISED;
        for (size_t i = 0; i < upstream->endpoint_count; i++) {
            if (PyTuple_GET_ITEM(endpoints, (Py_ssize_t)i) == result) {
                *chosen = &upstream->endpoints[i];
                outcome = HOOK_ANSWERED;
            }
        }
        if (outcome == HOOK_RAISED) {
            PyErr_SetString(PyExc_TypeError,
                            "balancer must return one of its endpoints, or "
                            "None");
            save_exception();
        }
    }
    Py_XDECREF(result);
    Py_DECREF(endpoints);
    return outcome;
}

HookResult run_header_filter(Connection *conn, const Headers *reply_headers,
                             Headers *headers) {
    if (!hooks[PHASE_HEADER_FILTER]) {
        return HOOK_DONE;
    }
    RequestObject *object = request_object_for(conn);
    PyObject *list = object ? header_pairs(reply_headers) : NULL;
    if (!list) {
        return save_exception();
    }
    Py_SETREF(object->reply_headers, list);

    PyObject *result = call_hook(conn, PHASE_HEADER_FILTER, NULL, NULL);
    if (!result) {
        return save_exception();
    }
    Py_DECREF(result);
    // As a tuple from here on, so later phases can read it but not change it.
    PyObject *frozen = PySequence_Tuple(object->reply_headers);
    if (!frozen || !copy_header_pairs(frozen, headers)) {
        Py_XDECREF(frozen);
        return save_exception();
    }
    Py_SETREF(object->reply_headers, frozen);
    return HOOK_DONE;
}

HookResult run_body_filter(Connection *conn, const char *data, size_t size,
                           int last, Buffer *out) {
    PyObject *piece = PyBytes_FromStringAndSize(data, (Py_ssize_t)size);

    if (!piece) {
        return save_exception();
    }
    PyObject *result = call_hook(conn, PHASE_BODY_FILTER, piece,
                                 last ? Py_True : Py_False);
    Py_DECREF(piece);
    if (!result) {
        return save_exception();
    }
    Py_buffer view;
    if (PyObject_GetBuffer(result, &view, PyBUF_SIMPLE) != 0) {
        Py_DECREF(result);
        return save_exception();
    }
    int ok = buffer_append(out, view.buf, (size_t)view.len);
    PyBuffer_Release(&view);
    Py_DECREF(result);
    if (!ok) {
        PyErr_NoMemory();
        return save_exception();
    }
    return HOOK_DONE;
}

HookResult run_log(Connection *conn) {
    if (!hooks[PHASE_LOG]) {
        return HOOK_DONE;
    }
    PyObject *result = call_hook(conn, PHASE_LOG, NULL, NULL);
    if (!result) {
        return save_exception();
    }
    Py_DECREF(result);
    return HOOK_DONE;
}

HookResult run_error(Connection *conn, int status, HookReply *reply) {
    if (!hooks[PHASE_ERROR]) {
        return HOOK_DONE;
    }
    PyObject *code = PyLong_FromLong(status);
    if (!code) {
        return save_exception();
    }
    PyObject *result = call_hook(conn, PHASE_ERROR, code, NULL);
    Py_DECREF(code);
    if (!result) {
        return save_exception();
    }
    HookResult outcome = HOOK_DONE;
    if (result != Py_None) {
        if (!PyTuple_Check(result) || PyTuple_GET_SIZE(result) != 3) {
            PyErr_SetString(PyExc_TypeError,
                            "error must return (status, headers, body), or "
                            "None");
            outcome = save_exception();
        } else if (!fill_reply(reply, PyTuple_GET_ITEM(result, 0),
                               PyTuple_GET_ITEM(result, 1),
                               PyTuple_GET_ITEM(result, 2))) {
            outcome = save_exception();
        } else {
            outcome = HOOK_ANSWERED;
        }
    }
    Py_DECREF(result);
    return outcome;
}
