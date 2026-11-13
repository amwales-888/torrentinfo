/*
 * download.c - Downloading a torrent from many peers at once.
 *
 * See download.h for the behaviour. Everything here runs on the libuv loop
 * thread: peer callbacks, storage callbacks and a one-second timer.
 *
 * Bookkeeping:
 *   pieceState[]     NEEDED, ACTIVE (being assembled), WRITING, DONE
 *   availability[]   how many connected peers have each piece
 *   active list      pieces being assembled; per block, how many peers have
 *                    an outstanding request for it and who delivered it
 *   per peer         its have-bitfield, its outstanding requests, and how
 *                    many pieces it has that we still need ("interesting")
 *
 * Peer records live until the download is released, so pointers to them
 * (the block contributors) never dangle; a closed peer is only marked so.
 * Everything is released from the timer's close callback, so a callback
 * that calls downloadEnd() never pulls memory out from under the code
 * that called it.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "download.h"
#include "extension.h"
#include "piece.h"
#include "storage.h"
#include "queue.h"

/* ------------------------------------------------------------------------ */
/* Internal constants                                                       */
/* ------------------------------------------------------------------------ */

#define DOWNLOAD_PIECE_NEEDED       0u
#define DOWNLOAD_PIECE_ACTIVE       1u
#define DOWNLOAD_PIECE_WRITING      2u
#define DOWNLOAD_PIECE_DONE         3u      /* on disk, or not wanted */

#define DOWNLOAD_PEER_QUEUED        0u
#define DOWNLOAD_PEER_CONNECTING    1u
#define DOWNLOAD_PEER_OPEN          2u
#define DOWNLOAD_PEER_CLOSED        3u      /* final */

#define DOWNLOAD_MAX_KNOWN_PEERS    2000u   /* addresses remembered            */
#define DOWNLOAD_MAX_ATTEMPTS       3u      /* connections per address         */
#ifndef DOWNLOAD_RETRY_SECONDS
#define DOWNLOAD_RETRY_SECONDS      30u     /* before reconnecting a lost peer */
#endif
#ifndef DOWNLOAD_CHOKED_SECONDS
#define DOWNLOAD_CHOKED_SECONDS     60u     /* choked this long while others
                                               wait for a slot: make room      */
#endif
#define DOWNLOAD_MAX_STRIKES        2u      /* shared hash failures: drop      */
#define DOWNLOAD_MAX_UNEXPECTED     256u    /* stray blocks before dropping    */
#define DOWNLOAD_ENDGAME_COPIES     2u      /* requests per block in endgame   */
#define DOWNLOAD_ENDGAME_DELAY_MS   1500u   /* ...once the first is this old   */
#define DOWNLOAD_MAX_ACTIVE_BYTES   (128u * 1024u * 1024u)
#define DOWNLOAD_TICK_MS            1000u
#define DOWNLOAD_RATE_TICK_MS       100u    /* refilling the speed limits      */
#define DOWNLOAD_UPLOAD_QUEUE       256u    /* requests a peer may queue with us */
#define DOWNLOAD_UPLOAD_READS       4u      /* block reads in flight per peer    */
#define DOWNLOAD_UPLOAD_BUFFERED    (256u * 1024u)  /* queued to send: wait     */
#define DOWNLOAD_RECHOKE_SECONDS    10u
#define DOWNLOAD_OPTIMISTIC_SECONDS 30u
#define DOWNLOAD_INCOMING_EXTRA     10u     /* incoming peers beyond maxPeers    */
#define DOWNLOAD_INCOMING_TIMEOUT   30u     /* seconds for their handshake       */
#ifndef DOWNLOAD_PEX_SECONDS
#define DOWNLOAD_PEX_SECONDS        60u     /* between PEX messages to a peer    */
#endif
#define DOWNLOAD_PEX_FIRST_SECONDS  10u     /* the first, after connecting       */
#define DOWNLOAD_PEX_MIN_RECEIVED   20u     /* ignore PEX more often than this   */
#define DOWNLOAD_PEX_TRACKED        128u    /* peers we remember telling about   */
#define DOWNLOAD_CLIENT_NAME        "torrentinfo 0.7"
#ifndef DOWNLOAD_NOPEERS_SECONDS
#define DOWNLOAD_NOPEERS_SECONDS    60u     /* no peer at all this long: give up */
#endif

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

struct downloadPeer;

typedef struct activePiece {
    TAILQ_ENTRY(activePiece) entries;
    pieceAssembly           piece;
    struct downloadPeer    *owner;          /* the peer that started it     */
    uint8_t                *requests;       /* outstanding requests, per block */
    uint64_t               *requestedAt;    /* uv_now() of each block's first */
    struct downloadPeer   **contributor;    /* who delivered each block     */
    uint8_t                 exclusive;      /* failed once: one peer at a time */
    uint8_t                 verifying;      /* SHA-1 check on the thread pool */
    struct download        *owningDownload;
    int32_t                 verifyResult;
    uv_work_t               verifyWork;
    uint8_t                *failedData;     /* the copy that failed, kept to */
    struct downloadPeer   **failedBy;       /* find the culprit once it passes */
} activePiece;

typedef struct downloadRequest {
    activePiece    *piece;
    uint32_t        block;
} downloadRequest;

typedef struct uploadRequest {
    uint32_t        index;
    uint32_t        begin;
    uint32_t        length;
} uploadRequest;

typedef struct downloadPeer {
    TAILQ_ENTRY(downloadPeer) entries;
    download               *owner;
    struct sockaddr_storage address;
    char                    where[72];      /* "1.2.3.4:6881" or "[::1]:6881" */
    peerConnection         *connection;
    uint8_t                 state;          /* DOWNLOAD_PEER_*              */
    uint8_t                 choked;         /* the peer is choking us       */
    uint8_t                 amInterested;
    uint8_t                 excluded;       /* misbehaved: never again      */
    uint8_t                 complete;       /* a seed, dropped as we are one */
    uint8_t                 strikes;
    uint8_t                 attempts;       /* connections made             */
    uint32_t                unexpected;     /* blocks we did not ask it for */
    uint8_t                *have;           /* its pieces (bitfield)        */
    uint32_t                interesting;    /* of those, NEEDED or ACTIVE   */
    downloadRequest        *requests;       /* outstanding                  */
    uint32_t                requestCount;
    uint64_t                retryAt;        /* uv_now() for a queued retry  */
    uint64_t                lastBlock;      /* uv_now(): block, or first request */
    uint64_t                chokedSince;
    uint64_t                openedAt;

    /* Uploading to the peer */
    uint8_t                 incoming;       /* it connected to us           */
    uint8_t                 peerInterested; /* it wants our pieces          */
    uint8_t                 amChoking;      /* we are choking it            */
    uint8_t                 selected;       /* scratch, for rechoke()       */
    uint8_t                 remoteId[PEER_ID_LENGTH];
    uint32_t                haveCount;      /* pieces it has                */
    uploadRequest          *uploadQueue;    /* its requests, oldest first   */
    uint32_t                queueCount;
    uploadRequest          *reading;        /* blocks being read from disk  */
    uint32_t                readingCount;
    uint64_t                uploaded;       /* total sent to it             */
    uint64_t                uploadedWindow; /* sent since the last rechoke  */
    uint64_t                uploadTurn;     /* when last served, under a limit */
    uint64_t                pumpPass;       /* pumpAll() pass that skipped it  */
    uint64_t                downloadedWindow;   /* received since then      */

    /* Extension protocol and peer exchange */
    uint8_t                 extended;       /* both support BEP 10          */
    uint8_t                 pexId;          /* its id for ut_pex; 0: none   */
    uint8_t                 metadataId;     /* its id for ut_metadata       */
    uint16_t                remotePort;     /* the port it accepts on, if told */
    uint64_t                nextPex;        /* uv_now() of our next PEX to it */
    uint64_t                lastPexIn;      /* uv_now() of its last PEX to us */
    struct downloadPeer   **advertised;     /* peers we have told it about  */
    uint32_t                advertisedCount;
    char                    client[EXTENSION_CLIENT_LENGTH];
} downloadPeer;

TAILQ_HEAD(activeList, activePiece);
TAILQ_HEAD(peerList, downloadPeer);

/* A speed limit: a token bucket of bytes, refilled continuously. */
typedef struct rateLimit {
    uint64_t    rate;                   /* bytes per second; 0: no limit */
    int64_t     tokens;                 /* may dip one block below zero  */
    uint64_t    updated;                /* uv_now() of the last refill   */
} rateLimit;

struct download {
    uv_loop_t              *loop;
    parserContext          *context;
    const parserTorrent    *torrent;
    downloadEvents          events;
    void                   *privateArg;
    storage                *store;
    uv_timer_t              timer;
    peerConnectOptions      connectOptions;
    uint32_t                maxPeers;
    uint32_t                pipeline;
    int64_t                 onlyPiece;

    uint8_t                *pieceState;
    uint32_t               *availability;
    struct activeList       active;
    uint32_t                activeCount;
    uint64_t                activeBytes;
    uint32_t                neededCount;    /* pieces in state NEEDED       */

    struct peerList         peers;
    uint32_t                peerCount;      /* known addresses              */
    uint32_t                connecting;
    uint32_t                open;
    uint32_t                unchoking;

    uint32_t                wanted;
    uint32_t                done;
    uint32_t                resumed;
    uint64_t                bytesReceived;
    uint64_t                bytesWasted;
    uint32_t                hashFailures;
    uint64_t                lastProgress;   /* uv_now() of the last block   */
    uint64_t                random;         /* xorshift state               */

    /* Uploading and seeding */
    uint8_t                *onDisk;         /* pieces we can serve (bitfield) */
    uint32_t                onDiskCount;
    uint64_t                left;           /* wanted bytes not yet on disk */
    uint64_t                bytesUploaded;
    peerListener           *listeners[2];   /* IPv4, IPv6                   */
    uint16_t                listenPort;
    uint8_t                 seedOption;
    uint8_t                 seeding;
    uint32_t                uploadSlots;
    uint32_t                uploadUnchoked; /* peers we are not choking     */
    downloadPeer           *optimistic;
    uint64_t                lastRechoke;
    uint64_t                lastOptimistic;
    uint64_t                noPeersSince;   /* 0 while there are peers      */
    uint8_t                 pexEnabled;     /* not for private torrents     */
    const uint8_t          *info;           /* raw info dictionary, or NULL */
    uint32_t                infoLength;
    uint32_t                pexLearned;     /* new peers heard of by PEX    */
    const blocklist        *blocklist;
    uint32_t                blocked;        /* addresses refused            */

    rateLimit               downLimit;      /* requests we send            */
    rateLimit               upLimit;        /* blocks we read to send      */
    uint64_t                uploadTurns;    /* counter for uploadTurn      */
    uint64_t                pumpPasses;     /* counter for pumpPass        */
    uint64_t                lastTick;       /* uv_now() of the last 1 s tick */

    uint32_t                verifyJobs;     /* piece checks on the thread pool */
    uint8_t                 timerClosed;    /* waiting only for those to end */

