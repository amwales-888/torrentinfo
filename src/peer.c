/*
 * peer.c - BitTorrent peer connections using libuv (BEP 3 peer wire protocol).
 *
 * Handshake (68 bytes each way):
 *
 *   offset  0: 19 (length of the protocol name)
 *   offset  1: "BitTorrent protocol"
 *   offset 20: 8 reserved bytes (extension flags)
 *   offset 28: info hash (20)
 *   offset 48: peer id (20)
 *
 * The peer's handshake is checked as it arrives, so a server that is not
 * speaking BitTorrent fails after its first 20 bytes rather than at the
 * timeout. After the handshake, received bytes collect in a buffer that
 * grows to fit the largest message allowed; complete messages are decoded
 * (message.c), checked, applied to the connection state and handed to the
 * user, then the buffer is compacted.
 *
 * One timer serves two purposes: before the handshake it is the timeout;
 * afterwards it ticks every 15 seconds to send keep-alives and to drop
 * peers that have gone quiet.
 *
 * Lifetime: 'pending' counts libuv callbacks still to come (open handles
 * and writes in flight). A connection is freed only once peerClose() has
 * been called, pending is zero, and no callback for it is running.
 * 'dead' means the network side has ended (handles closing); the handle
 * itself stays valid until peerClose().
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "peer.h"
#include "queue.h"

#define PEER_STATE_MAGIC        0x50454552u     /* "PEER" */
#define PEER_PROTOCOL_NAME      "BitTorrent protocol"
#define PEER_PROTOCOL_LENGTH    19u
#define PEER_OFFSET_RESERVED    20u
#define PEER_OFFSET_INFO_HASH   28u
#define PEER_OFFSET_PEER_ID     48u
#define PEER_BUFFER_INITIAL     16384u
#ifndef PEER_TICK_SECONDS
#define PEER_TICK_SECONDS       15u             /* keep-alive / idle check interval */
#define PEER_INCOMING_SECONDS   30u             /* for an incoming peer's handshake */
#define PEER_LISTEN_BACKLOG     64
#endif

/* One message (or our handshake) on its way to the socket. */
typedef struct peerWrite {
    uv_write_t      write;
    peerConnection *connection;
    uint8_t         data[];
} peerWrite;

struct peerConnection {
    peerCallbacks           callbacks;
    void                   *privateArg;
    int32_t                 status;
    uint8_t                 handshakeDone;
    uint8_t                 dead;           /* network side ended              */
    uint8_t                 closed;         /* peerClose() called              */
    uint8_t                 inCallback;
    uint8_t                 incoming;       /* accepted from a listener        */
    uint8_t                 accepted;       /* ...and the user took it         */
    struct peerListener    *listener;       /* while incoming && !accepted     */
    uint32_t                pending;        /* libuv callbacks still to come   */
    uint32_t                timeoutSeconds;
    uint32_t                pieceCount;
    uint32_t                maxMessage;     /* largest message length accepted */

    struct sockaddr_storage address;
    uint8_t                 infoHash[PARSER_SHA1_LENGTH];
    uint8_t                 localPeerId[PEER_ID_LENGTH];
    uint8_t                 handshake[PEER_HANDSHAKE_LENGTH];  /* ours */

    uv_tcp_t                tcp;
    uv_timer_t              timer;
    uv_connect_t            connectRequest;
    uint8_t                 tcpOpen;
    uint8_t                 timerOpen;

    uint8_t                *buffer;         /* received, not yet processed     */
    uint32_t                received;
    uint32_t                capacity;

    /* The peer, from its handshake and messages */
    uint8_t                 remotePeerId[PEER_ID_LENGTH];
    uint8_t                 reserved[PEER_RESERVED_LENGTH];
    uint32_t                extensions;
    uint8_t                 amChoking;
    uint8_t                 amInterested;
    uint8_t                 peerChoking;
    uint8_t                 peerInterested;
    uint8_t                *have;           /* bitfield of pieceCount bits     */
    uint32_t                haveCount;
    uint32_t                messagesReceived;
    uint32_t                coreMessages;   /* those the bitfield must precede */
    uint64_t                bytesReceived;
    uint64_t                bytesSent;
    uint64_t                lastReceive;    /* uv_now() milliseconds           */
    uint64_t                lastSend;

    char                    errorMessage[PEER_ERROR_MESSAGE_LENGTH];
    TAILQ_ENTRY(peerConnection) entries;
};
TAILQ_HEAD(peerConnectionList, peerConnection);

struct peerListener {
    uv_tcp_t                tcp;
    peerIncomingCallback    callback;
    void                   *privateArg;
    uint16_t                port;
    uint8_t                 closed;
    uint32_t                pendingConnections; /* accepted, info hash not seen */
};

static struct {
    uv_loop_t                *loop;
    parserContext            *context;
    uint32_t                  users;        /* peerInit() calls outstanding */
    netBinding                binding;      /* interface to use, if active  */
    uint32_t                  magic;
    struct peerConnectionList connections;
} peerModule;

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static void *allocate(uint64_t size)
{
    void *memory;

    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    memory = peerModule.context->allocator.realloc(NULL, (size_t)size);
    if (memory != NULL) {
        memset(memory, 0, (size_t)size);
    }
    return memory;
}

