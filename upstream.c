#include "upstream.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int add_peer(Upstream *upstream, const struct sockaddr_in *address) {
    Peer *peers = realloc(upstream->peers,
                          (upstream->peer_count + 1) * sizeof(Peer));
    char text[INET_ADDRSTRLEN];

    if (!peers) {
        return 0;
    }
    upstream->peers = peers;
    Peer *peer = &peers[upstream->peer_count++];
    peer->address = *address;
    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    snprintf(peer->text, sizeof(peer->text), "%s:%d", text,
             ntohs(address->sin_port));
    return 1;
}

AddPeers upstream_add_peers(Upstream *upstream, const char *host,
                            unsigned short port) {
    struct addrinfo hints;
    struct addrinfo *found;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0) {
        return PEERS_NOT_FOUND;
    }

    AddPeers result = PEERS_ADDED;
    for (const struct addrinfo *each = found; each; each = each->ai_next) {
        struct sockaddr_in address;

        memcpy(&address, each->ai_addr, sizeof(address));
        address.sin_port = htons(port);
        if (!add_peer(upstream, &address)) {
            result = PEERS_OUT_OF_MEMORY;
            break;
        }
    }
    freeaddrinfo(found);
    return result;
}

void upstream_free(Upstream *upstream) {
    free(upstream->name);
    free(upstream->peers);
    free(upstream);
}

const Peer *choose_peer(const Upstream *upstream) {
    return &upstream->peers[0];
}
