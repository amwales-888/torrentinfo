/*
 * udp.h - Asynchronous UDP tracker announces (BEP 15) using libuv.
 *
 * One call does the whole exchange:
 *
 *   resolve the host -> connect request -> connect response
 *   -> announce request -> announce response
 *
 * Lost packets are retransmitted after 2, 4, 8 and then every 16 seconds
 * (BEP 15 suggests 15 * 2^n, which is too slow for interactive use; a
 * timeout bounds the total). Replies are matched by transaction id, and the
 * socket is connected so only the tracker's address is accepted.
 *
 * Memory comes from the allocator in the parserContext given to udpInit().
 *
 * Usage:
 *     udpInit(loop, &context);
 *     udpAnnounce("udp://tracker.example:1337", &params, 15, onDone, me, &request);
 *     uv_run(loop, UV_RUN_DEFAULT);
 *
 *     static void onDone(void *privateArg, const udpResponse *response) {
 *         if (response->status == UDP_OK) { ... response->peers ... }
 *     }   // the request is freed after the callback returns
 *
 * Building: as for tracker.h, define _POSIX_C_SOURCE=200809L with -std=c99,
 * and link with libuv. URLs are split with httpUrlParse() from http.h.
 */
#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <uv.h>

#include "net.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define UDP_PEER_ID_LENGTH      20u
#define UDP_ERROR_MESSAGE_LENGTH 160u

#define UDP_FAMILY_IPV4         4u      /* 6-byte compact peers  */
#define UDP_FAMILY_IPV6         6u      /* 18-byte compact peers */

/* Announce events (BEP 15 values) */
#define UDP_EVENT_NONE          0u
#define UDP_EVENT_COMPLETED     1u
#define UDP_EVENT_STARTED       2u
#define UDP_EVENT_STOPPED       3u

/* Status codes (the same values as the matching TRACKER_* codes) */
#define UDP_OK                  0       /* Announce answered               */
#define UDP_ERROR               (-1)    /* Network error: see message      */
#define UDP_EINVAL              (-3)    /* Invalid argument or URL         */
#define UDP_ENOMEM              (-4)    /* Allocator returned NULL         */
#define UDP_ETIMEDOUT           (-5)    /* No answer in time               */
#define UDP_ENOTSUP             (-6)    /* URL scheme is not udp           */
#define UDP_EFAILURE            (-7)    /* Tracker refused: failureReason  */
#define UDP_EPROTO              (-8)    /* Unusable reply                  */
#define UDP_ENOTINIT            (-9)    /* udpInit() not called            */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* What to announce. Copied by udpAnnounce(). */
typedef struct udpAnnounceParams {
    uint8_t     infoHash[PARSER_SHA1_LENGTH];
    uint8_t     peerId[UDP_PEER_ID_LENGTH];
    uint64_t    downloaded;
    uint64_t    left;
    uint64_t    uploaded;
    uint32_t    event;          /* UDP_EVENT_*                               */
    uint32_t    key;            /* random value identifying this client      */
    int32_t     numWant;        /* peers wanted; -1 lets the tracker decide  */
    uint16_t    port;           /* port we accept peers on                   */
} udpAnnounceParams;

/* What a finished announce produced. Valid only during the callback. */
typedef struct udpResponse {
    int32_t         status;         /* UDP_OK or a UDP_E* code               */
    uint32_t        interval;       /* seconds until the next announce       */
    uint32_t        leechers;
    uint32_t        seeders;
    const uint8_t  *peers;          /* compact peers: address then port, in
                                       network byte order                   */
    uint64_t        peersLength;    /* bytes                                 */
    uint8_t         peerFamily;     /* UDP_FAMILY_IPV4 or UDP_FAMILY_IPV6:
                                       follows the family used to reach the
                                       tracker                              */
    const char     *failureReason;  /* tracker's error text, or NULL         */
    const char     *errorMessage;   /* NULL when status is UDP_OK            */
} udpResponse;

/* Opaque request handle. */
typedef struct udpRequest udpRequest;

/* Called exactly once per request unless it is cancelled first. */
typedef void (*udpCallback)(void *privateArg, const udpResponse *response);

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Set up the module. Calls are counted, so several users may each call
 * udpInit() and udpShutdown(). Returns UDP_EINVAL if a later call passes a
 * different loop or context.
 */
int32_t udpInit(uv_loop_t *loop, parserContext *context, const netBinding *binding);

/*
 * Start an announce to a udp:// URL (a port is required; any path is
 * ignored). On UDP_OK the request is under way, *request (if not NULL)
 * receives its handle, and the callback will run once from the libuv loop.
 * Any other return value means nothing was started.
 *
 * timeoutSeconds: 0 for no timeout (retransmits continue until cancelled).
 * The handle is valid until the callback runs or udpCancel() is called.
 */
int32_t udpAnnounce(const char *url, const udpAnnounceParams *params,
                    uint32_t timeoutSeconds, udpCallback callback,
                    void *privateArg, udpRequest **request);

/* Stop an announce that is still in progress; its callback will not run. */
void udpCancel(udpRequest *request);

/*
 * Release one udpInit(). The last call cancels every announce still in
 * progress (their callbacks do not run). Keep running the loop until
 * uv_run() returns so that all memory is released.
 */
void udpShutdown(void);

/* Human-readable description of a UDP_* status. Never NULL. */
const char *udpStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* UDP_H */