static void *resize(void *memory, uint64_t size)
{
    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    return peerModule.context->allocator.realloc(memory, (size_t)size);
}

static void release(void *memory)
{
    if (memory != NULL) {
        peerModule.context->allocator.free(memory);
    }
}

static void setError(peerConnection *connection, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(connection->errorMessage, sizeof(connection->errorMessage), format, arguments);
    va_end(arguments);
}

/* ======================================================================== */
/* Lifetime                                                                 */
/* ======================================================================== */

static void maybeFree(peerConnection *connection)
{
    if (connection->closed && !connection->inCallback && connection->pending == 0) {
        release(connection->buffer);
        release(connection->have);
        release(connection);
    }
}

static void onHandleClosed(uv_handle_t *handle)
{
    peerConnection *connection = handle->data;

    if (handle == (uv_handle_t *)&connection->tcp) {
        connection->tcpOpen = 0;
    } else {
        connection->timerOpen = 0;
    }
    connection->pending--;
    maybeFree(connection);
}

static void closeAll(peerConnection *connection)
{
    if (connection->timerOpen && !uv_is_closing((uv_handle_t *)&connection->timer)) {
        uv_timer_stop(&connection->timer);
        uv_close((uv_handle_t *)&connection->timer, onHandleClosed);
    }
    if (connection->tcpOpen && !uv_is_closing((uv_handle_t *)&connection->tcp)) {
        uv_close((uv_handle_t *)&connection->tcp, onHandleClosed);   /* cancels writes */
    }
}

/* A closed listener goes once its handle has closed and no connection still
 * waiting for an info hash points at it. */
static void maybeFreeListener(struct peerListener *listener)
{
    if (listener->closed && listener->tcp.data == NULL && listener->pendingConnections == 0) {
        release(listener);
    }
}

/* An incoming connection no longer waits on its listener. */
static void detachListener(peerConnection *connection)
{
    struct peerListener *listener = connection->listener;

    if (listener != NULL) {
        connection->listener = NULL;
        listener->pendingConnections--;
        maybeFreeListener(listener);
    }
}

/* Tell the user how the handshake went. */
static void deliverHandshake(peerConnection *connection, int32_t status)
{
    peerHandshakeResult result;

    if (connection->callbacks.onHandshake == NULL) {
        return;
    }
    memset(&result, 0, sizeof(result));
    result.status = status;
    memcpy(result.peerId, connection->remotePeerId, PEER_ID_LENGTH);
    memcpy(result.reserved, connection->reserved, PEER_RESERVED_LENGTH);
    result.extensions = connection->extensions;
    result.errorMessage = (status == PEER_OK) ? NULL : connection->errorMessage;

    /* inCallback stops peerClose()/peerShutdown() freeing the connection
     * while we are still using it. */
    connection->inCallback = 1;
    connection->callbacks.onHandshake(connection->privateArg, connection, &result);
    connection->inCallback = 0;
}

/*
 * End the network side and report it: through onHandshake if the handshake
 * had not finished, otherwise through onClosed. Nothing is reported after
 * peerClose().
 */
static void terminate(peerConnection *connection, int32_t status)
{
    if (connection->dead) {
        return;
    }
    connection->dead = 1;
    connection->status = status;
    if (connection->errorMessage[0] == '\0') {
        setError(connection, "%s", peerStatusString(status));
    }
    closeAll(connection);
    if (connection->closed) {
        return;
    }
    if (connection->incoming && !connection->accepted) {
        /* Nobody has the handle yet: nothing to report. */
        detachListener(connection);
        connection->closed = 1;
        TAILQ_REMOVE(&peerModule.connections, connection, entries);
        maybeFree(connection);
        return;
    }

    if (!connection->handshakeDone) {
        deliverHandshake(connection, status);
    } else if (connection->callbacks.onClosed != NULL) {
        connection->inCallback = 1;
        connection->callbacks.onClosed(connection->privateArg, connection, status,
                                       connection->errorMessage);
        connection->inCallback = 0;
    }
    maybeFree(connection);
}

/* ======================================================================== */
/* Sending                                                                  */
/* ======================================================================== */

static void onWrite(uv_write_t *write, int status)
{
    peerWrite      *chunk = write->data;
    peerConnection *connection = chunk->connection;

    release(chunk);
    connection->pending--;
    if (connection->dead) {
        maybeFree(connection);
        return;
    }
    if (status < 0) {
        setError(connection, "send failed: %s", uv_strerror(status));
        terminate(connection, PEER_ERROR);
        return;
    }
    if (connection->handshakeDone && connection->callbacks.onDrain != NULL &&
        uv_stream_get_write_queue_size((const uv_stream_t *)&connection->tcp) == 0) {
        connection->inCallback = 1;
        connection->callbacks.onDrain(connection->privateArg, connection);
        connection->inCallback = 0;
        maybeFree(connection);
    }
}

