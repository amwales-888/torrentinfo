/*
 * torrent.h - one torrent, from "start" to "stopped", inside a session.
 *
 * A torrent task takes a .torrent (its bytes) or a magnet link and does
 * everything needed to download and seed it:
 *
 *   magnet link  look for the torrent fetched before (torrents/ in the data
 *                directory); otherwise fetch its info dictionary from peers
 *                found through the link, its trackers and the DHT, and keep
 *                it there for next time
 *   download     choose the files, check what is on disk (fast resume), find
 *                peers through trackers and the DHT, download and seed
 *   stop         tell the trackers we are leaving and save the resume data
 *
 * Tasks are made by a session (session.h: sessionAddTorrent), which owns
 * what they share: the peer and tracker modules, the listening port, the
 * DHT node, the port mapping, speed limits and connection caps; it passes
 * them in a torrentHost, ticks each task's download, and hands it the
 * connections that arrive for it.
 *
 *   torrentStop()    leave politely (a second call: at once)
 *   onEnded          everything has stopped
 *
 * Nothing is printed: what happens is reported through torrentEvents, in
 * enough detail for a program to say it in its own words. Every call must be
 * made on the loop's thread.
 */
#ifndef TORRENT_H
#define TORRENT_H

#include <stdint.h>

#include "bandwidth.h"
#include "blocklist.h"
#include "dht.h"
#include "download.h"
#include "event.h"
#include "metadata.h"
#include "net.h"
#include "parser.h"
#include "portmap.h"
#include "selection.h"
#include "tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (torrentBegin) and results (onEnded). */
#define TORRENT_OK              0
#define TORRENT_EINVAL          (-1)    /* bad arguments                          */
#define TORRENT_ENOMEM          (-2)
#define TORRENT_EPARSE          (-3)    /* the torrent does not parse (detail: the
                                           PARSER_ERROR_*)                        */
#define TORRENT_EMAGNET         (-4)    /* not a usable magnet link               */
#define TORRENT_ENOSOURCES      (-6)    /* a magnet link with no trackers, no
                                           peers, and the DHT off                 */
#define TORRENT_EMETADATA       (-7)    /* the metadata could not be fetched      */
#define TORRENT_ENOPIECE        (-8)    /* onlyPiece is not in the torrent        */
#define TORRENT_ESELECTION      (-9)    /* the file choice is wrong (detail: the
                                           SELECTION_*; onFilesChosen said more)  */
#define TORRENT_EDOWNLOAD       (-10)   /* the download could not start (detail:
                                           the DOWNLOAD_*)                        */
#define TORRENT_ESTOPPED        (-11)   /* torrentStop() before the end           */

/* States, in the order a task goes through them. */
#define TORRENT_STATE_STARTING  0u      /* until the loop runs                    */
#define TORRENT_STATE_METADATA  1u      /* fetching a magnet link's metadata      */
#define TORRENT_STATE_CHECKING  2u      /* checking the files on disk             */
#define TORRENT_STATE_RUNNING   3u      /* downloading or seeding                 */
#define TORRENT_STATE_STOPPING  4u      /* telling the trackers we are leaving    */
#define TORRENT_STATE_ENDED     5u

#define TORRENT_KNOWN_PEERS     500u    /* peers kept from the metadata phase     */
#define TORRENT_PATH_LENGTH     1024u

typedef struct torrentTask torrentTask;

/* What the session lends its tasks; it must outlive them, and the session
 * may fill in dhtNode and the ports once it has started. */
typedef struct torrentHost {
    uint8_t             peerId[PEER_ID_LENGTH];
    uint16_t            port;               /* listening port; 0: not listening   */
    uint16_t            announcePort;       /* told to trackers and the DHT       */
    dht                *dhtNode;            /* or NULL                            */
    uint8_t             trackers;           /* the tracker module is up           */
    const blocklist    *blocklist;          /* or NULL                            */
    const httpConfig   *http;
    const downloadGate *gate;               /* the session's connection caps      */
    const bandwidthBucket *down;            /* the session's speed limits: a new  */
    const bandwidthBucket *up;              /* download waits for its first share */
    /* A task has ended (before its own onEnded). */
    void              (*onTaskEnded)(void *hostArg, torrentTask *task);
    void               *hostArg;
} torrentHost;

/*
 * What to do. Pointers are borrowed: they must stay valid until
 * torrentEnd() (data and magnet only until torrentBegin() returns: they are
 * copied).
 */
typedef struct torrentOptions {
    const uint8_t      *data;               /* a .torrent's bytes, or NULL...      */
    uint64_t            length;
    const char         *magnet;             /* ...a magnet link                    */
    const char         *directory;          /* where the files go ("": here)       */
    uint32_t            handshakeTimeoutSeconds;
    int64_t             onlyPiece;          /* one piece, or -1 for all            */
    uint8_t             seed;               /* keep serving peers once complete    */
    uint8_t             dht;                /* find peers through the DHT (if the
                                               session runs it)                    */
    uint8_t             webSeeds;           /* use the torrent's web seeds         */
    uint8_t             recheck;            /* ignore the resume file once         */
    uint32_t            maxPeers;           /* 0: the download module's defaults   */
    uint32_t            uploadSlots;
    uint64_t            maxDownloadRate;    /* this torrent's own limits, in bytes */
    uint64_t            maxUploadRate;      /* per second; 0: none (the session's
                                               limits apply as well)               */
    const selection    *choice;             /* files to fetch; NULL: all           */
    const char *const  *peers;              /* "address:port" to try first (as a
                                               magnet link's x.pe), or NULL        */
    uint32_t            peerCount;
} torrentOptions;

