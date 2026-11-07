#ifndef TINYPROXY_LOOP_H
#define TINYPROXY_LOOP_H

#include "config.h"

// Listens on every server's ports and serves until SIGINT or SIGTERM.
// Returns the exit status.
int loop_run(const Config *config);

#endif