/* Queue bytes for the socket; they are copied. */
static int32_t sendBytes(peerConnection *connection, const uint8_t *data, uint32_t length,
                         const messageFrame *message)
{
    peerWrite *chunk = allocate(sizeof(*chunk) + length);
    uint32_t   written = length;
    uv_buf_t   buffer;

    if (chunk == NULL) {
        return PEER_ENOMEM;
    }
    if (message != NULL) {
        messageEncode(message, chunk->data, length, &written);
    } else {
        memcpy(chunk->data, data, length);
    }
    chunk->connection = connection;
    chunk->write.data = chunk;
    buffer = uv_buf_init((char *)chunk->data, written);
    if (uv_write(&chunk->write, (uv_stream_t *)&connection->tcp, &buffer, 1, onWrite) < 0) {
        release(chunk);
        return PEER_ERROR;
    }
    connection->pending++;
    connection->lastSend = uv_now(peerModule.loop);
    return PEER_OK;
}

/* ======================================================================== */
/* Receiving messages                                                       */
/* ======================================================================== */

/* Check a message against the protocol and update our view of the peer. */
static int32_t applyMessage(peerConnection *connection, const messageFrame *message)
{
    switch (message->id) {
    case MESSAGE_KEEP_ALIVE:
        return PEER_OK;                             /* not counted as a message */
    case MESSAGE_CHOKE:
        connection->peerChoking = 1;
        break;
    case MESSAGE_UNCHOKE:
        connection->peerChoking = 0;
        break;
    case MESSAGE_INTERESTED:
        connection->peerInterested = 1;
        break;
    case MESSAGE_NOT_INTERESTED:
        connection->peerInterested = 0;
        break;
    case MESSAGE_HAVE:
        if (message->index >= connection->pieceCount) {
            setError(connection, "have for piece %" PRIu32 ", but the torrent has %" PRIu32
                     " pieces", message->index, connection->pieceCount);
            return PEER_EPROTO;
        }
        if (!messageBitfieldGet(connection->have, message->index)) {
            messageBitfieldSet(connection->have, message->index);
            connection->haveCount++;
        }
        break;
    case MESSAGE_BITFIELD:
        if (connection->coreMessages > 0) {
            setError(connection, "bitfield sent after other messages");
            return PEER_EPROTO;
        }
        if (!messageBitfieldValid(message->payload, message->payloadLength,
                                  connection->pieceCount)) {
            setError(connection, "invalid bitfield (%" PRIu32 " bytes for %" PRIu32 " pieces)",
                     message->payloadLength, connection->pieceCount);
            return PEER_EPROTO;
        }
        if (message->payloadLength > 0) {
            memcpy(connection->have, message->payload, message->payloadLength);
        }
        connection->haveCount = messageBitfieldCount(connection->have, connection->pieceCount);
        break;
    case MESSAGE_REQUEST:
    case MESSAGE_CANCEL:
        if (message->index >= connection->pieceCount || message->length == 0 ||
            message->length > MESSAGE_MAX_BLOCK_LENGTH) {
            setError(connection, "invalid %s (piece %" PRIu32 ", %" PRIu32 " bytes)",
                     messageName(message->id), message->index, message->length);
            return PEER_EPROTO;
        }
        break;
    case MESSAGE_PIECE:
        if (message->index >= connection->pieceCount) {
            setError(connection, "piece %" PRIu32 " is out of range", message->index);
            return PEER_EPROTO;
        }
        break;
    default:                                        /* port, extension messages */
        break;
    }
    /* The bitfield must come before the other core messages (BEP 3), but
     * the extended handshake (BEP 10), a DHT port message (BEP 5) or a
     * keep-alive may come before it: clients send their extended
     * handshake straight after the handshake. */
    if (message->id != 20u && message->id != MESSAGE_PORT && message->id != MESSAGE_KEEP_ALIVE) {
        connection->coreMessages++;
    }
    connection->messagesReceived++;
    return PEER_OK;
}

/*
 * Handle every complete message in the buffer, then compact it and make
 * sure it can hold the next message in full.
 */
