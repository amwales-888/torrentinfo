/*
 * session.h - many torrents on one loop, sharing what can be shared.
 *
 * A session owns:
 *   - the peer and tracker modules;
 *   - one listening port (IPv4 and IPv6), whose incoming connections go to
 *     the torrent they ask for, plain or encrypted;
 *   - the port forwarding on the router;
 *   - one DHT node, its routing table saved in the data directory;
 *   - session-wide speed limits, divided fairly among the torrents
 *     (bandwidth.h), and caps on open and half-open connections;
 *   - one 100 ms tick for every download.
 *
 * Torrents are added with sessionAddTorrent() and run as torrent tasks
 * (torrent.h), each reporting through its own events.
 *
 *   sessionBegin()       set it up; the work starts once the loop runs
 *   sessionAddTorrent()  any time before sessionStop()
 *   sessionStop()        stop every torrent politely, then the shared parts
 *                        (a second call: at once); onEnded follows
 *   sessionEnd()         release everything, once uv_run() has returned
 *
 * Nothing is printed. One session per process (the peer and tracker modules
 * are per process); every call must be made on the loop's thread.
 */
#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>

#include "bandwidth.h"
#include "blocklist.h"
#include "net.h"
#include "parser.h"
#include "portmap.h"
#include "torrent.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SESSION_OK                  0
#define SESSION_EINVAL              (-1)
#define SESSION_ENOMEM              (-2)
#define SESSION_EPEER               (-3)    /* the peer module could not start   */
#define SESSION_ESTOPPING           (-4)    /* sessionStop() has been called     */
/* sessionAddTorrent() also returns the TORRENT_E* codes of torrentBegin(). */

#define SESSION_DEFAULT_PORT            6881u   /* told to trackers when not listening */
#define SESSION_LISTEN_ATTEMPTS         10u     /* the port asked for, then the next 9 */
#define SESSION_DEFAULT_MAX_CONNECTIONS 500u    /* open peer connections, all torrents */
#define SESSION_DEFAULT_MAX_HALF_OPEN   30u     /* connections being made at once      */

typedef struct session session;

typedef struct sessionOptions {
    uint8_t             peerId[PEER_ID_LENGTH];
    uint16_t            listenPort;         /* 0: do not accept peers              */
    uint8_t             portForwarding;     /* ask the router to forward the port  */
    uint8_t             dht;                /* run a DHT node                      */
    const char *const  *dhtBootstrap;       /* "host:port" list ending in NULL, or
                                               NULL for the well-known routers     */
    const blocklist    *blocklist;          /* or NULL                             */
    uint64_t            maxDownloadRate;    /* for all torrents together, bytes    */
    uint64_t            maxUploadRate;      /* per second; 0: no limit             */
    uint32_t            maxConnections;     /* 0: SESSION_DEFAULT_MAX_CONNECTIONS  */
    uint32_t            maxHalfOpen;        /* 0: SESSION_DEFAULT_MAX_HALF_OPEN    */
    const char         *caFile;             /* for HTTPS; NULL: the system's       */
    const netBinding   *binding;            /* interface, or NULL                  */
} sessionOptions;

/* Things worth telling a user about the shared parts. */
#define SESSION_NOTICE_LISTENING        1u  /* number: the port                    */
#define SESSION_NOTICE_LISTEN_FAILED    2u  /* number..number2: the ports tried    */
#define SESSION_NOTICE_DHT_FAILED       3u  /* the DHT node could not start        */
#define SESSION_NOTICE_TRACKERS_FAILED  4u  /* status: the TRACKER_* from
                                               trackerInit()                       */

typedef struct sessionNotice {
    uint32_t code;                          /* SESSION_NOTICE_*                    */
    uint64_t number;
    uint64_t number2;
    int32_t  status;
} sessionNotice;

typedef struct sessionEvents {
    void (*onNotice)(void *privateArg, session *owner, const sessionNotice *notice);
    /* The router answered about the port forwarding. */
    void (*onPortmap)(void *privateArg, session *owner, const portmapResult *result);
    /* Every torrent and every shared part has stopped. */
    void (*onEnded)(void *privateArg, session *owner);
} sessionEvents;

typedef struct sessionStatus {
    uint16_t port;                          /* listening; 0: not                   */
    uint8_t  dhtRunning;
    uint32_t dhtNodes;                      /* in the routing table                */
    uint32_t torrents;                      /* added                               */
    uint32_t running;                       /* not yet ended                       */
    uint32_t connectionsOpen;               /* all torrents' downloads             */
    uint32_t connectionsConnecting;
    uint8_t  stopping;
} sessionStatus;

/*
 * Set up a session. SESSION_EPEER if the peer module cannot start. The
 * options' pointers are borrowed and must outlive the session; the events
 * are copied.
 */
int32_t sessionBegin(uv_loop_t *loop, parserContext *context, const sessionOptions *options,
                     const sessionEvents *events, void *privateArg, session **owner);

/* Add a torrent (see torrent.h for the options and events); it starts when
 * the loop next runs. */
int32_t sessionAddTorrent(session *owner, const torrentOptions *options,
                          const torrentEvents *events, void *privateArg, torrentTask **task);

/* Change the session-wide speed limits (bytes per second; 0: none). */
void sessionSetRateLimits(session *owner, uint64_t maxDownloadRate, uint64_t maxUploadRate);

void sessionGetStatus(const session *owner, sessionStatus *status);

/* Stop: every torrent leaves politely (trackers are told, for up to 5 s),
 * then the DHT's table is saved and the port forwarding removed. A second
 * call stops every torrent at once. onEnded follows, from the loop, after
 * every torrent's own onEnded. */
void sessionStop(session *owner);

/* Release the session and its torrents: after onEnded, once uv_run() has
 * returned. */
void sessionEnd(session *owner);

const char *sessionStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_H */