    uint8_t                 ready;          /* storage open                 */
    uint8_t                 endgame;
    uint8_t                 finished;       /* onFinished has run           */
    uint8_t                 ended;          /* downloadEnd() called         */
    char                    message[DOWNLOAD_MESSAGE_LENGTH];   /* for onFinished */
    char                    event[DOWNLOAD_MESSAGE_LENGTH];     /* for onEvent    */
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static void *allocate(download *dl, size_t size)
{
    void *memory = dl->context->allocator.realloc(NULL, size);

    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static void release(download *dl, void *memory)
{
    if (memory != NULL) {
        dl->context->allocator.free(memory);
    }
}

static uint64_t nextRandom(download *dl)
{
    uint64_t x = dl->random;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    dl->random = x;
    return x;
}

/* The download should stop doing anything. */
static int stopped(const download *dl)
{
    return dl->finished || dl->ended;
}

#if defined(__GNUC__)
static void emit(download *dl, const char *format, ...) __attribute__((format(printf, 2, 3)));
#endif

static void emit(download *dl, const char *format, ...)
{
    va_list arguments;

    if (stopped(dl) || dl->events.onEvent == NULL) {
        return;
    }
    va_start(arguments, format);
    vsnprintf(dl->event, sizeof(dl->event), format, arguments);
    va_end(arguments);
    dl->events.onEvent(dl->privateArg, dl, dl->event);
}

static void formatAddress(const struct sockaddr *address, char *out, size_t size)
{
    char host[48];

    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;

        if (uv_ip6_name(v6, host, sizeof(host)) != 0) {
            snprintf(host, sizeof(host), "?");
        }
        snprintf(out, size, "[%s]:%u", host, (unsigned)ntohs(v6->sin6_port));
    } else {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;

        if (uv_ip4_name(v4, host, sizeof(host)) != 0) {
            snprintf(host, sizeof(host), "?");
        }
        snprintf(out, size, "%s:%u", host, (unsigned)ntohs(v4->sin_port));
    }
}

static int sameAddress(const struct sockaddr_storage *a, const struct sockaddr *b)
{
    if (a->ss_family != b->sa_family) {
        return 0;
    }
    if (b->sa_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)a;
        const struct sockaddr_in *y = (const struct sockaddr_in *)b;

        return x->sin_port == y->sin_port &&
               memcmp(&x->sin_addr, &y->sin_addr, sizeof(x->sin_addr)) == 0;
    } else {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)b;

        return x->sin6_port == y->sin6_port &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
    }
}

static int pieceIsWanted(const download *dl, uint32_t index)
{
    uint8_t state = dl->pieceState[index];

    return state == DOWNLOAD_PIECE_NEEDED || state == DOWNLOAD_PIECE_ACTIVE;
}

static uint32_t pieceLengthOf(const parserTorrent *torrent, uint32_t index)
{
    if (index + 1u == torrent->pieceCount) {
        return (uint32_t)(torrent->totalLength - (uint64_t)index * torrent->pieceLength);
    }
    return (uint32_t)torrent->pieceLength;
}

static uint32_t blockLength(const activePiece *piece, uint32_t block)
{
    uint32_t begin = block * PIECE_BLOCK_LENGTH;
    uint32_t left = piece->piece.length - begin;

    return (left < PIECE_BLOCK_LENGTH) ? left : PIECE_BLOCK_LENGTH;
}

/* Up to half a second of data may be sent at once, and at least one block. */
static void rateSet(rateLimit *limit, uint64_t rate, uint64_t now)
{
    uint64_t burst = rate / 2u;

    limit->rate = rate;
    limit->tokens = (int64_t)((burst < PIECE_BLOCK_LENGTH) ? PIECE_BLOCK_LENGTH : burst);
    limit->updated = now;
}

/* May another block go now? */
static int rateAvailable(download *dl, rateLimit *limit)
{
    uint64_t now;
    uint64_t burst;

    if (limit->rate == 0) {
        return 1;
    }
    now = uv_now(dl->loop);
    if (now > limit->updated) {
        burst = limit->rate / 2u;
        if (burst < PIECE_BLOCK_LENGTH) {
            burst = PIECE_BLOCK_LENGTH;
        }
        limit->tokens += (int64_t)(limit->rate * (now - limit->updated) / 1000u);
        if (limit->tokens > (int64_t)burst) {
            limit->tokens = (int64_t)burst;
        }
        limit->updated = now;
    }
    return limit->tokens > 0;
}

static void rateCharge(rateLimit *limit, uint32_t bytes)
{
    if (limit->rate != 0) {
        limit->tokens -= (int64_t)bytes;
    }
}

/* Send a message; 0 on success. */
static int sendFrame(downloadPeer *peer, const messageFrame *frame)
{
    if (peer->connection == NULL || peer->state != DOWNLOAD_PEER_OPEN) {
        return -1;
    }
    return (peerSend(peer->connection, frame) == PEER_OK) ? 0 : -1;
}

static void sendSimple(downloadPeer *peer, uint32_t id)
{
    messageFrame frame;

    memset(&frame, 0, sizeof(frame));
    frame.id = id;
    (void)sendFrame(peer, &frame);
}

static void sendBlockMessage(downloadPeer *peer, uint32_t id, const activePiece *piece,
                             uint32_t block)
{
    messageFrame frame;

    memset(&frame, 0, sizeof(frame));
    frame.id = id;
    frame.index = piece->piece.index;
    frame.begin = block * PIECE_BLOCK_LENGTH;
    frame.length = blockLength(piece, block);
    (void)sendFrame(peer, &frame);
}

/* ------------------------------------------------------------------------ */
/* Forward declarations                                                     */
/* ------------------------------------------------------------------------ */

static void fillPeer(download *dl, downloadPeer *peer);
static int sameHost(const struct sockaddr_storage *a, const struct sockaddr *b);
static void fillAll(download *dl);
static void connectMore(download *dl);
static void finish(download *dl, int32_t status, const char *message);
static void complete(download *dl);

/* ------------------------------------------------------------------------ */
/* Requests                                                                 */
/* ------------------------------------------------------------------------ */

/* Forget request 'slot' of 'peer'; the block returns to the pool if no other
 * peer has asked for it. */
static void dropRequestSlot(downloadPeer *peer, uint32_t slot, int returnBlock)
{
    downloadRequest *request = &peer->requests[slot];
    activePiece     *piece = request->piece;

    if (piece->requests[request->block] > 0) {
        piece->requests[request->block]--;
    }
    if (returnBlock && piece->requests[request->block] == 0) {
        pieceCancelBlock(&piece->piece, request->block * PIECE_BLOCK_LENGTH);
    }
    peer->requests[slot] = peer->requests[peer->requestCount - 1u];
    peer->requestCount--;
}

/* All of a peer's requests back to the pool (choked or gone). */
static void releaseRequests(downloadPeer *peer)
{
    while (peer->requestCount > 0) {
        dropRequestSlot(peer, peer->requestCount - 1u, 1);
    }
}

static int findRequest(const downloadPeer *peer, const activePiece *piece, uint32_t block)
{
    uint32_t i;

    for (i = 0; i < peer->requestCount; i++) {
        if (peer->requests[i].piece == piece && peer->requests[i].block == block) {
            return (int)i;
        }
    }
    return -1;
}

/* Record and send a request; 0 on success, -1 if the peer was dropped. */
static void dropPeer(downloadPeer *peer, const char *reason, int exclude);