static void processMessages(peerConnection *connection)
{
    messageFrame frame;
    uint32_t     offset = 0;
    uint32_t     consumed;
    uint32_t     needed = 0;
    uint8_t     *grown;
    int32_t      result;

    while (!connection->dead) {
        result = messageDecode(connection->buffer + offset, connection->received - offset,
                               connection->maxMessage, &frame, &consumed);
        if (result == MESSAGE_INCOMPLETE) {
            needed = consumed;                      /* 0 if the length is not in yet */
            break;
        }
        if (result != MESSAGE_OK) {
            if (result == MESSAGE_ETOOLARGE) {
                setError(connection, "message too large (limit %" PRIu32 " bytes)",
                         connection->maxMessage);
            } else {
                setError(connection, "malformed %s message", messageName(frame.id));
            }
            terminate(connection, PEER_EPROTO);
            return;
        }
        result = applyMessage(connection, &frame);
        if (result != PEER_OK) {
            terminate(connection, result);
            return;
        }
        offset += consumed;

        if (connection->callbacks.onMessage != NULL) {
            connection->inCallback = 1;
            connection->callbacks.onMessage(connection->privateArg, connection, &frame);
            connection->inCallback = 0;
            if (connection->closed) {
                maybeFree(connection);
                return;
            }
        }
    }
    if (connection->dead) {
        return;
    }

    if (offset > 0) {
        memmove(connection->buffer, connection->buffer + offset, connection->received - offset);
        connection->received -= offset;
    }
    if (needed > connection->capacity) {
        grown = resize(connection->buffer, needed);
        if (grown == NULL) {
            terminate(connection, PEER_ENOMEM);
            return;
        }
        connection->buffer = grown;
        connection->capacity = needed;
    }
}

/* ======================================================================== */
/* Handshake                                                                */
/* ======================================================================== */

/* Keep whatever of the peer's handshake has arrived, for reporting. */
static void captureRemote(peerConnection *connection)
{
    const uint8_t *reserved = connection->buffer + PEER_OFFSET_RESERVED;

    if (connection->received >= PEER_OFFSET_INFO_HASH) {
        memcpy(connection->reserved, reserved, PEER_RESERVED_LENGTH);
        connection->extensions = ((reserved[5] & 0x10u) ? PEER_EXTENSION_PROTOCOL : 0u) |
                                 ((reserved[7] & 0x04u) ? PEER_EXTENSION_FAST : 0u) |
                                 ((reserved[7] & 0x01u) ? PEER_EXTENSION_DHT : 0u);
    }
    if (connection->received >= PEER_HANDSHAKE_LENGTH) {
        memcpy(connection->remotePeerId, connection->buffer + PEER_OFFSET_PEER_ID,
               PEER_ID_LENGTH);
    }
}

/*
 * Check what has arrived so far. Returns PEER_OK when the handshake is
 * complete and valid, 1 if more bytes are needed, or an error status.
 */
static int32_t checkHandshake(peerConnection *connection)
{
    const uint8_t *data = connection->buffer;
    uint32_t       length = connection->received;
    uint32_t       compare;

    if (length >= 1u && data[0] != PEER_PROTOCOL_LENGTH) {
        setError(connection, "not a BitTorrent peer (first byte %u)", (unsigned)data[0]);
        return PEER_EPROTO;
    }
    /* Compare as much of the protocol name as we have. */
    compare = (length > 1u) ? length - 1u : 0u;
    if (compare > PEER_PROTOCOL_LENGTH) {
        compare = PEER_PROTOCOL_LENGTH;
    }
    if (compare > 0u && memcmp(data + 1, PEER_PROTOCOL_NAME, compare) != 0) {
        setError(connection, "not a BitTorrent peer (unknown protocol name)");
        return PEER_EPROTO;
    }
    if (length < PEER_HANDSHAKE_LENGTH) {
        return 1;
    }
    if (memcmp(data + PEER_OFFSET_INFO_HASH, connection->infoHash, PARSER_SHA1_LENGTH) != 0) {
        setError(connection, "peer is serving a different torrent (info hash mismatch)");
        return PEER_EHASH;
    }
    if (memcmp(data + PEER_OFFSET_PEER_ID, connection->localPeerId, PEER_ID_LENGTH) == 0) {
        setError(connection, "connected to ourselves (peer id matches ours)");
        return PEER_ESELF;
    }
    return PEER_OK;
}

/* After the handshake: keep-alives and the idle limit. */
static void onTick(uv_timer_t *timer)
{
    peerConnection *connection = timer->data;
    uint64_t        now = uv_now(peerModule.loop);
    messageFrame    keepAlive;

    if (connection->dead) {
        return;
    }
    if (now - connection->lastReceive >= (uint64_t)PEER_IDLE_SECONDS * 1000u) {
        setError(connection, "nothing received from the peer for %u seconds",
                 (unsigned)PEER_IDLE_SECONDS);
        terminate(connection, PEER_ETIMEDOUT);
        return;
    }
    if (now - connection->lastSend >= (uint64_t)PEER_KEEP_ALIVE_SECONDS * 1000u) {
        memset(&keepAlive, 0, sizeof(keepAlive));
        keepAlive.id = MESSAGE_KEEP_ALIVE;
        peerSend(connection, &keepAlive);
    }
}

