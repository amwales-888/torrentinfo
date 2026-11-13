/*
 * tracker.h - Asynchronous BitTorrent tracker announces using libuv.
 *
 * Supports:
 *   - HTTP trackers (BEP 3), compact (BEP 23) and dictionary peer lists,
 *     plus IPv6 peers (BEP 7 "peers6")
 *   - HTTPS trackers: the same over TLS 1.2+ (OpenSSL), with certificate
 *     and host name verification
 *   - UDP trackers (BEP 15)
 *
 * HTTP and HTTPS go through the http module (http.h) and UDP through the
 * udp module (udp.h); trackerInit() sets both up and trackerShutdown()
 * releases them. OpenSSL allocates through its own
 * hook, CRYPTO_set_mem_functions(), which the application may point at the
 * same allocator if wanted.
 *
 * Model:
 *   trackerConnect() starts an announce and returns at once with a unique
 *   id. When the announce finishes, successfully or not, the callback runs
 *   once from the libuv loop. Call trackerInfo() to get the outcome (from the
 *   callback or at any time later), and trackerClose() when you no longer
 *   need it. trackerList() returns the ids that are still open.
 *
 *   All functions must be called from the thread that runs the libuv loop.
 *   Memory comes from the allocator in the parserContext given to
 *   trackerInit(). (libuv itself has its own allocator hook:
 *   uv_replace_allocator().)
 *
 * Usage:
 *     trackerInit(loop, &context, &config);
 *     trackerConnect(torrent.infoHash, torrent.announce, &id, 15, onDone, me);
 *     uv_run(loop, UV_RUN_DEFAULT);
 *
 *     static void onDone(void *privateArg, uint32_t id) {
 *         trackerResult result;
 *         if (trackerInfo(privateArg, id, &result) == TRACKER_OK) { ... }
 *         trackerClose(id);
 *     }
 *
 *     trackerShutdown();              when finished with all trackers,
 *     uv_run(loop, UV_RUN_DEFAULT);   then let the loop finish closing
 *
 * Building:
 *   Linux:   <uv.h> needs POSIX types, so with -std=c99 define
 *            _POSIX_C_SOURCE=200809L (before any #include, or with -D on the
 *            command line), or use -std=gnu99. Link with
 *            $(pkg-config --cflags --libs libuv openssl). See the Makefile.
 *   Windows: define WIN32_LEAN_AND_MEAN and link libuv, OpenSSL and the
 *            system libraries they need. torrentinfo.sln does all of this.
 */
#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>
#include <uv.h>

#include "http.h"
#include "net.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define TRACKER_PEER_ID_LENGTH          20u
#define TRACKER_DEFAULT_NUM_WANT        50u
#define TRACKER_MAX_URL_LENGTH          2048u
#define TRACKER_MAX_RESPONSE            (1024u * 1024u) /* HTTP response cap  */
#define TRACKER_ERROR_MESSAGE_LENGTH    160u

#define TRACKER_FAMILY_IPV4             4u
#define TRACKER_FAMILY_IPV6             6u

/* Status codes */
#define TRACKER_OK                      0       /* Finished successfully          */
#define TRACKER_PENDING                 1       /* Announce still in progress     */
#define TRACKER_ERROR                   (-1)    /* Network error (DNS, connect,
                                                   send, receive): see message   */
#define TRACKER_ENOENT                  (-2)    /* No such id                     */
#define TRACKER_EINVAL                  (-3)    /* Invalid argument or URL        */
#define TRACKER_ENOMEM                  (-4)    /* Allocator returned NULL        */
#define TRACKER_ETIMEDOUT               (-5)    /* No answer within the timeout   */
#define TRACKER_ENOTSUP                 (-6)    /* Unsupported scheme (https...)  */
#define TRACKER_EFAILURE                (-7)    /* Tracker refused: failureReason */
#define TRACKER_EPROTO                  (-8)    /* Bad or unexpected response     */
#define TRACKER_ENOTINIT                (-9)    /* trackerInit() not called       */
#define TRACKER_ETLS                    (-10)   /* TLS setup, handshake or
                                                   certificate check failed      */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* Announce events */
#define TRACKER_EVENT_NONE          0u      /* a regular re-announce       */
#define TRACKER_EVENT_STARTED       1u      /* the first announce          */
#define TRACKER_EVENT_COMPLETED     2u      /* the download just finished  */
#define TRACKER_EVENT_STOPPED       3u      /* we are leaving the swarm    */

/* What trackerAnnounce() reports. Copied. */
typedef struct trackerAnnounceParams {
    uint32_t    event;          /* TRACKER_EVENT_*                          */
    uint64_t    uploaded;       /* bytes sent to peers since 'started'      */
    uint64_t    downloaded;     /* bytes received from peers since 'started' */
    uint64_t    left;           /* bytes still needed (0: we are a seed)    */
} trackerAnnounceParams;