static int sendRequest(download *dl, downloadPeer *peer, activePiece *piece, uint32_t block)
{
    if (peer->requestCount == 0) {
        peer->lastBlock = uv_now(dl->loop);         /* the stall clock starts now */
    }
    peer->requests[peer->requestCount].piece = piece;
    peer->requests[peer->requestCount].block = block;
    peer->requestCount++;
    if (piece->requests[block]++ == 0) {
        piece->requestedAt[block] = uv_now(dl->loop);
    }
    rateCharge(&dl->downLimit, blockLength(piece, block));
    sendBlockMessage(peer, MESSAGE_REQUEST, piece, block);
    if (peer->state != DOWNLOAD_PEER_OPEN) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Active pieces                                                            */
/* ------------------------------------------------------------------------ */

static void freeActive(download *dl, activePiece *piece)
{
    pieceEnd(&piece->piece);
    release(dl, piece->requests);
    release(dl, piece->requestedAt);
    release(dl, (void *)piece->contributor);
    release(dl, piece->failedData);
    release(dl, (void *)piece->failedBy);
    release(dl, piece);
}

/* Cancel every outstanding request for the piece (endgame duplicates). */
static void cancelPieceRequests(download *dl, activePiece *piece)
{
    downloadPeer *peer;
    uint32_t      i;

    TAILQ_FOREACH(peer, &dl->peers, entries) {
        for (i = peer->requestCount; i > 0; i--) {
            if (peer->requests[i - 1u].piece == piece) {
                sendBlockMessage(peer, MESSAGE_CANCEL, piece, peer->requests[i - 1u].block);
                dropRequestSlot(peer, i - 1u, 0);
            }
        }
    }
}

/* Remove a piece from the active list (completed). */
static void retireActive(download *dl, activePiece *piece)
{
    cancelPieceRequests(dl, piece);
    TAILQ_REMOVE(&dl->active, piece, entries);
    dl->activeCount--;
    dl->activeBytes -= piece->piece.length;
    freeActive(dl, piece);
}

static int canStartPiece(const download *dl, uint32_t length)
{
    uint32_t limit = 2u * dl->unchoking + 2u;

    if (dl->activeCount == 0) {
        return 1;
    }
    return dl->activeCount < limit &&
           dl->activeBytes + length <= (uint64_t)DOWNLOAD_MAX_ACTIVE_BYTES;
}

/* The rarest NEEDED piece the peer has (ties at random), or -1. */
static int64_t pickPiece(download *dl, const downloadPeer *peer)
{
    uint32_t pieceCount = dl->torrent->pieceCount;
    uint32_t best = UINT32_MAX;
    uint32_t ties = 0;
    int64_t  chosen = -1;
    uint32_t i;

    for (i = 0; i < pieceCount; i++) {
        uint32_t count;

        if (dl->pieceState[i] != DOWNLOAD_PIECE_NEEDED || !messageBitfieldGet(peer->have, i)) {
            continue;
        }
        count = dl->availability[i];
        if (count < best) {
            best = count;
            ties = 1;
            chosen = (int64_t)i;
        } else if (count == best) {
            ties++;
            if (nextRandom(dl) % ties == 0) {      /* reservoir sampling */
                chosen = (int64_t)i;
            }
        }
    }
    return chosen;
}

static activePiece *startPiece(download *dl, downloadPeer *peer, uint32_t index)
{
    activePiece *piece = allocate(dl, sizeof(*piece));
    int32_t      status;

    if (piece == NULL) {
        return NULL;
    }
    status = pieceBegin(&piece->piece, dl->context, dl->torrent, index);
    if (status != PIECE_OK) {
        release(dl, piece);
        return NULL;
    }
    piece->requests = allocate(dl, piece->piece.blockCount);
    piece->requestedAt = allocate(dl, sizeof(*piece->requestedAt) * piece->piece.blockCount);
    piece->contributor = allocate(dl, sizeof(*piece->contributor) * piece->piece.blockCount);
    if (piece->requests == NULL || piece->requestedAt == NULL || piece->contributor == NULL) {
        freeActive(dl, piece);
        return NULL;
    }
    piece->owner = peer;
    TAILQ_INSERT_TAIL(&dl->active, piece, entries);
    dl->activeCount++;
    dl->activeBytes += piece->piece.length;
    dl->pieceState[index] = DOWNLOAD_PIECE_ACTIVE;
    dl->neededCount--;
    if (dl->neededCount == 0 && !dl->endgame) {
        dl->endgame = 1;
        emit(dl, "endgame: every remaining piece is in progress");
    }
    return piece;
}

/* Request a missing block of 'piece' from the peer. 1 if one was sent,
 * 0 if none is missing, -1 if the peer was dropped. */
static int requestFrom(download *dl, downloadPeer *peer, activePiece *piece)
{
    messageFrame frame;

    if (pieceNextRequest(&piece->piece, &frame) != PIECE_OK) {
        return 0;
    }
    return (sendRequest(dl, peer, piece, frame.begin / PIECE_BLOCK_LENGTH) == 0) ? 1 : -1;
}

/* Send one more request to the peer. 1 if sent, 0 if there is nothing it can
 * supply, -1 if the peer was dropped. Each piece is kept with one peer where
 * possible, so that a corrupt piece can be blamed on whoever sent it. */
static int requestOne(download *dl, downloadPeer *peer)
{
    activePiece *piece;
    int64_t      index;
    int          sent;
    uint32_t     block;
    uint64_t     now;

    /* 1. The peer's own pieces. */
    TAILQ_FOREACH(piece, &dl->active, entries) {
        if (piece->owner == peer && (sent = requestFrom(dl, peer, piece)) != 0) {
            return sent;
        }
    }
    /* 2. Pieces whose peer has gone: take them over. */
    TAILQ_FOREACH(piece, &dl->active, entries) {
        if (piece->owner == NULL && messageBitfieldGet(peer->have, piece->piece.index)) {
            piece->owner = peer;
            if ((sent = requestFrom(dl, peer, piece)) != 0) {
                return sent;
            }
        }
    }
    /* 3. A new piece, rarest first. */
    if (dl->neededCount > 0) {
        index = pickPiece(dl, peer);
        if (index >= 0 && canStartPiece(dl, pieceLengthOf(dl->torrent, (uint32_t)index))) {
            piece = startPiece(dl, peer, (uint32_t)index);
            if (piece == NULL) {
                finish(dl, DOWNLOAD_ENOMEM, "out of memory starting a piece");
                return -1;
            }
            return requestFrom(dl, peer, piece);
        }
    }
    /* 4. Help with other peers' pieces. */
    TAILQ_FOREACH(piece, &dl->active, entries) {
        if (!piece->exclusive && piece->owner != peer &&
            messageBitfieldGet(peer->have, piece->piece.index) &&
            (sent = requestFrom(dl, peer, piece)) != 0) {
            return sent;
        }
    }
    if (dl->neededCount > 0) {
        return 0;
    }
    /* 5. Endgame: ask for blocks requested from someone else a while ago. */
    now = uv_now(dl->loop);
    TAILQ_FOREACH(piece, &dl->active, entries) {
        if (piece->exclusive || !messageBitfieldGet(peer->have, piece->piece.index)) {
            continue;
        }
        for (block = 0; block < piece->piece.blockCount; block++) {
            if (piece->piece.blocks[block] == PIECE_BLOCK_REQUESTED &&
                piece->requests[block] < DOWNLOAD_ENDGAME_COPIES &&
                now - piece->requestedAt[block] >= DOWNLOAD_ENDGAME_DELAY_MS &&
                findRequest(peer, piece, block) < 0) {
                return (sendRequest(dl, peer, piece, block) == 0) ? 1 : -1;
            }
        }
    }
    return 0;
}

static int canRequest(const download *dl, const downloadPeer *peer)
{
    return !stopped(dl) && dl->ready && peer->state == DOWNLOAD_PEER_OPEN && !peer->choked &&
           !peer->excluded && peer->requestCount < dl->pipeline;
}

/* Keep the peer's pipeline full, as far as the download limit allows. */
static void fillPeer(download *dl, downloadPeer *peer)
{
    while (canRequest(dl, peer) && rateAvailable(dl, &dl->downLimit)) {
        if (requestOne(dl, peer) <= 0) {
            return;
        }
    }
}

static void fillAll(download *dl)
{
    downloadPeer *peer;
    int           sent;

    if (dl->downLimit.rate == 0) {
        TAILQ_FOREACH(peer, &dl->peers, entries) {
            if (stopped(dl)) {
                return;
            }
            fillPeer(dl, peer);
        }
        return;
    }
    /* Limited: one request per peer per round, so they share the allowance. */
    do {
        sent = 0;
        TAILQ_FOREACH(peer, &dl->peers, entries) {
            if (stopped(dl) || !rateAvailable(dl, &dl->downLimit)) {
                return;
            }
            if (canRequest(dl, peer) && requestOne(dl, peer) > 0) {
                sent = 1;
            }
        }
    } while (sent);
}

/* ------------------------------------------------------------------------ */
/* Interest and availability                                                */
/* ------------------------------------------------------------------------ */

static void updateInterest(downloadPeer *peer)
{
    if (peer->state != DOWNLOAD_PEER_OPEN) {
        return;
    }
    if (peer->interesting > 0 && !peer->amInterested) {
        peer->amInterested = 1;
        sendSimple(peer, MESSAGE_INTERESTED);
    } else if (peer->interesting == 0 && peer->amInterested) {
        peer->amInterested = 0;
        sendSimple(peer, MESSAGE_NOT_INTERESTED);
    }
}

static void peerHasPiece(download *dl, downloadPeer *peer, uint32_t index)
{
    if (index >= dl->torrent->pieceCount || messageBitfieldGet(peer->have, index)) {
        return;
    }
    messageBitfieldSet(peer->have, index);
    peer->haveCount++;
    dl->availability[index]++;
    if (dl->ready && pieceIsWanted(dl, index)) {
        peer->interesting++;
    }
}

/* Recount what the peer has that we need (after the storage opens). */
static void recountInteresting(download *dl, downloadPeer *peer)
{
    uint32_t i;

    peer->interesting = 0;
    for (i = 0; i < dl->torrent->pieceCount; i++) {
        if (messageBitfieldGet(peer->have, i) && pieceIsWanted(dl, i)) {
            peer->interesting++;
        }
    }
}

/* A piece is no longer needed: peers that have it are that much less useful. */
static void pieceNoLongerWanted(download *dl, uint32_t index)
{
    downloadPeer *peer;

    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (peer->state == DOWNLOAD_PEER_OPEN && peer->have != NULL &&
            messageBitfieldGet(peer->have, index) && peer->interesting > 0) {
            peer->interesting--;
            updateInterest(peer);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Uploading                                                                */
/* ------------------------------------------------------------------------ */

static void setChoking(download *dl, downloadPeer *peer, int choke)
{
    if (peer->state != DOWNLOAD_PEER_OPEN || peer->amChoking == (uint8_t)(choke != 0)) {
        return;
    }
    peer->amChoking = (uint8_t)(choke != 0);
    if (choke) {
        peer->queueCount = 0;                       /* BEP 3: choking discards requests */
        dl->uploadUnchoked--;
        sendSimple(peer, MESSAGE_CHOKE);
    } else {
        dl->uploadUnchoked++;
        sendSimple(peer, MESSAGE_UNCHOKE);
    }
}

static void onBlockRead(void *privateArg, storage *store, uint32_t pieceIndex, uint32_t begin,
                        uint32_t length, uint8_t *data, int32_t status, const char *errorMessage);

/* Read up to 'most' queued requests from disk while the peer's send queue
 * has room and the upload limit allows. The number started. */
static uint32_t pumpSome(download *dl, downloadPeer *peer, uint32_t most)
{
    peerRemoteState state;
    uploadRequest   request;
    uint8_t        *data;
    uint32_t        started = 0;

    while (started < most && !stopped(dl) && dl->store != NULL &&
           peer->state == DOWNLOAD_PEER_OPEN &&
           !peer->amChoking && peer->queueCount > 0 &&
           peer->readingCount < DOWNLOAD_UPLOAD_READS && rateAvailable(dl, &dl->upLimit)) {
        if (peerGetState(peer->connection, &state) != PEER_OK ||
            state.sendQueued >= DOWNLOAD_UPLOAD_BUFFERED) {
            return started;                         /* onDrain will call again */
        }
        request = peer->uploadQueue[0];
        peer->queueCount--;
        memmove(peer->uploadQueue, peer->uploadQueue + 1,
                sizeof(*peer->uploadQueue) * peer->queueCount);
        data = dl->context->allocator.realloc(NULL, request.length);
        if (data == NULL) {
            return started;
        }
        peer->reading[peer->readingCount++] = request;
        rateCharge(&dl->upLimit, request.length);
        peer->uploadTurn = ++dl->uploadTurns;
        started++;
        if (storageReadBlock(dl->store, request.index, request.begin, request.length, data,
                             onBlockRead, peer) != STORAGE_OK) {
            peer->readingCount--;                   /* storage freed the buffer */
        }
    }
    return started;
}

static int canPump(const download *dl, const downloadPeer *peer)
{
    return peer->state == DOWNLOAD_PEER_OPEN && !peer->amChoking && peer->queueCount > 0 &&
           peer->readingCount < DOWNLOAD_UPLOAD_READS && dl->store != NULL;
}

/* Limited uploads: each block the limit allows goes to the waiting peer
 * served longest ago, so peers share the allowance evenly. */
static void pumpAll(download *dl)
{
    downloadPeer *peer;
    downloadPeer *best;
    uint64_t      pass = ++dl->pumpPasses;

    while (!stopped(dl) && rateAvailable(dl, &dl->upLimit)) {
        best = NULL;
        TAILQ_FOREACH(peer, &dl->peers, entries) {
            if (peer->pumpPass != pass && canPump(dl, peer) &&
                (best == NULL || peer->uploadTurn < best->uploadTurn)) {
                best = peer;
            }
        }
        if (best == NULL) {
            return;
        }
        if (pumpSome(dl, best, 1u) == 0) {
            best->pumpPass = pass;                  /* send queue full: the others */
        }
    }
}

static void pumpUploads(download *dl, downloadPeer *peer)
{
    if (dl->upLimit.rate != 0) {
        pumpAll(dl);                                /* fair shares, not first come */
    } else {
        (void)pumpSome(dl, peer, UINT32_MAX);
    }
}

static void onBlockRead(void *privateArg, storage *store, uint32_t pieceIndex, uint32_t begin,
                        uint32_t length, uint8_t *data, int32_t status, const char *errorMessage)
{
    downloadPeer *peer = privateArg;
    download     *dl = peer->owner;
    messageFrame  frame;
    uint32_t      i;
    int           found = 0;

    (void)store;
    /* Still wanted? A read from before a disconnect or a choke is not. */
    for (i = 0; i < peer->readingCount; i++) {
        if (peer->reading[i].index == pieceIndex && peer->reading[i].begin == begin &&
            peer->reading[i].length == length) {
            peer->reading[i] = peer->reading[--peer->readingCount];
            found = 1;
            break;
        }
    }
    if (data == NULL) {
        if (found && !stopped(dl)) {
            emit(dl, "could not read piece %u for %s: %s", (unsigned)pieceIndex, peer->where,
                 (errorMessage != NULL) ? errorMessage : storageStatusString(status));
        }
        return;
    }
    if (!found || stopped(dl) || peer->state != DOWNLOAD_PEER_OPEN || peer->amChoking) {
        release(dl, data);
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.id = MESSAGE_PIECE;
    frame.index = pieceIndex;
    frame.begin = begin;
    frame.payload = data;
    frame.payloadLength = length;
    if (sendFrame(peer, &frame) == 0) {
        peer->uploaded += length;
        peer->uploadedWindow += length;
        dl->bytesUploaded += length;
    }
    release(dl, data);
    pumpUploads(dl, peer);
}

static void queueUpload(download *dl, downloadPeer *peer, const messageFrame *message)
{
    if (peer->amChoking || !messageBitfieldGet(dl->onDisk, message->index) ||
        (uint64_t)message->begin + message->length >
            pieceLengthOf(dl->torrent, message->index) ||
        peer->queueCount >= DOWNLOAD_UPLOAD_QUEUE) {
        return;                                     /* ignored, as BEP 3 allows */
    }
    peer->uploadQueue[peer->queueCount].index = message->index;
    peer->uploadQueue[peer->queueCount].begin = message->begin;
    peer->uploadQueue[peer->queueCount].length = message->length;
    peer->queueCount++;
    pumpUploads(dl, peer);
}

static void cancelUpload(downloadPeer *peer, const messageFrame *message)
{
    uint32_t i;

    for (i = 0; i < peer->queueCount; i++) {
        if (peer->uploadQueue[i].index == message->index &&
            peer->uploadQueue[i].begin == message->begin &&
            peer->uploadQueue[i].length == message->length) {
            peer->queueCount--;
            memmove(peer->uploadQueue + i, peer->uploadQueue + i + 1,
                    sizeof(*peer->uploadQueue) * (peer->queueCount - i));
            return;
        }
    }
}

static int canUpload(const downloadPeer *peer)
{
    return peer->state == DOWNLOAD_PEER_OPEN && peer->peerInterested && !peer->excluded;
}

/*
 * Choose whom to upload to: the best uploadSlots - 1 interested peers (by
 * what they sent us lately, or while seeding by what we sent them), plus
 * one optimistic unchoke that rotates every DOWNLOAD_OPTIMISTIC_SECONDS.
 */
static void rechoke(download *dl, uint64_t now)
{
    downloadPeer *peer;
    downloadPeer *best;
    uint32_t      regular = (dl->uploadSlots > 1u) ? dl->uploadSlots - 1u : 1u;
    uint32_t      chosen;
    uint32_t      ties;

    TAILQ_FOREACH(peer, &dl->peers, entries) {
        peer->selected = 0;
    }
    for (chosen = 0; chosen < regular; chosen++) {
        uint64_t bestScore = 0;

        best = NULL;
        TAILQ_FOREACH(peer, &dl->peers, entries) {
            uint64_t score;

            if (!canUpload(peer) || peer->selected) {
                continue;
            }
            score = (dl->seeding ? peer->uploadedWindow : peer->downloadedWindow) * 2u +
                    (peer->amChoking ? 0u : 1u);   /* ties: keep who we have */
            if (best == NULL || score > bestScore) {
                best = peer;
                bestScore = score;
            }
        }
        if (best == NULL) {
            break;
        }
        best->selected = 1;
    }

    /* The optimistic unchoke: keep it for its period unless it went away. */
    if (dl->optimistic != NULL &&
        (!canUpload(dl->optimistic) || dl->optimistic->selected ||
         now - dl->lastOptimistic >= (uint64_t)DOWNLOAD_OPTIMISTIC_SECONDS * 1000u)) {
        dl->optimistic = NULL;
    }
    if (dl->optimistic == NULL) {
        ties = 0;
        TAILQ_FOREACH(peer, &dl->peers, entries) {
            if (canUpload(peer) && !peer->selected && nextRandom(dl) % ++ties == 0) {
                dl->optimistic = peer;
            }
        }
        dl->lastOptimistic = now;
    }
    if (dl->optimistic != NULL) {
        dl->optimistic->selected = 1;
    }

    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (stopped(dl)) {
            return;
        }
        if (peer->state == DOWNLOAD_PEER_OPEN) {
            setChoking(dl, peer, !peer->selected);
            peer->uploadedWindow = 0;
            peer->downloadedWindow = 0;
            pumpUploads(dl, peer);
        }
    }
    dl->lastRechoke = now;
}

/* Give free upload slots to interested peers that are waiting. */
static void fillUploadSlots(download *dl)
{
    downloadPeer *peer;

    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (stopped(dl) || dl->uploadUnchoked >= dl->uploadSlots) {
            return;
        }
        if (canUpload(peer) && peer->amChoking) {
            setChoking(dl, peer, 0);
        }
    }
}

static int someoneWaiting(const download *dl)
{
    const downloadPeer *peer;

    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (canUpload(peer) && peer->amChoking) {
            return 1;
        }
    }
    return 0;
}

/* Our bitfield, as the first message after the handshake. */
static void sendBitfield(download *dl, downloadPeer *peer)
{
    messageFrame frame;

    if (dl->onDiskCount == 0) {
        return;                                     /* allowed to say nothing */
    }
    memset(&frame, 0, sizeof(frame));
    frame.id = MESSAGE_BITFIELD;
    frame.payload = dl->onDisk;
    frame.payloadLength = messageBitfieldBytes(dl->torrent->pieceCount);
    (void)sendFrame(peer, &frame);
}

/* ------------------------------------------------------------------------ */
/* Extension protocol and peer exchange                                     */
/* ------------------------------------------------------------------------ */

static void sendExtended(downloadPeer *peer, const uint8_t *payload, size_t length)
{
    messageFrame frame;

    memset(&frame, 0, sizeof(frame));
    frame.id = EXTENSION_MESSAGE_ID;
    frame.payload = payload;
    frame.payloadLength = (uint32_t)length;
    (void)sendFrame(peer, &frame);
}

static void sendExtendedHandshake(download *dl, downloadPeer *peer)
{
    extensionHandshake handshake;
    uint8_t            buffer[1 + 256];
    size_t             length = 0;

    memset(&handshake, 0, sizeof(handshake));
    handshake.pexId = dl->pexEnabled ? (uint8_t)EXTENSION_PEX_ID : 0u;
    if (dl->info != NULL) {
        handshake.metadataId = (uint8_t)EXTENSION_METADATA_ID;
        handshake.metadataSize = dl->infoLength;
    }
    handshake.port = dl->listenPort;
    handshake.requestQueue = DOWNLOAD_UPLOAD_QUEUE;
    snprintf(handshake.client, sizeof(handshake.client), "%s", DOWNLOAD_CLIENT_NAME);
    buffer[0] = (uint8_t)EXTENSION_HANDSHAKE;
    if (extensionEncodeHandshake(&handshake, buffer + 1, sizeof(buffer) - 1u, &length) ==
        EXTENSION_OK) {
        sendExtended(peer, buffer, 1u + length);
    }
}

/* Where a connected peer accepts connections, if we know: an incoming
 * peer's address has a temporary port, so only what it told us counts. */
static int listenAddress(const downloadPeer *peer, extensionPeer *out)
{
    uint16_t port;

    memset(out, 0, sizeof(*out));
    if (peer->address.ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)&peer->address;

        out->family = 4u;
        memcpy(out->address, &v4->sin_addr, 4);
        port = ntohs(v4->sin_port);
    } else {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&peer->address;

        out->family = 6u;
        memcpy(out->address, &v6->sin6_addr, 16);
        port = ntohs(v6->sin6_port);
    }
    out->port = peer->incoming ? peer->remotePort : port;
    return out->port != 0;
}