/* The handshake checked out: switch to message mode. */
static void handshakeComplete(peerConnection *connection)
{
    uint32_t bitfieldBytes = messageBitfieldBytes(connection->pieceCount);

    if (bitfieldBytes > 0) {
        connection->have = allocate(bitfieldBytes);
        if (connection->have == NULL) {
            terminate(connection, PEER_ENOMEM);
            return;
        }
    }
    connection->handshakeDone = 1;

    /* Anything after the handshake is the start of the message stream. */
    connection->received -= PEER_HANDSHAKE_LENGTH;
    memmove(connection->buffer, connection->buffer + PEER_HANDSHAKE_LENGTH, connection->received);
    connection->bytesReceived = connection->received;

    connection->lastReceive = uv_now(peerModule.loop);
    uv_timer_stop(&connection->timer);
    uv_timer_start(&connection->timer, onTick, (uint64_t)PEER_TICK_SECONDS * 1000u,
                   (uint64_t)PEER_TICK_SECONDS * 1000u);

    deliverHandshake(connection, PEER_OK);
    if (connection->closed) {
        maybeFree(connection);
        return;
    }
    processMessages(connection);
}

static void onTimeout(uv_timer_t *timer)
{
    peerConnection *connection = timer->data;

    setError(connection, "no handshake within %" PRIu32 " seconds", connection->timeoutSeconds);
    terminate(connection, PEER_ETIMEDOUT);
}

static void onRead(uv_stream_t *stream, ssize_t count, const uv_buf_t *buffer);

/* Settings from peerConnectOptions, for either direction. */
static void configure(peerConnection *connection, const peerConnectOptions *options)
{
    uint32_t bitfieldMessage;

    connection->timeoutSeconds = options->timeoutSeconds;
    connection->pieceCount = options->pieceCount;
    memcpy(connection->infoHash, options->infoHash, PARSER_SHA1_LENGTH);
    memcpy(connection->localPeerId, options->peerId, PEER_ID_LENGTH);
    connection->amChoking = 1;                      /* BEP 3 initial state */
    connection->peerChoking = 1;

    /* Largest message accepted: a piece message with a 128 KiB block, or
     * a bitfield for this torrent, whichever is bigger. */
    connection->maxMessage = 1u + 8u + MESSAGE_MAX_BLOCK_LENGTH;
    bitfieldMessage = 1u + messageBitfieldBytes(options->pieceCount);
    if (bitfieldMessage > connection->maxMessage) {
        connection->maxMessage = bitfieldMessage;
    }

    /* Our handshake: reserved bytes zero except the extensions we support. */
    connection->handshake[0] = (uint8_t)PEER_PROTOCOL_LENGTH;
    memcpy(connection->handshake + 1, PEER_PROTOCOL_NAME, PEER_PROTOCOL_LENGTH);
    memset(connection->handshake + PEER_OFFSET_RESERVED, 0, PEER_RESERVED_LENGTH);
    if (options->extensions & PEER_EXTENSION_PROTOCOL) {
        connection->handshake[PEER_OFFSET_RESERVED + 5u] = 0x10u;   /* BEP 10 */
    }
    memcpy(connection->handshake + PEER_OFFSET_INFO_HASH, options->infoHash, PARSER_SHA1_LENGTH);
    memcpy(connection->handshake + PEER_OFFSET_PEER_ID, options->peerId, PEER_ID_LENGTH);
}

/*
 * An incoming connection: once its info hash has arrived, ask the listener's
 * user whether to take it; if so, answer with our handshake. Returns PEER_OK
 * (accepted), 1 (need more bytes) or an error.
 */
static int32_t routeIncoming(peerConnection *connection)
{
    struct peerListener *listener = connection->listener;
    peerConnectOptions   options;
    peerCallbacks        callbacks;
    void                *privateArg = NULL;
    uint8_t              infoHash[PARSER_SHA1_LENGTH];
    int                  accept;
    int32_t              result;

    if (connection->received >= 1u && connection->buffer[0] != PEER_PROTOCOL_LENGTH) {
        setError(connection, "not a BitTorrent peer");
        return PEER_EPROTO;
    }
    if (connection->received < PEER_OFFSET_PEER_ID) {
        return 1;
    }
    if (memcmp(connection->buffer + 1, PEER_PROTOCOL_NAME, PEER_PROTOCOL_LENGTH) != 0) {
        setError(connection, "not a BitTorrent peer (unknown protocol name)");
        return PEER_EPROTO;
    }
    memcpy(infoHash, connection->buffer + PEER_OFFSET_INFO_HASH, PARSER_SHA1_LENGTH);
    connection->listener = NULL;
    listener->pendingConnections--;
    if (listener->closed || listener->callback == NULL) {
        maybeFreeListener(listener);
        setError(connection, "no longer accepting connections");
        return PEER_EHASH;
    }
    memset(&options, 0, sizeof(options));
    memset(&callbacks, 0, sizeof(callbacks));
    accept = listener->callback(listener->privateArg, connection,
                                (const struct sockaddr *)&connection->address, infoHash,
                                &options, &callbacks, &privateArg);
    if (!accept) {
        setError(connection, "not serving that torrent");
        return PEER_EHASH;
    }
    configure(connection, &options);
    connection->callbacks = callbacks;
    connection->privateArg = privateArg;
    connection->accepted = 1;
    uv_timer_stop(&connection->timer);
    if (options.timeoutSeconds > 0) {
        uv_timer_start(&connection->timer, onTimeout,
                       (uint64_t)options.timeoutSeconds * 1000u, 0);
    }
    result = sendBytes(connection, connection->handshake, PEER_HANDSHAKE_LENGTH, NULL);
    if (result != PEER_OK) {
        setError(connection, "could not send the handshake");
    }
    return result;
}

