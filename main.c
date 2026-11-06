#include <stdio.h>
#include <string.h>

#include "config.h"
#include "loop.h"

static int usage(void) {
    fprintf(stderr, "usage: tinyproxy -c <config-file>\n");
    return 1;
}

int main(int argc, char **argv) {
    Config config;
    char error[1024];

    // There's no install folder for nginx's default conf/nginx.conf.
    if (argc != 3 || strcmp(argv[1], "-c") != 0) {
        return usage();
    }

    if (!config_load(&config, argv[2], error, sizeof(error))) {
        fprintf(stderr, "tinyproxy: [emerg] %s\n", error);
        config_free(&config);
        return 1;
    }
    int status = loop_run(&config);
    config_free(&config);
    return status;
}
