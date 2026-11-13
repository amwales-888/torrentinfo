/*
 * metadata.c - Fetching a torrent's info dictionary from peers (BEP 9).
 * See metadata.h.
 *
 * The first peer to state a metadata_size fixes the size; the dictionary is
 * split into 16 KiB pieces, each requested from one peer at a time (two per
 * peer in flight). Peers stating another size, offering no metadata, or
 * rejecting requests are let go. A copy whose SHA-1 is wrong is thrown away
 * and the peers that sent it are not asked again.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "extension.h"
#include "metadata.h"
#include "queue.h"

#define METADATA_MAX_KNOWN      1000u
#define METADATA_TICK_MS        1000u
#define METADATA_CLIENT_NAME    "torrentinfo 0.8"
#define METADATA_MAX_ATTEMPTS   5u      /* connections per address, the waits
                                           between them doubling               */
#ifndef METADATA_RETRY_SECONDS
#define METADATA_RETRY_SECONDS  30u     /* before trying a peer again           */
#endif

#define PEER_QUEUED             0u
#define PEER_CONNECTING         1u
#define PEER_OPEN               2u
#define PEER_CLOSED             3u

#define PIECE_MISSING           0u
#define PIECE_REQUESTED         1u
#define PIECE_DONE              2u

typedef struct metaPeer {
    TAILQ_ENTRY(metaPeer)   entries;
    metadataFetch          *owner;
    struct sockaddr_storage address;
    char                    where[72];
    peerConnection         *connection;
    uint8_t                 state;
    uint8_t                 excluded;
    uint8_t                 metadataId;     /* its id for ut_metadata          */
    uint8_t                 attempts;       /* connections made                */
    uint8_t                 refused;        /* said it has no metadata to give */
    uint64_t                retryAt;        /* uv_now() before which to wait   */
    uint32_t                requests[METADATA_REQUESTS_PER_PEER];
    uint32_t                requestCount;
    uint64_t                requestedAt;    /* uv_now() of the oldest request  */
} metaPeer;

TAILQ_HEAD(metaPeerList, metaPeer);

struct metadataFetch {
    uv_loop_t          *loop;
    parserContext      *context;
    metadataEvents      events;
    void               *privateArg;
    peerConnectOptions  connectOptions;
    uint32_t            maxPeers;
    uint16_t            listenPort;
    const blocklist    *blocklist;
    uint32_t            blocked;
    uv_timer_t          timer;
    struct metaPeerList peers;
    uint32_t            peerCount;
    uint32_t            connecting;
    uint32_t            open;

    uint8_t            *info;           /* the dictionary, as it arrives      */
    uint32_t            size;
    uint32_t            pieces;
    uint8_t            *pieceState;
    metaPeer          **pieceFrom;      /* who sent each piece                */
    uint32_t            piecesDone;
    uint32_t            refusals;       /* peers without metadata to give    */
    uint8_t             offered;        /* some peer did offer it            */
    uint8_t             hinted;

    uint8_t             done;
    uint8_t             ended;
    char                message[256];
};