static void onAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    peerConnection *connection = handle->data;

    (void)suggested;
    /* A zero-length buffer (only if full) makes libuv report UV_ENOBUFS. */
    *buffer = uv_buf_init((char *)connection->buffer + connection->received,
                          connection->capacity - connection->received);
}

static void onRead(uv_stream_t *stream, ssize_t count, const uv_buf_t *buffer)
{
    peerConnection *connection = stream->data;
    int32_t         result;

    (void)buffer;
    if (connection->dead) {
        return;
    }
    if (count > 0) {
        connection->received += (uint32_t)count;
        connection->lastReceive = uv_now(peerModule.loop);
        if (connection->handshakeDone) {
            connection->bytesReceived += (uint64_t)count;
            processMessages(connection);
            return;
        }
        if (connection->incoming && !connection->accepted) {
            result = routeIncoming(connection);
            if (result == 1) {
                return;                             /* wait for the info hash */
            }
            if (result != PEER_OK) {
                terminate(connection, result);
                return;
            }
        }
        result = checkHandshake(connection);
        if (result == 1) {
            return;                                 /* wait for more */
        }
        captureRemote(connection);
        if (result == PEER_OK) {
            handshakeComplete(connection);
        } else {
            terminate(connection, result);
        }
    } else if (count == UV_EOF) {
        if (connection->handshakeDone) {
            setError(connection, "peer closed the connection");
        } else {
            captureRemote(connection);
            setError(connection, "peer closed the connection during the handshake "
                     "(after %" PRIu32 " bytes)", connection->received);
        }
        terminate(connection, PEER_ECLOSED);
    } else if (count == UV_ENOBUFS) {
        terminate(connection, PEER_ENOMEM);
    } else if (count < 0) {
        setError(connection, "receive failed: %s", uv_strerror((int)count));
        terminate(connection, PEER_ERROR);
    }
}

static void onConnect(uv_connect_t *connect, int status)
{
    peerConnection *connection = connect->data;
    int32_t         result;

    connection->pending--;
    if (connection->dead) {
        maybeFree(connection);
        return;
    }
    if (status < 0) {
        setError(connection, "connect failed: %s", uv_strerror(status));
        terminate(connection, PEER_ERROR);
        return;
    }
    uv_tcp_nodelay(&connection->tcp, 1);            /* small messages: no batching delay */

    /* Send our handshake and start reading theirs; peers may send theirs
     * before or after receiving ours. */
    result = sendBytes(connection, connection->handshake, PEER_HANDSHAKE_LENGTH, NULL);
    if (result != PEER_OK) {
        setError(connection, "could not send the handshake");
        terminate(connection, result);
        return;
    }
    if (uv_read_start((uv_stream_t *)&connection->tcp, onAlloc, onRead) < 0) {
        setError(connection, "could not start reading");
        terminate(connection, PEER_ERROR);
    }
}

/* ======================================================================== */
/* Listening                                                                */
/* ======================================================================== */

static void onListenerClosed(uv_handle_t *handle)
{
    struct peerListener *listener = handle->data;

    listener->tcp.data = NULL;
    maybeFreeListener(listener);
}

static void onIncomingTimeout(uv_timer_t *timer)
{
    peerConnection *connection = timer->data;

    setError(connection, "no handshake within %u seconds", (unsigned)PEER_INCOMING_SECONDS);
    terminate(connection, PEER_ETIMEDOUT);
}

static void onConnection(uv_stream_t *server, int status)
{
    struct peerListener *listener = server->data;
    peerConnection      *connection;
    int                  length = (int)sizeof(struct sockaddr_storage);

    if (status < 0 || listener->closed || peerModule.magic != PEER_STATE_MAGIC) {
        return;
    }
    connection = allocate(sizeof(*connection));
    if (connection == NULL) {
        return;
    }
    connection->buffer = allocate(PEER_BUFFER_INITIAL);
    if (connection->buffer == NULL) {
        release(connection);
        return;
    }
    connection->capacity = PEER_BUFFER_INITIAL;
    connection->incoming = 1;
    connection->amChoking = 1;
    connection->peerChoking = 1;

    uv_timer_init(peerModule.loop, &connection->timer);
    connection->timer.data = connection;
    connection->timerOpen = 1;
    connection->pending++;
    uv_tcp_init(peerModule.loop, &connection->tcp);
    connection->tcp.data = connection;
    connection->tcpOpen = 1;
    connection->pending++;
    TAILQ_INSERT_TAIL(&peerModule.connections, connection, entries);

    if (uv_accept(server, (uv_stream_t *)&connection->tcp) != 0 ||
        uv_tcp_getpeername(&connection->tcp, (struct sockaddr *)&connection->address,
                           &length) != 0 ||
        uv_read_start((uv_stream_t *)&connection->tcp, onAlloc, onRead) != 0) {
        terminate(connection, PEER_ERROR);
        return;
    }
    uv_tcp_nodelay(&connection->tcp, 1);
    connection->listener = listener;
    listener->pendingConnections++;
    uv_timer_start(&connection->timer, onIncomingTimeout,
                   (uint64_t)PEER_INCOMING_SECONDS * 1000u, 0);
}

