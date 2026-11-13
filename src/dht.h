/*
 * dht.h - The BitTorrent DHT (BEP 5): finding peers without a tracker.
 *
 * Every DHT node has a random 160-bit id. Nodes keep a routing table of
 * other nodes, organised by "distance" (the XOR of two ids), knowing many
 * nodes close to themselves and a few far away. To find peers for a torrent,
 * a node asks the nodes it knows that are closest to the info hash; they
 * answer with peers, or with nodes closer still, until the closest nodes
 * have been asked. It then "announces" itself to those, so others find it.
 *
 * Messages are bencoded dictionaries over UDP (KRPC): queries ping,
 * find_node, get_peers and announce_peer, and their responses. A dht
 * answers others' queries too, and stores the peers announced to it.
 *
 *   dhtBegin()           open the socket, load a saved table, bootstrap
 *   dhtAddTorrent()      find peers for an info hash now and every 15
 *                        minutes (5 while few are found), announcing us
 *   dhtRemoveTorrent()
 *   dhtSaveState()       the table, to load next time (dhtOptions.state)
 *   dhtEnd()
 *
 * IPv4 only (BEP 32 IPv6 is not implemented). Each dht is independent: a
 * program (or a test) may run several. Everything runs on the loop thread.
 */
#ifndef DHT_H
#define DHT_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "blocklist.h"
#include "net.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DHT_ID_LENGTH           20u
#define DHT_BUCKET_SIZE         8u      /* K: nodes per bucket                 */
#define DHT_MAX_STATE           (64u + 26u * 256u)  /* dhtSaveState() needs at most */
#define DHT_MESSAGE_LENGTH      256u

/* Timing (seconds); may be overridden at compile time (the tests do). */
#ifndef DHT_SEARCH_INTERVAL
#define DHT_SEARCH_INTERVAL     900u    /* repeat a torrent's lookup            */
#endif
#ifndef DHT_SEARCH_INTERVAL_FEW
#define DHT_SEARCH_INTERVAL_FEW 300u    /* ...when it found fewer than 8 peers  */
#endif
#define DHT_QUERY_TIMEOUT_MS    3000u

#define DHT_OK                  0
#define DHT_EINVAL              (-3)
#define DHT_ENOMEM              (-4)
#define DHT_EADDRINUSE          (-5)    /* no free UDP port                    */
#define DHT_ERROR               (-6)

typedef struct dht dht;
typedef struct dhtTorrent dhtTorrent;

typedef struct dhtOptions {
    uint16_t            port;           /* UDP port (usually the TCP listen
                                           port); 0 for 6881. The next 9 are
                                           tried if it is taken.               */
    const netBinding   *binding;        /* interface, or NULL (copied)         */
    const uint8_t      *state;          /* from dhtSaveState(), or NULL         */
    size_t              stateLength;
    const char *const  *bootstrap;      /* "host:port" list ending in NULL, or
                                           NULL for the well-known routers     */
    const blocklist    *blocklist;      /* nodes never to talk to, or NULL;
                                           must outlive the dht                */
} dhtOptions;

typedef struct dhtStatus {
    uint16_t    port;                   /* UDP port in use                     */
    uint32_t    nodes;                  /* in the routing table                */
    uint32_t    goodNodes;              /* ...that answered recently           */
    uint32_t    lookups;                /* lookups under way                   */
    uint32_t    storedPeers;            /* peers others announced to us        */
    uint64_t    received;               /* messages                            */
    uint64_t    sent;
    uint64_t    blocked;                /* messages and nodes refused          */
} dhtStatus;

/* Peers found for a torrent (IPv4); may repeat ones given before. */
typedef void (*dhtPeersCallback)(void *privateArg, const struct sockaddr_in *peers,
                                 uint32_t count);

/* A lookup finished: how many peers it found and nodes it asked (for logs). */
typedef void (*dhtSearchDoneCallback)(void *privateArg, uint32_t peers, uint32_t nodes);

int32_t dhtBegin(uv_loop_t *loop, parserContext *context, const dhtOptions *options,
                 dht **handle);

/*
 * Find peers for 'infoHash' now and then periodically; with 'announcePort'
 * (our TCP port, 0 for none) also tell the nodes we are a peer. 'done' may
 * be NULL.
 */
int32_t dhtAddTorrent(dht *node, const uint8_t infoHash[PARSER_SHA1_LENGTH],
                      uint16_t announcePort, dhtPeersCallback callback,
                      dhtSearchDoneCallback done, void *privateArg, dhtTorrent **torrent);

/* Stop looking; no callbacks for it afterwards. */
void dhtRemoveTorrent(dht *node, dhtTorrent *torrent);

void dhtGetStatus(const dht *node, dhtStatus *status);

/* Our id and the best nodes of the table, compactly, into 'out' (at least
 * DHT_MAX_STATE bytes). *length receives the size. */
int32_t dhtSaveState(const dht *node, uint8_t *out, size_t size, size_t *length);

/* Add a node to try (e.g. a peer's BEP 5 "port" message). */
void dhtAddNode(dht *node, const struct sockaddr_in *address);

/* Close and release everything. Keep running the loop until uv_run() returns. */
void dhtEnd(dht *node);

#ifdef __cplusplus
}
#endif

#endif /* DHT_H */