static void *allocate(metadataFetch *f, size_t size)
{
    void *memory = f->context->allocator.realloc(NULL, size);

    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static void release(metadataFetch *f, void *memory)
{
    if (memory != NULL) {
        f->context->allocator.free(memory);
    }
}

static int stopped(const metadataFetch *f)
{
    return f->done || f->ended;
}

#if defined(__GNUC__)
static void emit(metadataFetch *f, const char *format, ...) __attribute__((format(printf, 2, 3)));
#endif

static void emit(metadataFetch *f, const char *format, ...)
{
    va_list arguments;

    if (stopped(f) || f->events.onEvent == NULL) {
        return;
    }
    va_start(arguments, format);
    vsnprintf(f->message, sizeof(f->message), format, arguments);
    va_end(arguments);
    f->events.onEvent(f->privateArg, f, f->message);
}

static void formatAddress(const struct sockaddr *address, char *out, size_t size)
{
    char host[48];

    if (address->sa_family == AF_INET6) {
        uv_ip6_name((const struct sockaddr_in6 *)address, host, sizeof(host));
        snprintf(out, size, "[%s]:%u", host,
                 (unsigned)ntohs(((const struct sockaddr_in6 *)address)->sin6_port));
    } else {
        uv_ip4_name((const struct sockaddr_in *)address, host, sizeof(host));
        snprintf(out, size, "%s:%u", host,
                 (unsigned)ntohs(((const struct sockaddr_in *)address)->sin_port));
    }
}

static uint32_t pieceLength(const metadataFetch *f, uint32_t piece)
{
    uint32_t begin = piece * EXTENSION_METADATA_PIECE;

    return (f->size - begin < EXTENSION_METADATA_PIECE) ? f->size - begin
                                                         : EXTENSION_METADATA_PIECE;
}

static void sendExtended(metaPeer *peer, const uint8_t *payload, size_t length)
{
    messageFrame frame;

    if (peer->connection == NULL || peer->state != PEER_OPEN) {
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.id = EXTENSION_MESSAGE_ID;
    frame.payload = payload;
    frame.payloadLength = (uint32_t)length;
    (void)peerSend(peer->connection, &frame);
}

static void connectMore(metadataFetch *f);

/* Closed: try again later (peers often refuse while busy), unless it
 * misbehaved or has had its chances. */
static void requeue(metadataFetch *f, metaPeer *peer)
{
    if (!peer->excluded && peer->attempts < METADATA_MAX_ATTEMPTS) {
        peer->state = PEER_QUEUED;
        peer->retryAt = uv_now(f->loop) +
                        ((uint64_t)METADATA_RETRY_SECONDS * 1000u << (peer->attempts - 1u));
    } else {
        peer->state = PEER_CLOSED;
    }
}
static void requestMore(metadataFetch *f, metaPeer *peer);

/* Requests out to this peer go back to the pool. */
static void releaseRequests(metadataFetch *f, metaPeer *peer)
{
    uint32_t i;

    for (i = 0; i < peer->requestCount; i++) {
        if (f->pieceState != NULL && f->pieceState[peer->requests[i]] == PIECE_REQUESTED) {
            f->pieceState[peer->requests[i]] = PIECE_MISSING;
        }
    }
    peer->requestCount = 0;
}

static void dropPeer(metaPeer *peer, const char *reason, int exclude)
{
    metadataFetch *f = peer->owner;
    metaPeer      *other;

    if (peer->state == PEER_OPEN) {
        f->open--;
    } else if (peer->state == PEER_CONNECTING) {
        f->connecting--;
    } else {
        return;
    }
    releaseRequests(f, peer);
    peerClose(peer->connection);
    peer->connection = NULL;
    peer->excluded = (uint8_t)(peer->excluded || exclude);
    requeue(f, peer);
    if (reason != NULL) {
        emit(f, "%s: %s", peer->where, reason);
    }
    if (stopped(f)) {
        return;
    }
    TAILQ_FOREACH(other, &f->peers, entries) {
        requestMore(f, other);                      /* its pieces are free again */
    }
    connectMore(f);
}

/* Ask the peer for missing pieces, up to the per-peer limit. */
static void requestMore(metadataFetch *f, metaPeer *peer)
{
    uint8_t  buffer[1 + 64];
    size_t   length;
    uint32_t piece;

    if (stopped(f) || peer->state != PEER_OPEN || peer->metadataId == 0 || f->info == NULL) {
        return;
    }
    for (piece = 0; piece < f->pieces && peer->requestCount < METADATA_REQUESTS_PER_PEER;
         piece++) {
        if (f->pieceState[piece] != PIECE_MISSING) {
            continue;
        }
        buffer[0] = peer->metadataId;
        if (extensionEncodeMetadata(EXTENSION_METADATA_REQUEST, piece, 0, buffer + 1,
                                    sizeof(buffer) - 1u, &length) != EXTENSION_OK) {
            return;
        }
        if (peer->requestCount == 0) {
            peer->requestedAt = uv_now(f->loop);
        }
        f->pieceState[piece] = PIECE_REQUESTED;
        peer->requests[peer->requestCount++] = piece;
        sendExtended(peer, buffer, 1u + length);
    }
}

/* All pieces are here: check the hash. */
static void checkMetadata(metadataFetch *f)
{
    uint8_t   digest[PARSER_SHA1_LENGTH];
    metaPeer *peer;
    uint32_t  i;

    parserSha1(f->info, f->size, digest);
    if (memcmp(digest, f->connectOptions.infoHash, PARSER_SHA1_LENGTH) == 0) {
        f->done = 1;
        TAILQ_FOREACH(peer, &f->peers, entries) {
            peerClose(peer->connection);
            peer->connection = NULL;
            if (peer->state == PEER_OPEN || peer->state == PEER_CONNECTING) {
                peer->state = PEER_CLOSED;
            }
        }
        f->open = 0;
        f->connecting = 0;
        uv_timer_stop(&f->timer);
        if (f->events.onDone != NULL) {
            f->events.onDone(f->privateArg, f, f->info, f->size);
        }
        return;
    }
    /* Wrong: whoever sent pieces of it is not asked again. */
    emit(f, "the metadata received does not match the info hash; fetching it again");
    for (i = 0; i < f->pieces; i++) {
        peer = f->pieceFrom[i];
        f->pieceState[i] = PIECE_MISSING;
        f->pieceFrom[i] = NULL;
        if (peer != NULL && !peer->excluded) {
            peer->excluded = 1;
            if (peer->state == PEER_OPEN || peer->state == PEER_CONNECTING) {
                dropPeer(peer, "sent wrong metadata", 1);
            }
        }
    }
    /* The size came from a peer too, perhaps the liar: forget it, so that
     * the next peer's size is taken (peers turned away over it will be
     * tried again). */
    f->piecesDone = 0;
    release(f, f->info);
    release(f, f->pieceState);
    release(f, (void *)f->pieceFrom);
    f->info = NULL;
    f->pieceState = NULL;
    f->pieceFrom = NULL;
    f->size = 0;
    f->pieces = 0;
    TAILQ_FOREACH(peer, &f->peers, entries) {
        if (peer->state == PEER_OPEN) {
            dropPeer(peer, NULL, 0);                /* reconnect for a fresh handshake */
        }
    }
}

static void receiveMetadata(metadataFetch *f, metaPeer *peer, const uint8_t *data, size_t length)
{
    uint32_t type;
    uint32_t piece;
    uint32_t totalSize;
    size_t   offset;
    uint32_t i;
    int      asked = 0;

    if (extensionDecodeMetadata(data, length, &type, &piece, &totalSize, &offset) !=
        EXTENSION_OK) {
        dropPeer(peer, "sent a malformed metadata message", 1);
        return;
    }
    if (type == EXTENSION_METADATA_REQUEST) {
        uint8_t buffer[1 + 64];
        size_t  encoded;

        buffer[0] = peer->metadataId;               /* we have none to give yet */
        if (peer->metadataId != 0 &&
            extensionEncodeMetadata(EXTENSION_METADATA_REJECT, piece, 0, buffer + 1,
                                    sizeof(buffer) - 1u, &encoded) == EXTENSION_OK) {
            sendExtended(peer, buffer, 1u + encoded);
        }
        return;
    }
    for (i = 0; i < peer->requestCount; i++) {
        if (peer->requests[i] == piece) {
            peer->requests[i] = peer->requests[--peer->requestCount];
            asked = 1;
            break;
        }
    }
    if (!asked || f->info == NULL || piece >= f->pieces) {
        return;                                     /* not asked for: ignore */
    }
    if (type == EXTENSION_METADATA_REJECT) {
        f->pieceState[piece] = PIECE_MISSING;
        dropPeer(peer, "will not send the metadata", 0);
        return;
    }
    if (type != EXTENSION_METADATA_DATA || totalSize != f->size ||
        length - offset != pieceLength(f, piece)) {
        f->pieceState[piece] = PIECE_MISSING;
        dropPeer(peer, "sent a metadata piece of the wrong size", 1);
        return;
    }
    memcpy(f->info + (size_t)piece * EXTENSION_METADATA_PIECE, data + offset, length - offset);
    f->pieceState[piece] = PIECE_DONE;
    f->pieceFrom[piece] = peer;
    f->piecesDone++;
    peer->requestedAt = uv_now(f->loop);
    if (f->piecesDone == f->pieces) {
        checkMetadata(f);
        return;
    }
    requestMore(f, peer);
}

static void receiveHandshake(metadataFetch *f, metaPeer *peer, const uint8_t *data, size_t length)
{
    extensionHandshake handshake;
    metaPeer          *other;

    if (extensionDecodeHandshake(data, length, &handshake) != EXTENSION_OK) {
        dropPeer(peer, "sent a malformed extended handshake", 1);
        return;
    }
    if (handshake.metadataId == 0 || handshake.metadataSize == 0) {
        int first = !peer->refused;

        if (first) {
            peer->refused = 1;
            f->refusals++;
        }
        dropPeer(peer, first ? "has the torrent but will not send its details" : NULL, 0);
        /* Peers will not share a private torrent's metadata (BEP 27): if
         * none offers it, say what that probably means, once. */
        if (!f->offered && !f->hinted && f->refusals >= 3u) {
            f->hinted = 1;
            emit(f, "%u peers have the torrent but none will send its details: it is most "
                 "likely a private torrent, which cannot be fetched from a magnet link. Use "
                 "the .torrent file instead.", (unsigned)f->refusals);
        }
        return;
    }
    f->offered = 1;
    peer->metadataId = handshake.metadataId;
    if (f->info == NULL) {
        /* The first size we hear; the hash will tell if it was a lie. */
        f->size = handshake.metadataSize;
        f->pieces = (f->size + EXTENSION_METADATA_PIECE - 1u) / EXTENSION_METADATA_PIECE;
        f->info = allocate(f, f->size);
        f->pieceState = allocate(f, f->pieces);
        f->pieceFrom = allocate(f, sizeof(*f->pieceFrom) * f->pieces);
        if (f->info == NULL || f->pieceState == NULL || f->pieceFrom == NULL) {
            release(f, f->info);
            release(f, f->pieceState);
            release(f, (void *)f->pieceFrom);
            f->info = NULL;
            f->pieceState = NULL;
            f->pieceFrom = NULL;
            dropPeer(peer, "metadata too large for memory", 0);
            return;
        }
        emit(f, "metadata is %u bytes in %u piece%s", (unsigned)f->size, (unsigned)f->pieces,
             (f->pieces == 1u) ? "" : "s");
        TAILQ_FOREACH(other, &f->peers, entries) {
            if (other != peer) {
                requestMore(f, other);              /* those waiting for a size */
            }
        }
    } else if (handshake.metadataSize != f->size) {
        dropPeer(peer, "states a different metadata size", 0);
        return;
    }
    requestMore(f, peer);
}

/* ------------------------------------------------------------------------ */
/* Peer events                                                              */
/* ------------------------------------------------------------------------ */

static void onHandshake(void *privateArg, peerConnection *connection,
                        const peerHandshakeResult *result)
{
    metaPeer          *peer = privateArg;
    metadataFetch     *f = peer->owner;
    extensionHandshake handshake;
    uint8_t            buffer[1 + 256];
    size_t             length;

    f->connecting--;
    if (result->status != PEER_OK) {
        peerClose(connection);
        peer->connection = NULL;
        if (result->status == PEER_ESELF || result->status == PEER_EHASH) {
            peer->excluded = 1;
        }
        requeue(f, peer);
        connectMore(f);
        return;
    }
    peer->state = PEER_OPEN;
    f->open++;
    if (!(result->extensions & PEER_EXTENSION_PROTOCOL)) {
        peer->excluded = 1;                         /* it never will */
        dropPeer(peer, "does not support the extension protocol", 0);
        return;
    }
    memset(&handshake, 0, sizeof(handshake));
    handshake.metadataId = (uint8_t)EXTENSION_METADATA_ID;
    handshake.port = f->listenPort;
    snprintf(handshake.client, sizeof(handshake.client), "%s", METADATA_CLIENT_NAME);
    buffer[0] = (uint8_t)EXTENSION_HANDSHAKE;
    if (extensionEncodeHandshake(&handshake, buffer + 1, sizeof(buffer) - 1u, &length) ==
        EXTENSION_OK) {
        sendExtended(peer, buffer, 1u + length);
    }
}

static void onMessage(void *privateArg, peerConnection *connection, const messageFrame *message)
{
    metaPeer      *peer = privateArg;
    metadataFetch *f = peer->owner;

    (void)connection;
    if (stopped(f) || peer->state != PEER_OPEN || message->id != EXTENSION_MESSAGE_ID ||
        message->payloadLength < 1u) {
        return;                                     /* bitfields etc. do not matter here */
    }
    if (message->payload[0] == EXTENSION_HANDSHAKE) {
        receiveHandshake(f, peer, message->payload + 1, message->payloadLength - 1u);
    } else if (message->payload[0] == EXTENSION_METADATA_ID) {
        receiveMetadata(f, peer, message->payload + 1, message->payloadLength - 1u);
    }
}

static void onClosed(void *privateArg, peerConnection *connection, int32_t status,
                     const char *errorMessage)
{
    metaPeer *peer = privateArg;

    (void)connection;
    (void)status;
    (void)errorMessage;
    if (!stopped(peer->owner)) {
        dropPeer(peer, NULL, 0);
    }
}

static void connectMore(metadataFetch *f)
{
    peerCallbacks callbacks = { onHandshake, onMessage, onClosed, NULL };
    metaPeer     *peer;
    uint64_t      now = uv_now(f->loop);

    TAILQ_FOREACH(peer, &f->peers, entries) {
        if (stopped(f) || f->connecting + f->open >= f->maxPeers) {
            return;
        }
        if (peer->state != PEER_QUEUED || peer->retryAt > now) {
            continue;
        }
        peer->attempts++;
        if (peerConnect((const struct sockaddr *)&peer->address, &f->connectOptions, &callbacks,
                        peer, &peer->connection) == PEER_OK) {
            peer->state = PEER_CONNECTING;
            f->connecting++;
        } else {
            peer->connection = NULL;
            peer->state = PEER_CLOSED;
        }
    }
}

static void onTick(uv_timer_t *timer)
{
    metadataFetch *f = timer->data;
    metaPeer      *peer;
    uint64_t       now = uv_now(f->loop);

    TAILQ_FOREACH(peer, &f->peers, entries) {
        if (stopped(f)) {
            return;
        }
        if (peer->state == PEER_OPEN && peer->requestCount > 0 &&
            now - peer->requestedAt >= (uint64_t)METADATA_REQUEST_SECONDS * 1000u) {
            dropPeer(peer, "stopped answering metadata requests", 0);
        }
    }
    connectMore(f);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t metadataBegin(uv_loop_t *loop, parserContext *context, const metadataOptions *options,
                      const metadataEvents *events, void *privateArg, metadataFetch **fetch)
{
    metadataFetch *f;

    if (fetch != NULL) {
        *fetch = NULL;
    }
    if (loop == NULL || context == NULL || options == NULL || fetch == NULL) {
        return METADATA_EINVAL;
    }
    f = context->allocator.realloc(NULL, sizeof(*f));
    if (f == NULL) {
        return METADATA_ENOMEM;
    }
    memset(f, 0, sizeof(*f));
    f->loop = loop;
    f->context = context;
    if (events != NULL) {
        f->events = *events;
    }
    f->privateArg = privateArg;
    f->maxPeers = (options->maxPeers != 0) ? options->maxPeers : METADATA_DEFAULT_MAX_PEERS;
    f->listenPort = options->listenPort;
    f->blocklist = options->blocklist;
    memcpy(f->connectOptions.infoHash, options->infoHash, PARSER_SHA1_LENGTH);
    memcpy(f->connectOptions.peerId, options->peerId, PEER_ID_LENGTH);
    f->connectOptions.pieceCount = 0;               /* unknown until we have it */
    f->connectOptions.timeoutSeconds = options->handshakeTimeoutSeconds;
    f->connectOptions.extensions = PEER_EXTENSION_PROTOCOL;
    TAILQ_INIT(&f->peers);
    uv_timer_init(loop, &f->timer);
    f->timer.data = f;
    uv_timer_start(&f->timer, onTick, METADATA_TICK_MS, METADATA_TICK_MS);
    *fetch = f;
    return METADATA_OK;
}

int32_t metadataAddPeer(metadataFetch *f, const struct sockaddr *address)
{
    metaPeer *peer;
    size_t    size;

    if (f == NULL || address == NULL ||
        (address->sa_family != AF_INET && address->sa_family != AF_INET6) || stopped(f)) {
        return METADATA_EINVAL;
    }
    if (blocklistContains(f->blocklist, address)) {
        f->blocked++;
        return METADATA_OK;                         /* quietly ignored */
    }
    size = (address->sa_family == AF_INET) ? sizeof(struct sockaddr_in)
                                           : sizeof(struct sockaddr_in6);
    TAILQ_FOREACH(peer, &f->peers, entries) {
        if (memcmp(&peer->address, address, size) == 0) {
            if (peer->state == PEER_CLOSED && !peer->excluded) {
                peer->state = PEER_QUEUED;          /* reported again: new chances */
                peer->attempts = 0;
                peer->retryAt = 0;
                connectMore(f);
            }
            return METADATA_OK;
        }
    }
    if (f->peerCount >= METADATA_MAX_KNOWN) {
        return METADATA_OK;
    }
    peer = allocate(f, sizeof(*peer));
    if (peer == NULL) {
        return METADATA_ENOMEM;
    }
    memcpy(&peer->address, address, size);
    formatAddress(address, peer->where, sizeof(peer->where));
    peer->owner = f;
    TAILQ_INSERT_TAIL(&f->peers, peer, entries);
    f->peerCount++;
    connectMore(f);
    return METADATA_OK;
}

void metadataGetProgress(const metadataFetch *f, metadataProgress *progress)
{
    const metaPeer *peer;

    if (progress == NULL) {
        return;
    }
    memset(progress, 0, sizeof(*progress));
    if (f == NULL) {
        return;
    }
    progress->size = f->size;
    progress->pieces = f->pieces;
    progress->piecesDone = f->piecesDone;
    progress->peersBlocked = f->blocked;
    TAILQ_FOREACH(peer, &f->peers, entries) {
        progress->peersQueued += (peer->state == PEER_QUEUED) ? 1u : 0u;
        progress->peersConnecting += (peer->state == PEER_CONNECTING) ? 1u : 0u;
        progress->peersConnected += (peer->state == PEER_OPEN) ? 1u : 0u;
        progress->peersWithMetadata += (peer->state == PEER_OPEN && peer->metadataId) ? 1u : 0u;
    }
}

static void onTimerClosed(uv_handle_t *handle)
{
    metadataFetch *f = handle->data;
    metaPeer      *peer;

    while ((peer = TAILQ_FIRST(&f->peers)) != NULL) {
        TAILQ_REMOVE(&f->peers, peer, entries);
        release(f, peer);
    }
    release(f, f->info);
    release(f, f->pieceState);
    release(f, (void *)f->pieceFrom);
    release(f, f);
}

void metadataEnd(metadataFetch *f)
{
    metaPeer *peer;

    if (f == NULL || f->ended) {
        return;
    }
    f->ended = 1;
    TAILQ_FOREACH(peer, &f->peers, entries) {
        peerClose(peer->connection);
        peer->connection = NULL;
    }
    uv_timer_stop(&f->timer);
    uv_close((uv_handle_t *)&f->timer, onTimerClosed);
}