int32_t peerListen(int family, uint16_t port, peerIncomingCallback callback, void *privateArg,
                   peerListener **handle)
{
    struct peerListener *listener;
    struct sockaddr_storage local;
    int                  length = (int)sizeof(local);
    int                  result;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (peerModule.magic != PEER_STATE_MAGIC) {
        return PEER_ENOTINIT;
    }
    if (handle == NULL || callback == NULL || (family != AF_INET && family != AF_INET6)) {
        return PEER_EINVAL;
    }
    listener = allocate(sizeof(*listener));
    if (listener == NULL) {
        return PEER_ENOMEM;
    }
    listener->callback = callback;
    listener->privateArg = privateArg;
    result = netTcpInit(peerModule.loop, &listener->tcp, &peerModule.binding, family);
    if (result != 0) {
        release(listener);
        return PEER_ERROR;
    }
    listener->tcp.data = listener;
    result = netTcpListenBind(&listener->tcp, &peerModule.binding, family, port);
    if (result == 0) {
        result = uv_listen((uv_stream_t *)&listener->tcp, PEER_LISTEN_BACKLOG, onConnection);
    }
    if (result == 0) {
        result = uv_tcp_getsockname(&listener->tcp, (struct sockaddr *)&local, &length);
    }
    if (result != 0) {
        listener->closed = 1;
        uv_close((uv_handle_t *)&listener->tcp, onListenerClosed);
        return (result == UV_EADDRINUSE) ? PEER_EADDRINUSE : PEER_ERROR;
    }
    listener->port = ntohs((local.ss_family == AF_INET)
                               ? ((struct sockaddr_in *)&local)->sin_port
                               : ((struct sockaddr_in6 *)&local)->sin6_port);
    *handle = listener;
    return PEER_OK;
}

uint16_t peerListenerPort(const peerListener *listener)
{
    return (listener != NULL) ? listener->port : 0;
}

