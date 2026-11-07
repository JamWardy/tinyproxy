#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "buffer.h"

// As nginx's log escaping: '"', '\' and anything outside printable ASCII
// become \xHH, so a request can't forge a line or break the quoting.
static int append_escaped(Buffer *line, const char *text) {
    if (!text || !*text) {
        return buffer_append(line, "-", 1);
    }
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        int ok = (*c == '"' || *c == '\\' || *c < 0x20 || *c >= 0x7f)
                     ? buffer_appendf(line, "\\x%02X", *c)
                     : buffer_append(line, c, 1);
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

void access_log_write(int fd, const AccessEntry *entry) {
    char when[40];
    struct tm local;
    Buffer line;

    localtime_r(&entry->time, &local);
    strftime(when, sizeof(when), "%d/%b/%Y:%H:%M:%S %z", &local);

    buffer_init(&line);
    int ok = buffer_appendf(&line, "%s - - [%s] \"", entry->remote_addr,
                            when) &&
             append_escaped(&line, entry->request_line) &&
             buffer_appendf(&line, "\" %d %zu \"", entry->status,
                            entry->body_bytes_sent) &&
             append_escaped(&line, entry->referer) &&
             buffer_append_text(&line, "\" \"") &&
             append_escaped(&line, entry->user_agent) &&
             buffer_append_text(&line, "\"\n");

    // One write per line, so lines from O_APPEND writers never interleave.
    if (ok && write(fd, buffer_bytes(&line), buffer_length(&line)) < 0) {
        perror("tinyproxy: access_log");
    }
    buffer_free(&line);
}

void error_log(const char *level, const char *format, ...) {
    char when[32];
    struct tm local;
    time_t now = time(NULL);
    va_list args;

    localtime_r(&now, &local);
    strftime(when, sizeof(when), "%Y/%m/%d %H:%M:%S", &local);

    fprintf(stderr, "%s [%s] ", when, level);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}