static int advertisedIndex(const downloadPeer *peer, const downloadPeer *other)
{
    uint32_t i;

    for (i = 0; i < peer->advertisedCount; i++) {
        if (peer->advertised[i] == other) {
            return (int)i;
        }
    }
    return -1;
}

/* Tell the peer which peers we have connected to, and dropped, since last time. */
static void sendPex(download *dl, downloadPeer *peer)
{
    extensionPeer added[EXTENSION_MAX_PEX_PEERS];
    extensionPeer dropped[EXTENSION_MAX_PEX_PEERS];
    downloadPeer *addedPeer[EXTENSION_MAX_PEX_PEERS];
    downloadPeer *other;
    uint8_t       buffer[1 + EXTENSION_PEX_BUFFER];
    uint32_t      addedCount = 0;
    uint32_t      droppedCount = 0;
    uint32_t      i;
    size_t        length = 0;

    /* Dropped: told about before, no longer connected. */
    for (i = peer->advertisedCount; i > 0 && droppedCount < EXTENSION_MAX_PEX_PEERS; i--) {
        other = peer->advertised[i - 1u];
        if (other->state != DOWNLOAD_PEER_OPEN || other->excluded) {
            if (listenAddress(other, &dropped[droppedCount])) {
                droppedCount++;
            }
            peer->advertised[i - 1u] = peer->advertised[--peer->advertisedCount];
        }
    }
    /* Added: connected, with a known address, not told about yet. */
    TAILQ_FOREACH(other, &dl->peers, entries) {
        if (addedCount >= EXTENSION_MAX_PEX_PEERS ||
            peer->advertisedCount + addedCount >= DOWNLOAD_PEX_TRACKED) {
            break;
        }
        if (other == peer || other->state != DOWNLOAD_PEER_OPEN || other->excluded ||
            advertisedIndex(peer, other) >= 0 || !listenAddress(other, &added[addedCount])) {
            continue;
        }
        added[addedCount].flags =
            (uint8_t)((other->haveCount >= dl->torrent->pieceCount ? EXTENSION_PEX_SEED : 0u) |
                      (other->incoming ? 0u : EXTENSION_PEX_CONNECTABLE));
        addedPeer[addedCount++] = other;
    }
    for (i = 0; i < addedCount; i++) {
        peer->advertised[peer->advertisedCount++] = addedPeer[i];
    }
    if (addedCount == 0 && droppedCount == 0) {
        return;
    }
    buffer[0] = peer->pexId;
    if (extensionEncodePex(added, addedCount, dropped, droppedCount, buffer + 1,
                           sizeof(buffer) - 1u, &length) == EXTENSION_OK) {
        sendExtended(peer, buffer, 1u + length);
    }
}

typedef struct pexContext {
    download     *dl;
    uint32_t      learned;
} pexContext;

static void onPexPeer(void *privateArg, const extensionPeer *peer, int added)
{
    pexContext             *context = privateArg;
    struct sockaddr_storage address;
    uint32_t                before = context->dl->peerCount;

    if (!added) {
        return;                                     /* we keep our own view */
    }
    memset(&address, 0, sizeof(address));
    if (peer->family == 4u) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&address;

        v4->sin_family = AF_INET;
        memcpy(&v4->sin_addr, peer->address, 4);
        v4->sin_port = htons(peer->port);
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;

        v6->sin6_family = AF_INET6;
        memcpy(&v6->sin6_addr, peer->address, 16);
        v6->sin6_port = htons(peer->port);
    }
    (void)downloadAddPeer(context->dl, (const struct sockaddr *)&address);
    if (context->dl->peerCount > before) {
        context->learned++;
    }
}

/* A peer asks for a piece of the info dictionary (it has a magnet link). */
static void sendMetadata(download *dl, downloadPeer *peer, const uint8_t *data, size_t length)
{
    uint8_t  buffer[1 + 64 + EXTENSION_METADATA_PIECE];
    uint32_t type;
    uint32_t piece;
    uint32_t totalSize;
    uint32_t begin;
    uint32_t size;
    size_t   offset;
    size_t   header;

    if (peer->metadataId == 0 ||
        extensionDecodeMetadata(data, length, &type, &piece, &totalSize, &offset) !=
            EXTENSION_OK ||
        type != EXTENSION_METADATA_REQUEST) {
        return;
    }
    buffer[0] = peer->metadataId;
    begin = piece * EXTENSION_METADATA_PIECE;
    if (dl->info == NULL || piece >= (dl->infoLength + EXTENSION_METADATA_PIECE - 1u) /
                                         EXTENSION_METADATA_PIECE) {
        if (extensionEncodeMetadata(EXTENSION_METADATA_REJECT, piece, 0, buffer + 1, 64,
                                    &header) == EXTENSION_OK) {
            sendExtended(peer, buffer, 1u + header);
        }
        return;
    }
    size = (dl->infoLength - begin < EXTENSION_METADATA_PIECE) ? dl->infoLength - begin
                                                                : EXTENSION_METADATA_PIECE;
    if (extensionEncodeMetadata(EXTENSION_METADATA_DATA, piece, dl->infoLength, buffer + 1, 64,
                                &header) == EXTENSION_OK) {
        memcpy(buffer + 1 + header, dl->info + begin, size);
        sendExtended(peer, buffer, 1u + header + size);
    }
}