void peerListenerClose(peerListener *listener)
{
    if (listener == NULL || listener->closed) {
        return;
    }
    listener->closed = 1;
    uv_close((uv_handle_t *)&listener->tcp, onListenerClosed);
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t peerInit(uv_loop_t *loop, parserContext *context, const netBinding *binding)
{
    if (loop == NULL || context == NULL || context->magic != PARSER_CONTEXT_MAGIC) {
        return PEER_EINVAL;
    }
    if (peerModule.magic == PEER_STATE_MAGIC) {
        if (loop != peerModule.loop || context != peerModule.context) {
            return PEER_EINVAL;
        }
        peerModule.users++;
        return PEER_OK;
    }
    peerModule.loop = loop;
    peerModule.context = context;
    if (binding != NULL) {
        peerModule.binding = *binding;
    } else {
        memset(&peerModule.binding, 0, sizeof(peerModule.binding));
    }
    peerModule.users = 1;
    TAILQ_INIT(&peerModule.connections);
    peerModule.magic = PEER_STATE_MAGIC;
    return PEER_OK;
}

int32_t peerConnect(const struct sockaddr *address, const peerConnectOptions *options,
                    const peerCallbacks *callbacks, void *privateArg,
                    peerConnection **handle)
{
    peerConnection *connection;
    size_t          addressLength;
    int             result;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (peerModule.magic != PEER_STATE_MAGIC) {
        return PEER_ENOTINIT;
    }
    if (address == NULL || options == NULL || handle == NULL) {
        return PEER_EINVAL;
    }
    if (address->sa_family == AF_INET) {
        addressLength = sizeof(struct sockaddr_in);
    } else if (address->sa_family == AF_INET6) {
        addressLength = sizeof(struct sockaddr_in6);
    } else {
        return PEER_EINVAL;
    }

    connection = allocate(sizeof(*connection));
    if (connection == NULL) {
        return PEER_ENOMEM;
    }
    connection->buffer = allocate(PEER_BUFFER_INITIAL);
    if (connection->buffer == NULL) {
        release(connection);
        return PEER_ENOMEM;
    }
    connection->capacity = PEER_BUFFER_INITIAL;
    if (callbacks != NULL) {
        connection->callbacks = *callbacks;
    }
    connection->privateArg = privateArg;
    memcpy(&connection->address, address, addressLength);
    configure(connection, options);

    /* From here on, failures must close what has been opened. The timer is
     * always needed: handshake timeout first, keep-alive ticks later. */
    connection->closed = 1;                         /* not listed until started */
    uv_timer_init(peerModule.loop, &connection->timer);
    connection->timer.data = connection;
    connection->timerOpen = 1;
    connection->pending++;
    if (options->timeoutSeconds > 0) {
        uv_timer_start(&connection->timer, onTimeout,
                       (uint64_t)options->timeoutSeconds * 1000u, 0);
    }

    result = netTcpInit(peerModule.loop, &connection->tcp, &peerModule.binding,
                        connection->address.ss_family);
    if (result == 0) {
        connection->tcp.data = connection;
        connection->tcpOpen = 1;
        connection->pending++;
        result = netTcpBind(&connection->tcp, &peerModule.binding,
                            connection->address.ss_family);
    }
    if (result == 0) {
        connection->connectRequest.data = connection;
        result = uv_tcp_connect(&connection->connectRequest, &connection->tcp,
                                (const struct sockaddr *)&connection->address, onConnect);
    }
    if (result < 0) {
        connection->dead = 1;
        closeAll(connection);
        maybeFree(connection);                      /* once the handles close */
        return PEER_ERROR;
    }
    connection->pending++;

    connection->closed = 0;
    TAILQ_INSERT_TAIL(&peerModule.connections, connection, entries);
    *handle = connection;
    return PEER_OK;
}

int32_t peerSend(peerConnection *connection, const messageFrame *message)
{
    uint32_t length;
    int32_t  result;

    if (connection == NULL || message == NULL || connection->closed) {
        return PEER_EINVAL;
    }
    if (connection->dead) {
        return PEER_ECLOSED;
    }
    if (!connection->handshakeDone) {
        return PEER_EINVAL;
    }
    length = messageEncodedLength(message);
    if (length == 0 ||
        (message->id == MESSAGE_HAVE && message->index >= connection->pieceCount) ||
        (message->id == MESSAGE_BITFIELD &&
         ((message->payload == NULL && message->payloadLength > 0) ||
          !messageBitfieldValid(message->payload, message->payloadLength,
                                connection->pieceCount)))) {
        return PEER_EINVAL;
    }

    result = sendBytes(connection, NULL, length, message);
    if (result != PEER_OK) {
        return result;
    }
    connection->bytesSent += length;
    switch (message->id) {
    case MESSAGE_CHOKE:          connection->amChoking = 1;    break;
    case MESSAGE_UNCHOKE:        connection->amChoking = 0;    break;
    case MESSAGE_INTERESTED:     connection->amInterested = 1; break;
    case MESSAGE_NOT_INTERESTED: connection->amInterested = 0; break;
    default:                                                   break;
    }
    return PEER_OK;
}

int32_t peerGetState(peerConnection *connection, peerRemoteState *state)
{
    if (connection == NULL || state == NULL || connection->closed) {
        return PEER_EINVAL;
    }
    memset(state, 0, sizeof(*state));
    state->connected = (uint8_t)(connection->handshakeDone && !connection->dead);
    state->amChoking = connection->amChoking;
    state->amInterested = connection->amInterested;
    state->peerChoking = connection->peerChoking;
    state->peerInterested = connection->peerInterested;
    state->pieceCount = connection->pieceCount;
    state->haveCount = connection->haveCount;
    state->have = connection->have;
    state->messagesReceived = connection->messagesReceived;
    state->bytesReceived = connection->bytesReceived;
    state->bytesSent = connection->bytesSent;
    memcpy(state->peerId, connection->remotePeerId, PEER_ID_LENGTH);
    state->extensions = connection->extensions;
    state->incoming = connection->incoming;
    state->sendQueued = connection->tcpOpen
        ? (uint64_t)uv_stream_get_write_queue_size((const uv_stream_t *)&connection->tcp) : 0u;
    return PEER_OK;
}

void peerClose(peerConnection *connection)
{
    if (connection == NULL || connection->closed) {
        return;
    }
    connection->closed = 1;
    connection->dead = 1;                           /* no callbacks from now on */
    detachListener(connection);
    TAILQ_REMOVE(&peerModule.connections, connection, entries);
    closeAll(connection);
    maybeFree(connection);
}

void peerShutdown(void)
{
    peerConnection *connection;

    if (peerModule.magic != PEER_STATE_MAGIC || --peerModule.users > 0) {
        return;
    }
    while ((connection = TAILQ_FIRST(&peerModule.connections)) != NULL) {
        peerClose(connection);
    }
    /* loop and context stay set: closing connections still need the allocator. */
    peerModule.magic = 0;
}

const char *peerStatusString(int32_t status)
{
    switch (status) {
    case PEER_OK:        return "OK";
    case PEER_ERROR:     return "Network error";
    case PEER_EINVAL:    return "Invalid argument";
    case PEER_ENOMEM:    return "Out of memory";
    case PEER_ETIMEDOUT: return "Timed out";
    case PEER_EPROTO:    return "Protocol error";
    case PEER_ENOTINIT:  return "Peer module not initialised";
    case PEER_EHASH:     return "Different torrent";
    case PEER_ESELF:     return "Connected to ourselves";
    case PEER_EADDRINUSE: return "Port already in use";
    case PEER_ECLOSED:   return "Connection closed";
    default:             return "Unknown status";
    }
}
