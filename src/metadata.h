/*
 * metadata.h - Fetching a torrent's info dictionary from peers (BEP 9).
 *
 * For a magnet link, all we know is the info hash. Peers that support the
 * extension protocol and "ut_metadata" can send the info dictionary in
 * 16 KiB pieces; once all have arrived, its SHA-1 must equal the info hash,
 * which proves it is the right one whoever sent it.
 *
 *   metadataBegin()    start; peers are connected as they are added
 *   metadataAddPeer()  a peer address (from trackers, DHT, the link itself)
 *   onDone             the dictionary, checked (or the fetch failed)
 *   metadataEnd()      release everything
 *
 * Call peerInit() first: connections use the peer module.
 */
#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>
#include <uv.h>

#include "blocklist.h"
#include "event.h"
#include "parser.h"
#include "peer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define METADATA_DEFAULT_MAX_PEERS  30u
#define METADATA_REQUESTS_PER_PEER  2u
#define METADATA_REQUEST_SECONDS    20u     /* a request unanswered this long:
                                               drop the peer                 */

#define METADATA_OK                 0
#define METADATA_EINVAL             (-3)
#define METADATA_ENOMEM             (-4)

typedef struct metadataFetch metadataFetch;

typedef struct metadataOptions {
    uint8_t     infoHash[PARSER_SHA1_LENGTH];
    uint8_t     peerId[PEER_ID_LENGTH];
    uint32_t    maxPeers;               /* connections at once; 0 = default  */
    uint32_t    handshakeTimeoutSeconds;    /* 0: no limit                   */
    uint16_t    listenPort;             /* told to peers; 0 if not listening */
    const blocklist *blocklist;         /* addresses never to talk to; NULL:
                                           none. Must outlive the fetch.     */
} metadataOptions;

typedef struct metadataProgress {
    uint32_t    peersConnecting;
    uint32_t    peersConnected;
    uint32_t    peersWithMetadata;      /* connected and offering it         */
    uint32_t    peersQueued;
    uint32_t    size;                   /* bytes, once a peer has said       */
    uint32_t    pieces;                 /* 16 KiB pieces of it               */
    uint32_t    piecesDone;
    uint32_t    peersBlocked;           /* refused by the blocklist          */
} metadataProgress;

typedef struct metadataEvents {
    /* Something worth reporting (a peer without metadata, a bad copy). */
    void (*onEvent)(void *privateArg, metadataFetch *fetch, const eventInfo *event);
    /* Done: 'info' (valid during the call only) is the info dictionary,
     * whose SHA-1 is the info hash. Runs once; the connections are closed
     * afterwards. Call metadataEnd() (it may be called from here). */
    void (*onDone)(void *privateArg, metadataFetch *fetch, const uint8_t *info,
                   uint32_t length);
} metadataEvents;

int32_t metadataBegin(uv_loop_t *loop, parserContext *context, const metadataOptions *options,
                      const metadataEvents *events, void *privateArg, metadataFetch **fetch);

/* Add a peer (IPv4 or IPv6); duplicates are ignored. */
int32_t metadataAddPeer(metadataFetch *fetch, const struct sockaddr *address);

void metadataGetProgress(const metadataFetch *fetch, metadataProgress *progress);

/* Stop and release everything; no callbacks afterwards. Safe from callbacks.
 * Keep running the loop until uv_run() returns. */
void metadataEnd(metadataFetch *fetch);

#ifdef __cplusplus
}
#endif

#endif /* METADATA_H */
