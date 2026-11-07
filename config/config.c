#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    size_t used = 0;
    size_t capacity = 0;

    if (!file) {
        return NULL;
    }
    while (1) {
        if (capacity - used < 4096) {
            capacity = capacity ? capacity * 2 : 8192;
            char *grown = realloc(text, capacity);
            if (!grown) {
                free(text);
                fclose(file);
                errno = ENOMEM;
                return NULL;
            }
            text = grown;
        }
        size_t count = fread(text + used, 1, capacity - used - 1, file);
        used += count;
        if (count == 0) {
            break;
        }
    }
    int failed = ferror(file);
    fclose(file);
    if (failed) {
        free(text);
        errno = EIO;
        return NULL;
    }
    text[used] = '\0';
    *length = used;
    return text;
}

int config_load(Config *config, const char *path, char *error,
                size_t error_size) {
    Parser parser;
    size_t length;

    memset(config, 0, sizeof(*config));
    block_settings_init(&config->http);

    char *text = read_file(path, &length);
    if (!text) {
        snprintf(error, error_size, "open() \"%s\" failed (%d: %s)", path,
                 errno, strerror(errno));
        return 0;
    }
    // Each word is copied with a NUL after it, and is no longer than in the
    // file, so twice the file always holds them all.
    config->arena = malloc(2 * length + 1);
    if (!config->arena) {
        free(text);
        snprintf(error, error_size, "out of memory");
        return 0;
    }

    parser.config = config;
    parser.path = path;
    parser.text = text;
    parser.position = 0;
    parser.line = 1;
    parser.arena_used = 0;
    parser.error = error;
    parser.error_size = error_size;

    Scope top = {CONTEXT_MAIN, NULL, NULL, NULL, NULL, NULL};
    int ok = parse_block(&parser, &top) && link_proxy_passes(&parser) &&
             group_servers(&parser);
    free(text);
    if (ok && !inherit_all(config)) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    return ok;
}

static void block_settings_free(BlockSettings *settings) {
    free(settings->proxy_set_headers.items);
    if (settings->access_log.kind == ACCESS_LOG_FILE) {
        close(settings->access_log.fd);
    }
}

void config_free(Config *config) {
    for (size_t i = 0; i < config->port_count; i++) {
        for (size_t j = 0; j < config->ports[i].group_count; j++) {
            free(config->ports[i].groups[j].servers);
        }
        free(config->ports[i].groups);
    }
    free(config->ports);
    for (size_t i = 0; i < config->upstream_count; i++) {
        upstream_free(config->upstreams[i]);
    }
    free(config->upstreams);
    for (size_t i = 0; i < config->server_count; i++) {
        Server *server = &config->servers[i];

        for (size_t j = 0; j < server->location_count; j++) {
            free(server->locations[j].proxy_pass.host);
            free(server->locations[j].proxy_headers.items);
            block_settings_free(&server->locations[j].written);
        }
        free(server->locations);
        free(server->listens);
        free(server->names);
        block_settings_free(&server->written);
    }
    free(config->servers);
    block_settings_free(&config->http);
    free(config->arena);
    memset(config, 0, sizeof(*config));
}