static void receiveExtended(download *dl, downloadPeer *peer, const messageFrame *message)
{
    extensionHandshake handshake;
    pexContext         context;
    uint64_t           now = uv_now(dl->loop);

    if (!peer->extended || message->payloadLength < 1u) {
        return;
    }
    if (message->payload[0] == EXTENSION_HANDSHAKE) {
        if (extensionDecodeHandshake(message->payload + 1, message->payloadLength - 1u,
                                     &handshake) == EXTENSION_OK) {
            peer->pexId = dl->pexEnabled ? handshake.pexId : 0u;
            peer->metadataId = handshake.metadataId;
            peer->remotePort = handshake.port;
            snprintf(peer->client, sizeof(peer->client), "%s", handshake.client);
        }
        return;
    }
    if (message->payload[0] == EXTENSION_METADATA_ID) {
        sendMetadata(dl, peer, message->payload + 1, message->payloadLength - 1u);
        return;
    }
    if (message->payload[0] != EXTENSION_PEX_ID || !dl->pexEnabled ||
        (peer->lastPexIn != 0 &&
         now - peer->lastPexIn < (uint64_t)DOWNLOAD_PEX_MIN_RECEIVED * 1000u)) {
        return;                                     /* unknown, private, or too often */
    }
    peer->lastPexIn = now;
    context.dl = dl;
    context.learned = 0;
    (void)extensionDecodePex(message->payload + 1, message->payloadLength - 1u, onPexPeer,
                             &context);
    if (context.learned > 0) {
        dl->pexLearned += context.learned;
        emit(dl, "%s: peer exchange: %u new peer%s", peer->where, (unsigned)context.learned,
             (context.learned == 1u) ? "" : "s");
    }
}

/* When we are complete, a peer that is complete too is of no use. */
static int bothComplete(const download *dl, const downloadPeer *peer)
{
    return dl->seeding && peer->haveCount >= dl->torrent->pieceCount;
}

/* ------------------------------------------------------------------------ */
/* Peer lifetime                                                            */
/* ------------------------------------------------------------------------ */

/* Undo everything an open peer contributed to the shared state. */
static void detachPeer(download *dl, downloadPeer *peer)
{
    activePiece *piece;
    uint32_t     i;

    releaseRequests(peer);
    TAILQ_FOREACH(piece, &dl->active, entries) {
        if (piece->owner == peer) {
            piece->owner = NULL;
        }
    }
    if (peer->have != NULL) {
        for (i = 0; i < dl->torrent->pieceCount; i++) {
            if (messageBitfieldGet(peer->have, i) && dl->availability[i] > 0) {
                dl->availability[i]--;
            }
        }
        memset(peer->have, 0, messageBitfieldBytes(dl->torrent->pieceCount));
    }
    peer->interesting = 0;
    peer->haveCount = 0;
    if (!peer->amChoking && dl->uploadUnchoked > 0) {
        dl->uploadUnchoked--;
    }
    peer->amChoking = 1;
    peer->peerInterested = 0;
    peer->queueCount = 0;
    peer->readingCount = 0;                         /* reads in flight are dropped */
    if (dl->optimistic == peer) {
        dl->optimistic = NULL;
    }
    if (!peer->choked && dl->unchoking > 0) {
        dl->unchoking--;
    }
    peer->choked = 1;
    peer->amInterested = 0;
    dl->open--;
}

/*
 * Close a connection. 'exclude' marks the peer as never to be used again;
 * otherwise a peer that was open may be retried later.
 */
static void dropPeer(downloadPeer *peer, const char *reason, int exclude)
{
    download *dl = peer->owner;
    uint8_t   wasOpen = (peer->state == DOWNLOAD_PEER_OPEN);

    if (peer->state != DOWNLOAD_PEER_OPEN && peer->state != DOWNLOAD_PEER_CONNECTING) {
        return;
    }
    if (wasOpen) {
        detachPeer(dl, peer);
    } else {
        dl->connecting--;
    }
    peerClose(peer->connection);
    peer->connection = NULL;
    if (exclude) {
        peer->excluded = 1;
    }
    if (!peer->excluded && wasOpen && !peer->incoming && peer->attempts < DOWNLOAD_MAX_ATTEMPTS) {
        peer->state = DOWNLOAD_PEER_QUEUED;
        peer->retryAt = uv_now(dl->loop) + (uint64_t)DOWNLOAD_RETRY_SECONDS * 1000u;
    } else {
        peer->state = DOWNLOAD_PEER_CLOSED;
    }
    if (reason != NULL) {
        emit(dl, "%s: dropped: %s", peer->where, reason);
    }
    if (!stopped(dl)) {
        fillAll(dl);                                /* its blocks are free again */
        fillUploadSlots(dl);                        /* and its upload slot       */
        connectMore(dl);
    }
}

static int allocatePeerState(download *dl, downloadPeer *peer)
{
    if (peer->have == NULL) {
        peer->have = allocate(dl, messageBitfieldBytes(dl->torrent->pieceCount) + 1u);
    }
    if (peer->requests == NULL) {
        peer->requests = allocate(dl, sizeof(*peer->requests) * dl->pipeline);
    }
    if (peer->uploadQueue == NULL) {
        peer->uploadQueue = allocate(dl, sizeof(*peer->uploadQueue) * DOWNLOAD_UPLOAD_QUEUE);
    }
    if (peer->reading == NULL) {
        peer->reading = allocate(dl, sizeof(*peer->reading) * DOWNLOAD_UPLOAD_READS);
    }
    if (peer->advertised == NULL) {
        peer->advertised = allocate(dl, sizeof(*peer->advertised) * DOWNLOAD_PEX_TRACKED);
    }
    return (peer->have != NULL && peer->requests != NULL && peer->uploadQueue != NULL &&
            peer->reading != NULL && peer->advertised != NULL) ? 0 : -1;
}

static void onPeerHandshake(void *privateArg, peerConnection *connection,
                            const peerHandshakeResult *result)
{
    downloadPeer *peer = privateArg;
    download     *dl = peer->owner;
    char          client[PEER_ID_LENGTH + 1];
    uint32_t      i;

    dl->connecting--;
    if (result->status != PEER_OK) {
        peerClose(connection);
        peer->connection = NULL;
        peer->state = DOWNLOAD_PEER_CLOSED;         /* not retried */
        emit(dl, "%s: no handshake: %s", peer->where,
             (result->errorMessage != NULL) ? result->errorMessage
                                            : peerStatusString(result->status));
        connectMore(dl);
        return;
    }
    if (allocatePeerState(dl, peer) != 0) {
        peer->state = DOWNLOAD_PEER_OPEN;           /* so dropPeer() accounts for it */
        dl->open++;
        dropPeer(peer, "out of memory", 0);
        return;
    }
    peer->state = DOWNLOAD_PEER_OPEN;
    peer->choked = 1;
    peer->amInterested = 0;
    peer->interesting = 0;
    peer->unexpected = 0;
    peer->requestCount = 0;
    peer->lastBlock = uv_now(dl->loop);
    peer->chokedSince = peer->lastBlock;
    peer->openedAt = peer->lastBlock;
    peer->amChoking = 1;
    peer->peerInterested = 0;
    peer->haveCount = 0;
    peer->queueCount = 0;
    peer->readingCount = 0;
    peer->uploadedWindow = 0;
    peer->downloadedWindow = 0;
    memcpy(peer->remoteId, result->peerId, PEER_ID_LENGTH);
    peer->extended = (uint8_t)((result->extensions & PEER_EXTENSION_PROTOCOL) != 0);
    peer->pexId = 0;
    peer->metadataId = 0;
    peer->remotePort = 0;
    peer->lastPexIn = 0;
    peer->advertisedCount = 0;
    peer->client[0] = '\0';
    peer->nextPex = peer->openedAt + (uint64_t)DOWNLOAD_PEX_FIRST_SECONDS * 1000u;
    dl->open++;

    /* The same client by another route (it connected to us as we to it). */
    {
        downloadPeer *other;

        TAILQ_FOREACH(other, &dl->peers, entries) {
            if (other != peer && other->state == DOWNLOAD_PEER_OPEN &&
                memcmp(other->remoteId, peer->remoteId, PEER_ID_LENGTH) == 0) {
                peer->attempts = DOWNLOAD_MAX_ATTEMPTS;     /* no retry */
                dropPeer(peer, NULL, 0);
                return;
            }
        }
    }
    sendBitfield(dl, peer);
    if (peer->extended) {
        sendExtendedHandshake(dl, peer);
    }

    for (i = 0; i < PEER_ID_LENGTH; i++) {
        uint8_t c = result->peerId[i];

        client[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    client[i < 8u ? i : 8u] = '\0';
    emit(dl, "%s: connected%s (peer id %s...)", peer->where,
         peer->incoming ? " (incoming)" : "", client);
}

/* ------------------------------------------------------------------------ */
/* Blocks and pieces                                                        */
/* ------------------------------------------------------------------------ */

static activePiece *findActive(download *dl, uint32_t index)
{
    activePiece *piece;

    TAILQ_FOREACH(piece, &dl->active, entries) {
        if (piece->piece.index == index) {
            return piece;
        }
    }
    return NULL;
}

static void onPieceWritten(void *privateArg, storage *store, uint32_t pieceIndex, int32_t status,
                           const char *errorMessage)
{
    download     *dl = privateArg;
    downloadPeer *peer;
    messageFrame  have;

    (void)store;
    if (stopped(dl)) {
        return;
    }
    if (status != STORAGE_OK) {
        snprintf(dl->message, sizeof(dl->message), "could not write piece %u: %s",
                 (unsigned)pieceIndex, (errorMessage != NULL) ? errorMessage : "");
        finish(dl, DOWNLOAD_EIO, dl->message);
        return;
    }
    dl->pieceState[pieceIndex] = DOWNLOAD_PIECE_DONE;
    dl->done++;
    if (!messageBitfieldGet(dl->onDisk, pieceIndex)) {
        messageBitfieldSet(dl->onDisk, pieceIndex);
        dl->onDiskCount++;
    }
    dl->left -= (dl->left >= pieceLengthOf(dl->torrent, pieceIndex))
                    ? pieceLengthOf(dl->torrent, pieceIndex) : dl->left;

    memset(&have, 0, sizeof(have));
    have.id = MESSAGE_HAVE;
    have.index = pieceIndex;
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (peer->state == DOWNLOAD_PEER_OPEN && !messageBitfieldGet(peer->have, pieceIndex)) {
            (void)sendFrame(peer, &have);
        }
    }
    if (dl->done >= dl->wanted) {
        complete(dl);
    }
}

/* Exclude a peer for good and close it. */
static void excludePeer(download *dl, downloadPeer *peer, const char *reason)
{
    if (peer->excluded) {
        return;
    }
    peer->excluded = 1;
    if (peer->state == DOWNLOAD_PEER_OPEN || peer->state == DOWNLOAD_PEER_CONNECTING) {
        dropPeer(peer, reason, 1);
    } else {
        peer->state = DOWNLOAD_PEER_CLOSED;
        emit(dl, "%s: excluded: %s", peer->where, reason);
    }
}

/* A piece that failed before has now passed: whoever sent a block that
 * differs from the good copy sent corrupt data. */
static void blameCulprits(download *dl, activePiece *piece)
{
    downloadPeer *peer;
    char          reason[64];
    uint32_t      block;
    uint32_t      later;

    for (block = 0; block < piece->piece.blockCount && !stopped(dl); block++) {
        uint32_t begin = block * PIECE_BLOCK_LENGTH;

        peer = piece->failedBy[block];
        if (peer == NULL || memcmp(piece->failedData + begin, piece->piece.data + begin,
                                   blockLength(piece, block)) == 0) {
            continue;
        }
        for (later = block; later < piece->piece.blockCount; later++) {
            if (piece->failedBy[later] == peer) {
                piece->failedBy[later] = NULL;
            }
        }
        snprintf(reason, sizeof(reason), "sent corrupt data for piece %u",
                 (unsigned)piece->piece.index);
        excludePeer(dl, peer, reason);
    }
}

/* Everything wanted is on disk: stop, or carry on as a seed. */
static void complete(download *dl)
{
    downloadPeer *peer;

    if (stopped(dl) || dl->seeding) {
        return;
    }
    if (dl->events.onComplete != NULL) {
        dl->events.onComplete(dl->privateArg, dl);
        if (stopped(dl)) {
            return;
        }
    }
    if (!dl->seedOption) {
        finish(dl, DOWNLOAD_OK, "download complete");
        return;
    }
    dl->seeding = 1;
    emit(dl, "download complete; seeding");
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (stopped(dl)) {
            return;
        }
        if (peer->state == DOWNLOAD_PEER_OPEN && bothComplete(dl, peer)) {
            peer->attempts = DOWNLOAD_MAX_ATTEMPTS;
            peer->complete = 1;
            dropPeer(peer, "complete, as we are", 0);
        }
    }
}

