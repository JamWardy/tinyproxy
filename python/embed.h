#ifndef TINYPROXY_EMBED_H
#define TINYPROXY_EMBED_H

// The Python module's hooks, as the rest of tinyproxy calls them. Only the
// files in python/ include Python.h.

#include "buffer.h"
#include "config.h"
#include "conn.h"
#include "error_page.h"
#include "upstream.h"

typedef enum {
    PHASE_INIT,
    PHASE_ACCESS,
    PHASE_BALANCER,
    PHASE_HEADER_FILTER,
    PHASE_BODY_FILTER,
    PHASE_LOG,
    PHASE_ERROR,
    PHASE_COUNT,
    PHASE_NONE = PHASE_COUNT,  // Between hooks.
} Phase;

typedef enum {
    HOOK_DONE,      // It ran without answering, or there's none.
    HOOK_ANSWERED,  // It gave a reply, or an endpoint.
    HOOK_RAISED,    // python_print_error has its traceback.
} HookResult;

// Starts Python, imports the config's python_module and runs its init. With
// no python_module, does nothing. On failure, the error log says why.
int python_start(const Config *config);
void python_stop(void);

int has_hook(Phase phase);

// Its function's name, such as "access".
const char *phase_name(Phase phase);

// Writes the traceback of the hook that raised last to the error log.
void python_print_error(void);

// access(request). A reply from request.respond goes in reply; headers from
// request.set_header go in conn->headers_from_access.
HookResult run_access(Connection *conn, HookReply *reply);

// balancer(request, endpoints). The endpoint it returned goes in *chosen.
HookResult run_balancer(Connection *conn, const Upstream *upstream,
                        const Endpoint **chosen);

// header_filter(request), with reply_headers, the upstream's or tinyproxy's
// own reply's, and conn->status its status. On HOOK_DONE, headers has copies
// of request.reply_headers, and conn->status request.status.
HookResult run_header_filter(Connection *conn, const Headers *reply_headers,
                             Headers *headers);

// body_filter(request, data, last). What it returned is appended to out.
HookResult run_body_filter(Connection *conn, const char *data, size_t size,
                           int last, Buffer *out);

HookResult run_log(Connection *conn);

// error(request, status). A reply it returned goes in reply.
HookResult run_error(Connection *conn, int status, HookReply *reply);

// The request is done: its request object, if a hook had one, keeps only
// its state.
void request_object_done(Connection *conn);

#endif
