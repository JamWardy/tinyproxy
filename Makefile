CC ?= cc
# glibc hides POSIX calls such as getaddrinfo under -std=c99 unless asked;
# macOS shows them anyway. compile_flags.txt repeats these, and the -I flags, for clangd.
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -g -D_DEFAULT_SOURCE

DIRS = config core http balance util
CFLAGS += $(DIRS:%=-I%)

SRCS = main.c \
       config/config.c config/parse.c config/directives.c \
       config/upstreams.c config/inherit.c \
       core/loop.c core/conn.c core/route.c \
       http/request.c http/proxy.c http/chunked.c http/uri.c \
       http/error_page.c http/http.c \
       balance/upstream.c balance/hash.c balance/keepalive.c \
       util/buffer.c util/log.c util/variables.c

# Objects and .d files go in build/, in the same folders as their sources.
OBJS = $(SRCS:%.c=build/%.o)
DEPS = $(OBJS:.o=.d)

all: tinyproxy

tinyproxy: $(OBJS)
	$(CC) $(OBJS) -o $@

# -MMD -MP write a .d file per object listing the headers it includes,
# so editing a header rebuilds every .c file that uses it.
build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf tinyproxy build

-include $(DEPS)

.PHONY: all clean
