#ifndef TINYPROXY_LOG_H
#define TINYPROXY_LOG_H

#include <stddef.h>
#include <time.h>

// One line of nginx's combined format. NULL strings are logged as "-".
typedef struct {
    const char *remote_addr;
    time_t time;
    const char *request_line;
    int status;
    size_t body_bytes_sent;
    const char *referer;
    const char *user_agent;
} AccessEntry;

void access_log_write(int fd, const AccessEntry *entry);

// To stderr, as nginx's error_log: the time, [level], then the message.
void error_log(const char *level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
