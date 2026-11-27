/*
 * announce.h - Keeping a torrent's trackers up to date while it runs.
 *
 * announceBegin() announces to every tracker the torrent lists (all of
 * them, all tiers, at once) with event "started", then re-announces each at
 * the interval it asks for, reporting real statistics obtained from the
 * getStats callback. Peers from every answer go to onPeers. A tracker that
 * fails is retried after 1 minute, then 2, 4, ... up to 30 minutes.
 *
 *   announceCompleted()  the download finished: tell every tracker
 *                        ("completed") at once
 *   announceStop()       we are leaving: tell every tracker that knows us
 *                        ("stopped"), then call back
 *   announceEnd()        release everything
 *
 * Uses the tracker module: call trackerInit() first (it sets the peer id,
 * the port peers can reach us on and the network binding), and
 * trackerShutdown() after announceEnd(). The torrent must outlive the
 * announce. Everything runs on the libuv loop thread.
 */
#ifndef ANNOUNCE_H
#define ANNOUNCE_H

#include <stdint.h>
#include <uv.h>

#include "parser.h"
#include "tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timing in seconds; may be overridden at compile time (the tests do). */
#ifndef ANNOUNCE_MIN_INTERVAL
#define ANNOUNCE_MIN_INTERVAL       60u     /* never re-announce more often      */
#endif
#define ANNOUNCE_DEFAULT_INTERVAL   1800u   /* when a tracker does not say       */
#define ANNOUNCE_MAX_INTERVAL       3600u
#ifndef ANNOUNCE_RETRY_SECONDS
#define ANNOUNCE_RETRY_SECONDS      60u     /* first retry after a failure       */
#endif
#define ANNOUNCE_MAX_RETRY_SECONDS  1800u
#define ANNOUNCE_TIMEOUT_SECONDS    30u     /* per announce                      */

#define ANNOUNCE_OK                 0
#define ANNOUNCE_EINVAL             (-3)
#define ANNOUNCE_ENOMEM             (-4)
#define ANNOUNCE_ENOTRACKERS        (-2)    /* the torrent lists no trackers     */

typedef struct announce announce;

/* What the trackers are told. */
typedef struct announceStats {
    uint64_t    uploaded;
    uint64_t    downloaded;
    uint64_t    left;
    uint16_t    port;           /* port peers can reach us on; 0: the tracker
                                   module's (trackerInit())                 */
} announceStats;

typedef struct announceEvents {
    /* Required: the current statistics, asked for before each announce. */
    void (*getStats)(void *privateArg, announceStats *stats);
    /* Peers from a tracker (may repeat ones given before). May be NULL. */
    void (*onPeers)(void *privateArg, const trackerPeer *peers, uint32_t count);
    /* An announce finished (for logging): 'event' is TRACKER_EVENT_*;
     * 'result' is the tracker's answer, or NULL if it could not be sent
     * (then 'result' is NULL and 'status' says why). May be NULL. */
    void (*onResult)(void *privateArg, const char *url, uint32_t event, int32_t status,
                     const trackerResult *result);
} announceEvents;

typedef void (*announceStoppedCallback)(void *privateArg);

/* Start announcing. On ANNOUNCE_OK *handle receives the announce. */
int32_t announceBegin(uv_loop_t *loop, parserContext *context, const parserTorrent *torrent,
                      const announceEvents *events, void *privateArg, announce **handle);

/*
 * As announceBegin(), for a list of tracker URLs and an info hash (a magnet
 * link's). The URL strings must outlive the announce.
 */
int32_t announceBeginUrls(uv_loop_t *loop, parserContext *context,
                          const uint8_t infoHash[PARSER_SHA1_LENGTH], char *const *urls,
                          uint32_t count, const announceEvents *events, void *privateArg,
                          announce **handle);

/* The download has completed: send "completed" to the trackers now. */
void announceCompleted(announce *handle);

/* Ask every tracker for more peers now (minimum interval permitting). */
void announceNow(announce *handle);

/*
 * Stop re-announcing and send "stopped" to every tracker that accepted our
 * "started". 'callback' runs from the loop once they have all answered or
 * 'timeoutSeconds' has passed, whichever is first (and also when no tracker
 * needs telling). Call announceEnd() afterwards (it may be called from the
 * callback).
 */
void announceStop(announce *handle, uint32_t timeoutSeconds, announceStoppedCallback callback,
                  void *privateArg);

/* Release everything; no callbacks run afterwards. Keep running the loop
 * until uv_run() returns so that all memory is released. */
void announceEnd(announce *handle);

#ifdef __cplusplus
}
#endif

#endif /* ANNOUNCE_H */