/* Things worth telling a user, with the numbers to say them. */
#define TORRENT_NOTICE_CACHED           1u  /* path: the torrent fetched before    */
#define TORRENT_NOTICE_METADATA         2u  /* number: bytes of metadata received  */
#define TORRENT_NOTICE_SAVED            3u  /* path: where the torrent was kept    */
#define TORRENT_NOTICE_DHT_FAILED       6u  /* the DHT lookup could not start      */
#define TORRENT_NOTICE_NO_TRACKERS      8u  /* the torrent lists no trackers       */
#define TORRENT_NOTICE_ANNOUNCE_FAILED  9u  /* announcing could not start          */
#define TORRENT_NOTICE_LEAVING          10u /* telling the trackers we are leaving */

typedef struct torrentNotice {
    uint32_t    code;                       /* TORRENT_NOTICE_*                    */
    const char *path;
    uint64_t    number;
    uint64_t    number2;
    int32_t     status;
} torrentNotice;

typedef struct torrentEvents {
    /* The task moved to a new TORRENT_STATE_. */
    void (*onState)(void *privateArg, torrentTask *task, uint32_t state);
    /* See torrentNotice. */
    void (*onNotice)(void *privateArg, torrentTask *task, const torrentNotice *notice);
    /* A report from the download or the metadata fetch (event.h). */
    void (*onEvent)(void *privateArg, torrentTask *task, const eventInfo *event);
    /* A tracker answered, or could not be reached (result NULL); 'event' is a
     * TRACKER_EVENT_. */
    void (*onTracker)(void *privateArg, torrentTask *task, const char *url, uint32_t event,
                      int32_t status, const trackerResult *result);
    /* A DHT lookup finished: peers found, nodes asked, nodes in the table. */
    void (*onDht)(void *privateArg, torrentTask *task, uint32_t peers, uint32_t nodes,
                  uint32_t tableNodes);
    /* The file choice was applied (only with an active selection). */
    void (*onFilesChosen)(void *privateArg, torrentTask *task, int32_t status,
                          const selectionResult *result);
    /* The files are ready: torrentDownload() can say what is on disk. */
    void (*onReady)(void *privateArg, torrentTask *task);
    /* Every wanted piece is on disk (seeding follows with 'seed'). */
    void (*onComplete)(void *privateArg, torrentTask *task);
    /* The download ended (DOWNLOAD_OK, or why not); stopping follows. */
    void (*onFinished)(void *privateArg, torrentTask *task, int32_t status,
                       const char *message);
    /* Everything has stopped: 'result' is TORRENT_OK if the download was
     * complete, else why not ('detail' as the codes say). Call torrentEnd()
     * once uv_run() has returned. */
    void (*onEnded)(void *privateArg, torrentTask *task, int32_t result, int32_t detail);
} torrentEvents;

/* What a task is doing; the parts not yet started are zero. */
typedef struct torrentStatus {
    uint32_t         state;                 /* TORRENT_STATE_*                     */
    uint8_t          hasMetadataProgress;   /* fetching the metadata               */
    metadataProgress metadata;
    uint8_t          hasDownloadProgress;   /* the download has begun              */
    downloadProgress download;
    uint8_t          dhtRunning;
    uint32_t         dhtNodes;              /* in the routing table                */
} torrentStatus;

/*
 * For the session (applications call sessionAddTorrent): set up a task; the
 * work starts when the loop next runs, and every result, failures included,
 * then comes through the events. Returns TORRENT_EINVAL, TORRENT_ENOMEM,
 * TORRENT_EPARSE or TORRENT_EMAGNET (with no task) if it cannot even start.
 * The events are copied.
 */
int32_t torrentBegin(uv_loop_t *loop, parserContext *context, const torrentHost *host,
                     const torrentOptions *options, const torrentEvents *events,
                     void *privateArg, torrentTask **task);

/* For the session: the info hash (known from the start, also for a magnet
 * link), SHA1("req2", info hash) for encrypted connections, the task's
 * shares of the session's speed limits, a tick, and an incoming
 * connection (1 taken, 0 refused). */
const uint8_t *torrentInfoHash(const torrentTask *task);
const uint8_t *torrentReq2Hash(const torrentTask *task);
void torrentShares(torrentTask *task, bandwidthShare **down, bandwidthShare **up);
int  torrentHasDownload(const torrentTask *task);
void torrentTick(torrentTask *task);
int  torrentAcceptIncoming(torrentTask *task, peerConnection *connection,
                           const struct sockaddr *address,
                           const uint8_t infoHash[PARSER_SHA1_LENGTH],
                           peerConnectOptions *options, peerCallbacks *callbacks,
                           void **privateArg);

/* Stop: the first call leaves politely (trackers are told, for up to 5 s);
 * a second call, or one before the download has begun, stops at once.
 * onEnded follows. */
void torrentStop(torrentTask *task);

void torrentGetStatus(const torrentTask *task, torrentStatus *status);
uint32_t torrentState(const torrentTask *task);

/* The torrent, once known (always for a .torrent; for a magnet link once its
 * metadata is in), else NULL. */
const parserTorrent *torrentMetainfo(const torrentTask *task);

/* The download, from onState(TORRENT_STATE_CHECKING) until onEnded, else NULL. */
download *torrentDownload(const torrentTask *task);

/* For the session: release the task, after onEnded, once uv_run() has
 * returned (its handles close in the meantime). */
void torrentEnd(torrentTask *task);

const char *torrentStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* TORRENT_H */