/* Every block of the piece is here: verify it, then write it or try again. */
static void releaseDownload(download *dl);

static void verifyWork(uv_work_t *work)
{
    activePiece *piece = work->data;

    piece->verifyResult = pieceVerify(&piece->piece);   /* only reads the piece */
}

static void pieceChecked(download *dl, activePiece *piece, int32_t result);

static void verifyDone(uv_work_t *work, int status)
{
    activePiece *piece = work->data;
    download    *dl = piece->owningDownload;

    dl->verifyJobs--;
    piece->verifying = 0;
    if (dl->ended) {
        if (dl->timerClosed && dl->verifyJobs == 0) {
            releaseDownload(dl);                    /* the last thing we waited for */
        }
        return;
    }
    if (stopped(dl)) {
        return;                                     /* freed with the download */
    }
    pieceChecked(dl, piece, (status == 0) ? piece->verifyResult : PIECE_EINVAL);
}

/*
 * Every block of the piece is here: check it on the thread pool (a large
 * piece takes a while to hash), then write it or try again.
 */
static void completePiece(download *dl, activePiece *piece)
{
    piece->owningDownload = dl;
    piece->verifying = 1;
    piece->verifyWork.data = piece;
    if (uv_queue_work(dl->loop, &piece->verifyWork, verifyWork, verifyDone) == 0) {
        dl->verifyJobs++;
        return;
    }
    piece->verifying = 0;
    pieceChecked(dl, piece, pieceVerify(&piece->piece));    /* no pool: do it here */
}

static void pieceChecked(download *dl, activePiece *piece, int32_t result)
{
    downloadPeer *sole = NULL;
    downloadPeer *peer;
    uint8_t      *data;
    uint32_t      index = piece->piece.index;
    uint32_t      contributors = 0;
    uint32_t      block;
    uint32_t      earlier;
    int32_t       status;

    if (result == PIECE_OK) {
        if (piece->failedData != NULL) {
            blameCulprits(dl, piece);
            if (stopped(dl)) {
                return;
            }
        }
        dl->pieceState[index] = DOWNLOAD_PIECE_WRITING;
        data = pieceTakeData(&piece->piece);
        retireActive(dl, piece);
        pieceNoLongerWanted(dl, index);
        status = storageWritePiece(dl->store, index, data, onPieceWritten, dl);
        if (status != STORAGE_OK) {
            snprintf(dl->message, sizeof(dl->message), "could not queue piece %u for writing: %s",
                     (unsigned)index, storageStatusString(status));
            finish(dl, DOWNLOAD_EIO, dl->message);
        }
        return;
    }

    /* Corrupt. Work out who sent it. */
    dl->hashFailures++;
    dl->bytesWasted += piece->piece.length;
    for (block = 0; block < piece->piece.blockCount; block++) {
        int seen = 0;

        peer = piece->contributor[block];
        for (earlier = 0; peer != NULL && earlier < block && !seen; earlier++) {
            seen = (piece->contributor[earlier] == peer);
        }
        if (peer != NULL && !seen) {
            contributors++;
            sole = peer;
        }
    }
    emit(dl, "piece %u failed its SHA-1 check (%u peer%s contributed); downloading it again",
         (unsigned)index, (unsigned)contributors, (contributors == 1u) ? "" : "s");

    if (contributors > 1u && piece->failedData == NULL) {
        /* Keep this copy: once a good one arrives, the blocks that differ
         * show who sent bad data. */
        piece->failedData = allocate(dl, piece->piece.length);
        piece->failedBy = allocate(dl, sizeof(*piece->failedBy) * piece->piece.blockCount);
        if (piece->failedData != NULL && piece->failedBy != NULL) {
            memcpy(piece->failedData, piece->piece.data, piece->piece.length);
            memcpy((void *)piece->failedBy, (const void *)piece->contributor,
                   sizeof(*piece->failedBy) * piece->piece.blockCount);
            contributors = 0;                       /* no strikes: we will know */
        } else {
            release(dl, piece->failedData);
            release(dl, (void *)piece->failedBy);
            piece->failedData = NULL;
            piece->failedBy = NULL;
        }
    }

    /* Strikes: the sole sender is out; otherwise (no copy kept, or a second
     * failure) everyone involved gets one. */
    for (block = 0; contributors > 0 && block < piece->piece.blockCount; block++) {
        uint32_t later;

        peer = piece->contributor[block];
        if (peer == NULL) {
            continue;
        }
        for (later = block; later < piece->piece.blockCount; later++) {
            if (piece->contributor[later] == peer) {
                piece->contributor[later] = NULL;
            }
        }
        peer->strikes = (contributors == 1u) ? (uint8_t)DOWNLOAD_MAX_STRIKES
                                             : (uint8_t)(peer->strikes + 1u);
    }
    cancelPieceRequests(dl, piece);
    pieceReset(&piece->piece);
    memset(piece->requests, 0, piece->piece.blockCount);
    memset((void *)piece->contributor, 0, sizeof(*piece->contributor) * piece->piece.blockCount);
    piece->owner = NULL;
    piece->exclusive = 1;                           /* next time, from one peer */

    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (stopped(dl)) {
            return;
        }
        if (peer->strikes >= DOWNLOAD_MAX_STRIKES && !peer->excluded) {
            excludePeer(dl, peer, (peer == sole) ? "sent corrupt data"
                                                 : "contributed to corrupt pieces twice");
        }
    }
    fillAll(dl);
}

static void receiveBlock(download *dl, downloadPeer *peer, const messageFrame *message)
{
    activePiece  *piece;
    downloadPeer *other;
    uint32_t      block;
    int           slot;
    int32_t       status;

    dl->bytesReceived += message->payloadLength;
    peer->downloadedWindow += message->payloadLength;
    piece = findActive(dl, message->index);
    block = message->begin / PIECE_BLOCK_LENGTH;
    slot = (piece != NULL && message->begin % PIECE_BLOCK_LENGTH == 0)
               ? findRequest(peer, piece, block) : -1;

    if (slot < 0) {
        /* Not asked of this peer: a block that crossed our cancel, or junk. */
        dl->bytesWasted += message->payloadLength;
        if (dl->pieceState[message->index] == DOWNLOAD_PIECE_NEEDED ||
            message->begin % PIECE_BLOCK_LENGTH != 0 ||
            (message->payloadLength != PIECE_BLOCK_LENGTH &&
             (uint64_t)message->begin + message->payloadLength !=
                 pieceLengthOf(dl->torrent, message->index))) {
            dropPeer(peer, "sent a block nobody asked for", 1);
        } else if (++peer->unexpected > DOWNLOAD_MAX_UNEXPECTED) {
            dropPeer(peer, "keeps sending blocks we did not ask for", 1);
        }
        return;
    }

    peer->lastBlock = uv_now(dl->loop);
    dl->lastProgress = peer->lastBlock;
    if (piece->piece.blocks[block] == PIECE_BLOCK_RECEIVED) {
        dropRequestSlot(peer, (uint32_t)slot, 0);
        dl->bytesWasted += message->payloadLength;
        fillPeer(dl, peer);
        return;
    }
    status = pieceReceive(&piece->piece, message);
    if (status < 0) {
        dropRequestSlot(peer, (uint32_t)slot, 1);
        dropPeer(peer, "sent a block of the wrong size", 1);
        return;
    }
    dropRequestSlot(peer, (uint32_t)slot, 0);
    piece->contributor[block] = peer;

    /* Endgame: nobody else needs to send this block now. */
    if (piece->requests[block] > 0) {
        TAILQ_FOREACH(other, &dl->peers, entries) {
            int otherSlot;

            if (other == peer || other->requestCount == 0) {
                continue;
            }
            otherSlot = findRequest(other, piece, block);
            if (otherSlot >= 0) {
                sendBlockMessage(other, MESSAGE_CANCEL, piece, block);
                dropRequestSlot(other, (uint32_t)otherSlot, 0);
            }
        }
    }

    if (status == PIECE_COMPLETE) {
        completePiece(dl, piece);
    }
    if (!stopped(dl)) {
        fillPeer(dl, peer);
    }
}

/* ------------------------------------------------------------------------ */
/* Peer events                                                              */
/* ------------------------------------------------------------------------ */

static void onPeerMessage(void *privateArg, peerConnection *connection,
                          const messageFrame *message)
{
    downloadPeer *peer = privateArg;
    download     *dl = peer->owner;
    uint32_t      i;

    (void)connection;
    if (stopped(dl) || peer->state != DOWNLOAD_PEER_OPEN) {
        return;
    }
    switch (message->id) {
    case MESSAGE_BITFIELD:
        for (i = 0; i < dl->torrent->pieceCount; i++) {
            if (messageBitfieldGet(message->payload, i)) {
                peerHasPiece(dl, peer, i);
            }
        }
        if (bothComplete(dl, peer)) {
            peer->attempts = DOWNLOAD_MAX_ATTEMPTS;
            peer->complete = 1;
            dropPeer(peer, "complete, as we are", 0);
            break;
        }
        updateInterest(peer);
        fillPeer(dl, peer);
        break;
    case MESSAGE_HAVE:
        peerHasPiece(dl, peer, message->index);
        if (bothComplete(dl, peer)) {
            peer->attempts = DOWNLOAD_MAX_ATTEMPTS;
            peer->complete = 1;
            dropPeer(peer, "complete, as we are", 0);
            break;
        }
        updateInterest(peer);
        fillPeer(dl, peer);
        break;
    case MESSAGE_INTERESTED:
        peer->peerInterested = 1;
        if (dl->uploadUnchoked < dl->uploadSlots) {
            setChoking(dl, peer, 0);                /* a free slot: no need to wait */
        }
        break;
    case MESSAGE_NOT_INTERESTED:
        peer->peerInterested = 0;
        if (!peer->amChoking && someoneWaiting(dl)) {
            setChoking(dl, peer, 1);                /* its slot is wanted */
            fillUploadSlots(dl);
        }
        break;
    case MESSAGE_REQUEST:
        queueUpload(dl, peer, message);
        break;
    case MESSAGE_CANCEL:
        cancelUpload(peer, message);
        break;
    case EXTENSION_MESSAGE_ID:
        receiveExtended(dl, peer, message);
        break;
    case MESSAGE_CHOKE:
        if (!peer->choked) {
            peer->choked = 1;
            peer->chokedSince = uv_now(dl->loop);
            dl->unchoking--;
            releaseRequests(peer);                  /* the peer discards them */
            fillAll(dl);
        }
        break;
    case MESSAGE_UNCHOKE:
        if (peer->choked) {
            peer->choked = 0;
            dl->unchoking++;
            peer->lastBlock = uv_now(dl->loop);
            fillPeer(dl, peer);
        }
        break;
    case MESSAGE_PIECE:
        receiveBlock(dl, peer, message);
        break;
    default:
        break;                                      /* port, extensions */
    }
}

static void onPeerClosed(void *privateArg, peerConnection *connection, int32_t status,
                         const char *errorMessage)
{
    downloadPeer *peer = privateArg;

    (void)connection;
    (void)status;
    if (stopped(peer->owner)) {
        return;
    }
    dropPeer(peer, (errorMessage != NULL) ? errorMessage : "connection closed", 0);
}