/* Called once when an announce finishes. Not called after trackerClose(). */
typedef void (*trackerCallback)(void *privateArg, uint32_t uniqueId);

/* Settings shared by every announce. Copied by trackerInit(). */
typedef struct trackerConfig {
    uint8_t     peerId[TRACKER_PEER_ID_LENGTH]; /* Our 20-byte peer id        */
    uint16_t    port;                           /* Port we accept peers on    */
    uint32_t    numWant;                        /* Peers to ask for; 0 = default */
    const char *caFile;     /* PEM file of trusted CA certificates for HTTPS,
                               or NULL for the system's CA bundle. Loaded into
                               memory during trackerInit().                  */
    const netBinding *binding;  /* send announces through this interface
                                   (net.h; copied), or NULL for any         */
} trackerConfig;

/* A peer returned by a tracker. */
typedef struct trackerPeer {
    uint8_t     family;         /* TRACKER_FAMILY_IPV4 or TRACKER_FAMILY_IPV6  */
    uint8_t     address[16];    /* Network byte order; IPv4 uses the first 4   */
    uint16_t    port;           /* Host byte order                             */
} trackerPeer;

/*
 * The outcome of an announce, filled in by trackerInfo(). (Named
 * trackerResult because C does not allow a type and a function to share
 * the name trackerInfo.)
 *
 * Pointers refer to memory owned by the request: they stay valid until
 * trackerClose() for that id, or trackerShutdown().
 */
typedef struct trackerResult {
    int32_t             status;         /* Same value trackerInfo() returns  */
    const char         *url;
    uint8_t             infoHash[PARSER_SHA1_LENGTH];
    uint32_t            interval;       /* Seconds until the next announce   */
    uint32_t            minInterval;    /* 0 if not given                    */
    int64_t             seeders;        /* -1 if not reported                */
    int64_t             leechers;       /* -1 if not reported                */
    const char         *failureReason;  /* From the tracker, or NULL         */
    const char         *warningMessage; /* From the tracker, or NULL         */
    const char         *errorMessage;   /* What went wrong, or NULL on success */
    const trackerPeer  *peers;
    uint32_t            peerCount;
} trackerResult;

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Set up the module. 'loop' and 'context' must outlive all tracker use.
 * Returns TRACKER_ETLS if the TLS context or CA certificates cannot be loaded.
 */
int32_t trackerInit(uv_loop_t *loop, parserContext *context, const trackerConfig *config);

/*
 * Start an announce (event=started) to 'trackerUrl' for 'infoHash'.
 *
 * Returns TRACKER_OK once the request is under way, and stores its id in
 * *uniqueId. Problems found later, including the timeout, are reported
 * through the callback and trackerInfo(). Any other return value means
 * nothing was started and the callback will not run.
 *
 * The URL and info hash are copied. 'callback' may be NULL.
 */
int32_t trackerConnect(const uint8_t infoHash[PARSER_SHA1_LENGTH],
                       const char *trackerUrl, uint32_t *uniqueId,
                       uint32_t timeoutSeconds, trackerCallback callback,
                       void *privateArg);

/*
 * As trackerConnect(), but reporting 'params': the event and our transfer
 * statistics. trackerConnect() is this with event "started", no transfers
 * and a small "left". A "stopped" announce asks for no peers.
 */
int32_t trackerAnnounce(const uint8_t infoHash[PARSER_SHA1_LENGTH], const char *trackerUrl,
                        const trackerAnnounceParams *params, uint32_t *uniqueId,
                        uint32_t timeoutSeconds, trackerCallback callback,
                        void *privateArg);

/*
 * Get the state of an announce. 'privateArg' must be the value given to
 * trackerConnect(); otherwise the id is treated as unknown.
 *
 * Returns TRACKER_PENDING while in progress (only url and infoHash are
 * filled in), TRACKER_OK on success, TRACKER_ENOENT for an unknown id, or
 * the error the announce ended with (result is filled in, including
 * errorMessage).
 */
int32_t trackerInfo(void *privateArg, uint32_t uniqueId, trackerResult *result);

/*
 * Forget an id and free its memory. If the announce is still running it is
 * cancelled and its callback will not run. Safe to call from the callback.
 * Returns TRACKER_OK or TRACKER_ENOENT.
 */
int32_t trackerClose(uint32_t uniqueId);

/*
 * List open ids, oldest first. Up to 'capacity' ids are written to 'ids';
 * *count is set to the total number open, so if *count > capacity, call
 * again with a larger array. 'ids' may be NULL when capacity is 0.
 */
int32_t trackerList(uint32_t *ids, uint32_t capacity, uint32_t *count);

/*
 * Close every id and stop accepting new requests. Network handles close
 * asynchronously: keep running the loop until uv_run() returns so that
 * all memory is released. Call trackerInit() again to reuse the module.
 */
void trackerShutdown(void);

/* Human-readable description of a TRACKER_* status. Never NULL. */
const char *trackerStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* TRACKER_H */
