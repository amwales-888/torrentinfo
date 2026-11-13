/*
 * download.h - Downloading a torrent from many peers at once.
 *
 * A download owns the torrent's storage (storage.h) and its peer
 * connections (peer.h):
 *
 *   downloadBegin()    open the files (resuming from existing data), then
 *                      connect to peers as they are added
 *   downloadAddPeer()  add a peer address (e.g. from a tracker); up to
 *                      maxPeers are connected at once, the rest wait
 *   downloadGetProgress()
 *   downloadEnd()      stop and release everything
 *
 * How pieces are fetched:
 *   - Each unchoked peer keeps up to 'pipeline' block requests in flight.
 *   - Pieces already in progress are finished first; new pieces are chosen
 *     rarest-first (fewest connected peers have them), ties at random.
 *   - Each block remembers which peer sent it. A piece that fails its
 *     SHA-1 check is downloaded again; if one peer sent all of it, that
 *     peer is dropped, otherwise every contributor gets a strike and is
 *     dropped at two.
 *   - When every remaining block has been requested (endgame), idle peers
 *     request outstanding blocks too; when a block arrives, the other
 *     requests for it are cancelled.
 *   - A choke or disconnect returns that peer's outstanding requests to
 *     the pool. A peer with requests but no block for 30 seconds is dropped.
 *   - Verified pieces are written on the thread pool while downloading
 *     continues, and every connected peer is sent a "have".
 *   - We tell a peer we are interested only while it has a piece we need.
 *
 * The torrent passed to downloadBegin() must stay valid until downloadEnd().
 * Memory comes from the allocator in the parserContext. Call peerInit()
 * before downloadBegin(): the download uses the peer module.
 */
#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stdint.h>
#include <uv.h>

#include "parser.h"
#include "peer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define DOWNLOAD_DEFAULT_MAX_PEERS      50u
#define DOWNLOAD_DEFAULT_PIPELINE       16u     /* 16 x 16 KiB = 256 KiB per peer */
#define DOWNLOAD_MAX_PIPELINE           64u
/* Timing; may be overridden at compile time (the tests use short values). */
#ifndef DOWNLOAD_STALL_SECONDS
#define DOWNLOAD_STALL_SECONDS          30u     /* requests out, no block: drop  */
#endif
#ifndef DOWNLOAD_IDLE_SECONDS
#define DOWNLOAD_IDLE_SECONDS           120u    /* no data from anyone: give up  */
#endif
#define DOWNLOAD_MESSAGE_LENGTH         256u

/* Status codes */
#define DOWNLOAD_OK                     0
#define DOWNLOAD_EINVAL                 (-3)
#define DOWNLOAD_ENOMEM                 (-4)
#define DOWNLOAD_EIO                    (-20)   /* storage failed: see the message */
#define DOWNLOAD_ENOPEERS               (-29)   /* no peer left that can help      */
#define DOWNLOAD_ESTALLED               (-30)   /* no data for DOWNLOAD_IDLE_SECONDS */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct download download;

typedef struct downloadOptions {
    uint8_t         peerId[PEER_ID_LENGTH];
    const char     *directory;          /* where to save; "" or NULL = current */
    uint32_t        maxPeers;           /* connections at once; 0 = default    */
    uint32_t        pipeline;           /* requests per peer; 0 = default      */
    uint32_t        handshakeTimeoutSeconds;    /* 0 = no limit                */
    int64_t         onlyPiece;          /* download just this piece, or -1     */
} downloadOptions;

typedef struct downloadProgress {
    uint32_t        pieceCount;         /* pieces in the torrent               */
    uint32_t        wanted;             /* pieces this download fetches        */
    uint32_t        done;               /* of those, on disk                   */
    uint32_t        resumed;            /* pieces already on disk at the start */
    uint64_t        bytesReceived;      /* block data received                 */
    uint64_t        bytesWasted;        /* duplicates and pieces that failed   */
    uint32_t        hashFailures;
    uint32_t        peersConnecting;
    uint32_t        peersConnected;     /* handshake done                      */
    uint32_t        peersUnchoking;     /* of those, unchoking us              */
    uint32_t        peersDownloading;   /* of those, with requests in flight   */
    uint32_t        peersQueued;        /* addresses waiting for a free slot   */
    uint32_t        piecesActive;       /* being assembled now                 */
    uint8_t         endgame;
} downloadProgress;

typedef struct downloadEvents {
    /* The files are ready (status DOWNLOAD_OK) or could not be prepared. */
    void (*onReady)(void *privateArg, download *transfer, int32_t status, const char *message);
    /* Something worth reporting: a peer dropped, a piece failed, endgame. */
    void (*onEvent)(void *privateArg, download *transfer, const char *message);
    /* The download has ended: DOWNLOAD_OK when everything is on disk. Runs
     * once; call downloadEnd() afterwards (it may be called from here). */
    void (*onFinished)(void *privateArg, download *transfer, int32_t status, const char *message);
} downloadEvents;

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Start a download: open the files (hashing existing data to resume); once
 * ready, onReady runs and queued peers are connected. On DOWNLOAD_OK,
 * *transfer receives the handle; release it with downloadEnd().
 */
int32_t downloadBegin(uv_loop_t *loop, parserContext *context, const parserTorrent *torrent,
                      const downloadOptions *options, const downloadEvents *events,
                      void *privateArg, download **transfer);

/* Add a peer (IPv4 or IPv6). Duplicates are ignored. */
int32_t downloadAddPeer(download *transfer, const struct sockaddr *address);

void downloadGetProgress(const download *transfer, downloadProgress *progress);

/* The download's top-level path (file, or directory of a multi-file torrent). */
const char *downloadRoot(const download *transfer);

/*
 * Stop and release everything. No callbacks run afterwards. Safe to call
 * from any download callback. Keep running the loop until uv_run() returns
 * so that all memory is released.
 */
void downloadEnd(download *transfer);

/* Human-readable description of a DOWNLOAD_* status. Never NULL. */
const char *downloadStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* DOWNLOAD_H */