static void onPeerDrain(void *privateArg, peerConnection *connection)
{
    downloadPeer *peer = privateArg;

    (void)connection;
    pumpUploads(peer->owner, peer);
}

static int sameHost(const struct sockaddr_storage *a, const struct sockaddr *b)
{
    if (a->ss_family != b->sa_family) {
        return 0;
    }
    if (b->sa_family == AF_INET) {
        return memcmp(&((const struct sockaddr_in *)a)->sin_addr,
                      &((const struct sockaddr_in *)b)->sin_addr, sizeof(struct in_addr)) == 0;
    }
    return memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr,
                  &((const struct sockaddr_in6 *)b)->sin6_addr, sizeof(struct in6_addr)) == 0;
}

/* Someone connected to us: take them if they want this torrent and there is room. */
static int onIncoming(void *listenerArg, peerConnection *connection, const struct sockaddr *address,
                      const uint8_t infoHash[PARSER_SHA1_LENGTH], peerConnectOptions *options,
                      peerCallbacks *callbacks, void **privateArg)
{
    download     *dl = listenerArg;
    downloadPeer *peer;
    size_t        size;

    if (stopped(dl) || !dl->ready ||
        memcmp(infoHash, dl->torrent->infoHash, PARSER_SHA1_LENGTH) != 0 ||
        dl->open + dl->connecting >= dl->maxPeers + DOWNLOAD_INCOMING_EXTRA ||
        dl->peerCount >= DOWNLOAD_MAX_KNOWN_PEERS ||
        (address->sa_family != AF_INET && address->sa_family != AF_INET6)) {
        return 0;
    }
    if (blocklistContains(dl->blocklist, address)) {
        dl->blocked++;
        return 0;
    }
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (peer->excluded && sameHost(&peer->address, address)) {
            return 0;                               /* misbehaved before */
        }
    }
    peer = allocate(dl, sizeof(*peer));
    if (peer == NULL) {
        return 0;
    }
    size = (address->sa_family == AF_INET) ? sizeof(struct sockaddr_in)
                                           : sizeof(struct sockaddr_in6);
    memcpy(&peer->address, address, size);
    formatAddress(address, peer->where, sizeof(peer->where));
    peer->owner = dl;
    peer->state = DOWNLOAD_PEER_CONNECTING;
    peer->incoming = 1;
    peer->choked = 1;
    peer->amChoking = 1;
    peer->attempts = 1;
    peer->connection = connection;
    TAILQ_INSERT_TAIL(&dl->peers, peer, entries);
    dl->peerCount++;
    dl->connecting++;

    *options = dl->connectOptions;
    options->timeoutSeconds = DOWNLOAD_INCOMING_TIMEOUT;
    callbacks->onHandshake = onPeerHandshake;
    callbacks->onMessage = onPeerMessage;
    callbacks->onClosed = onPeerClosed;
    callbacks->onDrain = onPeerDrain;
    *privateArg = peer;
    return 1;
}

static void closeListeners(download *dl)
{
    peerListenerClose(dl->listeners[0]);
    peerListenerClose(dl->listeners[1]);
    dl->listeners[0] = NULL;
    dl->listeners[1] = NULL;
}

/* Fill free connection slots from the queue. */
static void connectMore(download *dl)
{
    peerCallbacks callbacks = { onPeerHandshake, onPeerMessage, onPeerClosed, onPeerDrain };
    downloadPeer *peer;
    uint64_t      now = uv_now(dl->loop);
    int32_t       status;

    if (stopped(dl) || !dl->ready) {
        return;
    }
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (dl->connecting + dl->open >= dl->maxPeers) {
            return;
        }
        if (peer->state != DOWNLOAD_PEER_QUEUED || peer->retryAt > now) {
            continue;
        }
        peer->attempts++;
        status = peerConnect((const struct sockaddr *)&peer->address, &dl->connectOptions,
                             &callbacks, peer, &peer->connection);
        if (status == PEER_OK) {
            peer->state = DOWNLOAD_PEER_CONNECTING;
            dl->connecting++;
        } else {
            peer->connection = NULL;
            peer->state = DOWNLOAD_PEER_CLOSED;
            emit(dl, "%s: could not connect: %s", peer->where, peerStatusString(status));
            if (stopped(dl)) {
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Timer and lifetime                                                       */
/* ------------------------------------------------------------------------ */

/* Stop everything and tell the user (once). */
static void finish(download *dl, int32_t status, const char *message)
{
    downloadPeer *peer;

    if (stopped(dl)) {
        return;
    }
    dl->finished = 1;
    closeListeners(dl);
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        peerClose(peer->connection);
        peer->connection = NULL;
        if (peer->state == DOWNLOAD_PEER_OPEN || peer->state == DOWNLOAD_PEER_CONNECTING) {
            peer->state = DOWNLOAD_PEER_CLOSED;
        }
    }
    dl->open = 0;
    dl->connecting = 0;
    dl->unchoking = 0;
    if (status != DOWNLOAD_OK) {
        storageClose(dl->store);                    /* writes in flight are dropped */
        dl->store = NULL;
    }
    uv_timer_stop(&dl->timer);
    if (dl->events.onFinished != NULL) {
        if (message != dl->message) {
            snprintf(dl->message, sizeof(dl->message), "%s", message);
        }
        dl->events.onFinished(dl->privateArg, dl, status, dl->message);
    }
}

static void onTick(uv_timer_t *timer)
{
    download     *dl = timer->data;
    downloadPeer *peer;
    downloadPeer *next;
    uint64_t      now = uv_now(dl->loop);
    uint32_t      waiting = 0;
    uint32_t      known = 0;

    if (stopped(dl)) {
        return;
    }
    /* Every 100 ms: let through what the speed limits now allow. */
    if (dl->upLimit.rate != 0) {
        pumpAll(dl);
    }
    if (dl->downLimit.rate != 0) {
        fillAll(dl);
    }
    if (stopped(dl) || now - dl->lastTick < DOWNLOAD_TICK_MS) {
        return;
    }
    dl->lastTick = now;
    if (dl->seeding) {
        dl->lastProgress = now;                     /* nothing to wait for */
    }
    if (now - dl->lastProgress >= (uint64_t)DOWNLOAD_IDLE_SECONDS * 1000u) {
        snprintf(dl->message, sizeof(dl->message), "no data received for %u seconds",
                 (unsigned)DOWNLOAD_IDLE_SECONDS);
        finish(dl, DOWNLOAD_ESTALLED, dl->message);
        return;
    }
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (peer->state == DOWNLOAD_PEER_QUEUED) {
            known++;
            waiting += (peer->retryAt <= now) ? 1u : 0u;
        } else if (peer->state != DOWNLOAD_PEER_CLOSED) {
            known++;
        }
    }
    if (known > 0 || dl->seeding) {
        dl->noPeersSince = 0;
    } else if (dl->noPeersSince == 0) {
        dl->noPeersSince = now;                     /* trackers or incoming may bring more */
    } else if (now - dl->noPeersSince >= (uint64_t)DOWNLOAD_NOPEERS_SECONDS * 1000u) {
        finish(dl, DOWNLOAD_ENOPEERS, "no peers left to download from");
        return;
    }

    for (peer = TAILQ_FIRST(&dl->peers); peer != NULL && !stopped(dl); peer = next) {
        next = TAILQ_NEXT(peer, entries);
        if (peer->state != DOWNLOAD_PEER_OPEN) {
            continue;
        }
        if (peer->requestCount > 0 &&
            now - peer->lastBlock >= (uint64_t)DOWNLOAD_STALL_SECONDS * 1000u) {
            dropPeer(peer, "stopped sending blocks", 0);
        } else if (waiting > 0 && peer->interesting == 0 && !peer->peerInterested &&
                   !dl->seeding && now - peer->openedAt >= (uint64_t)DOWNLOAD_CHOKED_SECONDS * 1000u) {
            waiting--;
            dropPeer(peer, "has nothing we need; making room for another peer", 0);
        } else if (waiting > 0 && peer->choked && !peer->peerInterested && !dl->seeding &&
                   now - peer->chokedSince >= (uint64_t)DOWNLOAD_CHOKED_SECONDS * 1000u) {
            waiting--;
            dropPeer(peer, "kept us choked; making room for another peer", 0);
        }
    }
    if (stopped(dl)) {
        return;
    }
    if (now - dl->lastRechoke >= (uint64_t)DOWNLOAD_RECHOKE_SECONDS * 1000u) {
        rechoke(dl, now);
        if (stopped(dl)) {
            return;
        }
    }
    if (dl->pexEnabled) {
        TAILQ_FOREACH(peer, &dl->peers, entries) {
            if (peer->state == DOWNLOAD_PEER_OPEN && peer->pexId != 0 && now >= peer->nextPex) {
                peer->nextPex = now + (uint64_t)DOWNLOAD_PEX_SECONDS * 1000u;
                sendPex(dl, peer);
            }
        }
    }
    fillAll(dl);
    connectMore(dl);
}

static void onTimerClosed(uv_handle_t *handle)
{
    download *dl = handle->data;

    dl->timerClosed = 1;
    if (dl->verifyJobs == 0) {
        releaseDownload(dl);                        /* else the last check does it */
    }
}

static void releaseDownload(download *dl)
{
    downloadPeer *peer;
    activePiece  *piece;

    while ((peer = TAILQ_FIRST(&dl->peers)) != NULL) {
        TAILQ_REMOVE(&dl->peers, peer, entries);
        release(dl, peer->have);
        release(dl, peer->requests);
        release(dl, peer->uploadQueue);
        release(dl, peer->reading);
        release(dl, (void *)peer->advertised);
        release(dl, peer);
    }
    while ((piece = TAILQ_FIRST(&dl->active)) != NULL) {
        TAILQ_REMOVE(&dl->active, piece, entries);
        freeActive(dl, piece);
    }
    release(dl, dl->pieceState);
    release(dl, dl->availability);
    release(dl, dl->onDisk);
    release(dl, dl);
}

/* The files are ready: decide what to fetch, then start connecting. */
static void onStorageOpen(void *privateArg, storage *store, int32_t status,
                          const char *errorMessage)
{
    download     *dl = privateArg;
    downloadPeer *peer;
    const uint8_t *have;
    uint32_t      haveCount = 0;
    uint32_t      i;

    if (stopped(dl)) {
        return;
    }
    if (status != STORAGE_OK) {
        snprintf(dl->message, sizeof(dl->message), "%s",
                 (errorMessage != NULL) ? errorMessage : storageStatusString(status));
        if (dl->events.onReady != NULL) {
            dl->events.onReady(dl->privateArg, dl, DOWNLOAD_EIO, dl->message);
        }
        finish(dl, DOWNLOAD_EIO, dl->message);
        return;
    }
    have = storageHave(store, &haveCount);
    memcpy(dl->onDisk, have, messageBitfieldBytes(dl->torrent->pieceCount));
    dl->onDiskCount = haveCount;
    for (i = 0; i < dl->torrent->pieceCount; i++) {
        int wanted = (dl->onlyPiece < 0) || ((int64_t)i == dl->onlyPiece);

        if (!wanted) {
            dl->pieceState[i] = DOWNLOAD_PIECE_DONE;
        } else if (messageBitfieldGet(have, i)) {
            dl->pieceState[i] = DOWNLOAD_PIECE_DONE;
            dl->wanted++;
            dl->done++;
            dl->resumed++;
        } else {
            dl->pieceState[i] = DOWNLOAD_PIECE_NEEDED;
            dl->wanted++;
            dl->neededCount++;
            dl->left += pieceLengthOf(dl->torrent, i);
        }
    }
    dl->ready = 1;
    dl->lastProgress = uv_now(dl->loop);
    if (dl->events.onReady != NULL) {
        dl->events.onReady(dl->privateArg, dl, DOWNLOAD_OK, NULL);
        if (stopped(dl)) {
            return;
        }
    }
    if (dl->neededCount == 0 && !dl->seedOption) {
        finish(dl, DOWNLOAD_OK, "nothing to download: already complete");
        return;
    }
    if (dl->neededCount == 0) {
        dl->seeding = 1;                            /* complete before we started */
        emit(dl, "already complete; seeding");
    }
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (peer->state == DOWNLOAD_PEER_OPEN) {
            recountInteresting(dl, peer);
        }
    }
    dl->lastTick = uv_now(dl->loop);
    uv_timer_start(&dl->timer, onTick, DOWNLOAD_RATE_TICK_MS, DOWNLOAD_RATE_TICK_MS);
    connectMore(dl);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t downloadBegin(uv_loop_t *loop, parserContext *context, const parserTorrent *torrent,
                      const downloadOptions *options, const downloadEvents *events,
                      void *privateArg, download **transfer)
{
    download *dl;
    uint64_t  seed = 0;
    int32_t   status;

    if (transfer != NULL) {
        *transfer = NULL;
    }
    if (loop == NULL || context == NULL || torrent == NULL || options == NULL ||
        transfer == NULL || torrent->pieceCount == 0 ||
        options->onlyPiece >= (int64_t)torrent->pieceCount ||
        options->pipeline > DOWNLOAD_MAX_PIPELINE) {
        return DOWNLOAD_EINVAL;
    }
    dl = context->allocator.realloc(NULL, sizeof(*dl));
    if (dl == NULL) {
        return DOWNLOAD_ENOMEM;
    }
    memset(dl, 0, sizeof(*dl));
    dl->loop = loop;
    dl->context = context;
    dl->torrent = torrent;
    if (events != NULL) {
        dl->events = *events;
    }
    dl->privateArg = privateArg;
    dl->maxPeers = (options->maxPeers != 0) ? options->maxPeers : DOWNLOAD_DEFAULT_MAX_PEERS;
    dl->pipeline = (options->pipeline != 0) ? options->pipeline : DOWNLOAD_DEFAULT_PIPELINE;
    dl->onlyPiece = (options->onlyPiece < 0) ? -1 : options->onlyPiece;
    dl->seedOption = options->seed;
    dl->uploadSlots = (options->uploadSlots != 0) ? options->uploadSlots
                                                  : DOWNLOAD_DEFAULT_UPLOAD_SLOTS;
    TAILQ_INIT(&dl->active);
    TAILQ_INIT(&dl->peers);

    memcpy(dl->connectOptions.infoHash, torrent->infoHash, PARSER_SHA1_LENGTH);
    memcpy(dl->connectOptions.peerId, options->peerId, PEER_ID_LENGTH);
    dl->connectOptions.pieceCount = torrent->pieceCount;
    dl->connectOptions.timeoutSeconds = options->handshakeTimeoutSeconds;
    dl->connectOptions.extensions = PEER_EXTENSION_PROTOCOL;
    dl->pexEnabled = (uint8_t)!torrent->isPrivate;      /* BEP 27: trackers only */
    dl->info = options->infoDictionary;
    dl->infoLength = options->infoDictionaryLength;
    dl->blocklist = options->blocklist;
    rateSet(&dl->downLimit, options->maxDownloadRate, uv_now(loop));
    rateSet(&dl->upLimit, options->maxUploadRate, uv_now(loop));

    if (uv_random(NULL, NULL, &seed, sizeof(seed), 0, NULL) != 0) {
        seed = 0;
    }
    dl->random = (seed ^ uv_hrtime()) | 1u;

    dl->pieceState = allocate(dl, torrent->pieceCount);
    dl->availability = allocate(dl, sizeof(*dl->availability) * torrent->pieceCount);
    dl->onDisk = allocate(dl, messageBitfieldBytes(torrent->pieceCount) + 1u);
    if (dl->pieceState == NULL || dl->availability == NULL || dl->onDisk == NULL) {
        release(dl, dl->pieceState);
        release(dl, dl->availability);
        release(dl, dl->onDisk);
        release(dl, dl);
        return DOWNLOAD_ENOMEM;
    }

    status = storageOpen(loop, context, torrent,
                         (options->directory != NULL) ? options->directory : "",
                         STORAGE_CHECK_EXISTING, onStorageOpen, dl, &dl->store);
    if (status != STORAGE_OK) {
        release(dl, dl->pieceState);
        release(dl, dl->availability);
        release(dl, dl->onDisk);
        release(dl, dl);
        return (status == STORAGE_ENOMEM) ? DOWNLOAD_ENOMEM
             : (status == STORAGE_EINVAL) ? DOWNLOAD_EINVAL : DOWNLOAD_EIO;
    }
    uv_timer_init(loop, &dl->timer);
    dl->timer.data = dl;

    /* Accept connections: the given port, or the next free one. Either
     * family may be missing (no IPv6, or an interface with one family). */
    if (options->listenPort != 0) {
        uint32_t attempt;

        for (attempt = 0; attempt < DOWNLOAD_LISTEN_ATTEMPTS &&
                          (uint32_t)options->listenPort + attempt <= 65535u; attempt++) {
            uint16_t port = (uint16_t)(options->listenPort + attempt);
            int32_t  v4 = peerListen(AF_INET, port, onIncoming, dl, &dl->listeners[0]);
            int32_t  v6;

            if (v4 == PEER_EADDRINUSE) {
                continue;
            }
            v6 = peerListen(AF_INET6, port, onIncoming, dl, &dl->listeners[1]);
            if (v4 == PEER_OK || v6 == PEER_OK) {
                dl->listenPort = port;
            }
            break;
        }
    }
    *transfer = dl;
    return DOWNLOAD_OK;
}

int32_t downloadAddPeer(download *dl, const struct sockaddr *address)
{
    downloadPeer *peer;
    size_t        size;

    if (dl == NULL || address == NULL ||
        (address->sa_family != AF_INET && address->sa_family != AF_INET6)) {
        return DOWNLOAD_EINVAL;
    }
    if (stopped(dl)) {
        return DOWNLOAD_EINVAL;
    }
    if (blocklistContains(dl->blocklist, address)) {
        dl->blocked++;
        return DOWNLOAD_OK;                         /* quietly ignored */
    }
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        if (sameAddress(&peer->address, address)) {
            if (peer->state == DOWNLOAD_PEER_CLOSED && !peer->excluded && !peer->incoming &&
                !(peer->complete && dl->seeding)) {
                peer->state = DOWNLOAD_PEER_QUEUED; /* reported again: new chances */
                peer->attempts = 0;
                peer->retryAt = 0;
                connectMore(dl);
            }
            return DOWNLOAD_OK;                     /* already known */
        }
        if (peer->incoming && peer->remotePort != 0 && peer->state == DOWNLOAD_PEER_OPEN &&
            sameHost(&peer->address, address) &&
            peer->remotePort == ntohs((address->sa_family == AF_INET)
                                          ? ((const struct sockaddr_in *)address)->sin_port
                                          : ((const struct sockaddr_in6 *)address)->sin6_port)) {
            return DOWNLOAD_OK;                     /* connected to us already */
        }
    }
    if (dl->peerCount >= DOWNLOAD_MAX_KNOWN_PEERS) {
        return DOWNLOAD_OK;                         /* plenty to be going on with */
    }
    peer = allocate(dl, sizeof(*peer));
    if (peer == NULL) {
        return DOWNLOAD_ENOMEM;
    }
    size = (address->sa_family == AF_INET) ? sizeof(struct sockaddr_in)
                                           : sizeof(struct sockaddr_in6);
    memcpy(&peer->address, address, size);
    formatAddress(address, peer->where, sizeof(peer->where));
    peer->owner = dl;
    peer->state = DOWNLOAD_PEER_QUEUED;
    peer->choked = 1;
    TAILQ_INSERT_TAIL(&dl->peers, peer, entries);
    dl->peerCount++;
    connectMore(dl);
    return DOWNLOAD_OK;
}

