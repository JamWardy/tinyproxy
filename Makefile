CC ?= cc
# glibc hides POSIX calls such as getaddrinfo under -std=c99 unless asked;
# macOS shows them anyway. compile_flags.txt repeats these for clangd.
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -g -D_DEFAULT_SOURCE

SRCS = main.c config.c loop.c conn.c request.c proxy.c chunked.c uri.c \
       route.c upstream.c keepalive.c variables.c error_page.c http.c \
       buffer.c log.c

# Objects and .d files go in build/.
OBJS = $(SRCS:%.c=build/%.o)
DEPS = $(OBJS:.o=.d)

all: tinyproxy

tinyproxy: $(OBJS)
	$(CC) $(OBJS) -o $@

# -MMD -MP write a .d file per object listing the headers it includes,
# so editing a header rebuilds every .c file that uses it.
build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf tinyproxy build

-include $(DEPS)

.PHONY: all clean