void downloadGetProgress(const download *dl, downloadProgress *progress)
{
    const downloadPeer *peer;

    if (progress == NULL) {
        return;
    }
    memset(progress, 0, sizeof(*progress));
    if (dl == NULL) {
        return;
    }
    progress->pieceCount = dl->torrent->pieceCount;
    progress->wanted = dl->wanted;
    progress->done = dl->done;
    progress->resumed = dl->resumed;
    progress->bytesReceived = dl->bytesReceived;
    progress->bytesWasted = dl->bytesWasted;
    progress->hashFailures = dl->hashFailures;
    progress->piecesActive = dl->activeCount;
    progress->endgame = dl->endgame;
    progress->seeding = dl->seeding;
    progress->left = dl->left;
    progress->bytesUploaded = dl->bytesUploaded;
    progress->listenPort = dl->listenPort;
    progress->peersFromPex = dl->pexLearned;
    progress->peersBlocked = dl->blocked;
    progress->maxDownloadRate = dl->downLimit.rate;
    progress->maxUploadRate = dl->upLimit.rate;
    progress->checking = (uint8_t)(!dl->ready && !dl->finished);
    storageCheckProgress(dl->store, &progress->checkedPieces, &progress->checkTotal);
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        switch (peer->state) {
        case DOWNLOAD_PEER_QUEUED:
            progress->peersQueued++;
            break;
        case DOWNLOAD_PEER_CONNECTING:
            progress->peersConnecting++;
            break;
        case DOWNLOAD_PEER_OPEN:
            progress->peersConnected++;
            progress->peersUnchoking += peer->choked ? 0u : 1u;
            progress->peersDownloading += (peer->requestCount > 0) ? 1u : 0u;
            progress->peersInterested += peer->peerInterested ? 1u : 0u;
            progress->peersUploading += peer->amChoking ? 0u : 1u;
            progress->peersIncoming += peer->incoming ? 1u : 0u;
            break;
        default:
            break;
        }
    }
}

void downloadSetRateLimits(download *dl, uint64_t maxDownloadRate, uint64_t maxUploadRate)
{
    if (dl == NULL || stopped(dl)) {
        return;
    }
    rateSet(&dl->downLimit, maxDownloadRate, uv_now(dl->loop));
    rateSet(&dl->upLimit, maxUploadRate, uv_now(dl->loop));
    fillAll(dl);
    if (!stopped(dl)) {
        pumpAll(dl);
    }
}

uint16_t downloadListenPort(const download *dl)
{
    return (dl != NULL) ? dl->listenPort : 0;
}

const char *downloadRoot(const download *dl)
{
    return (dl != NULL && dl->store != NULL) ? storageRoot(dl->store) : "";
}

void downloadEnd(download *dl)
{
    downloadPeer *peer;

    if (dl == NULL || dl->ended) {
        return;
    }
    dl->ended = 1;
    closeListeners(dl);
    TAILQ_FOREACH(peer, &dl->peers, entries) {
        peerClose(peer->connection);
        peer->connection = NULL;
    }
    storageClose(dl->store);
    dl->store = NULL;
    uv_timer_stop(&dl->timer);
    uv_close((uv_handle_t *)&dl->timer, onTimerClosed);
}

const char *downloadStatusString(int32_t status)
{
    switch (status) {
    case DOWNLOAD_OK:           return "OK";
    case DOWNLOAD_EINVAL:       return "invalid argument";
    case DOWNLOAD_ENOMEM:       return "out of memory";
    case DOWNLOAD_EIO:          return "file error";
    case DOWNLOAD_ENOPEERS:     return "no peers left";
    case DOWNLOAD_ESTALLED:     return "no progress";
    default:                    return "unknown status";
    }
}
