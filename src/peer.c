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
 * Protocol encryption (MSE/PE, see crypto.h) runs before the handshake
 * when enabled. Outgoing (we are "A"):
 *
 *   A -> B  Ya, PadA
 *   B -> A  Yb, PadB
 *   A -> B  SHA1("req1", S), SHA1("req2", infohash) ^ SHA1("req3", S),
 *           RC4(VC, crypto_provide, len(PadC), PadC, len(IA)), RC4(IA)
 *   B -> A  RC4(VC, crypto_select, len(PadD), PadD), payload...
 *
 * where S is the Diffie-Hellman secret, VC eight zero bytes and IA our
 * BitTorrent handshake. A finds the end of PadB by looking for VC as B's
 * key stream would encrypt it; B finds the end of PadA by looking for
 * SHA1("req1", S), and the torrent by trying the info hashes its listener
 * serves (peerListenerAddInfoHash()). crypto_select picks RC4 for the whole
 * connection, or plaintext after these headers. Incoming connections are
 * told apart by their first bytes: a plain one starts with the 20-byte
 * protocol header. Once negotiated, bytes are encrypted as they are queued
 * (sendBytes()) and decrypted as they arrive (onRead()), so everything
 * after the handshake works unchanged. An outgoing attempt that the peer
 * drops during the exchange fails with PEER_ECRYPTO, so the caller can
 * retry without encryption.
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

#include "crypto.h"
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

/* Protocol encryption */
#define MSE_MAX_PAD             512u            /* PadA..PadD                      */
#define MSE_MAX_IA              8192u           /* the initial payload we accept   */
#define MSE_VC_LENGTH           8u
#define MSE_CRYPTO_PLAIN        0x01u           /* crypto_provide / crypto_select  */
#define MSE_CRYPTO_RC4          0x02u
#define MSE_DISCARD             1024u           /* RC4 key stream thrown away      */
#ifndef MSE_REPLY_SECONDS
#define MSE_REPLY_SECONDS       5u              /* for the peer's key: a peer that
                                                   does encryption answers at once,
                                                   one that does not may just wait */
#endif

enum {
    MSE_NONE = 0,                               /* not encrypting, or finished     */
    MSE_A_WAIT_Y,                               /* we sent Ya; waiting for Yb      */
    MSE_A_FIND_VC,                              /* looking for B's encrypted VC    */
    MSE_A_SELECT,                               /* VC, crypto_select, len(PadD)    */
    MSE_A_PAD,                                  /* PadD                            */
    MSE_B_WAIT_Y,                               /* waiting for Ya                  */
    MSE_B_FIND_REQ1,                            /* looking for SHA1("req1", S)     */
    MSE_B_PROVIDE,                              /* VC, crypto_provide, len(PadC)   */
    MSE_B_PAD,                                  /* PadC, len(IA)                   */
    MSE_B_IA                                    /* IA                              */
};

/* Policy for all connections (peerSetEncryption()). */
static uint8_t peerEncryption = PEER_ENCRYPTION_PREFER;

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

    /* Protocol encryption */
    uint8_t                 msePhase;       /* MSE_*                           */
    uint8_t                 mseInitiator;   /* we connected out                */
    uint8_t                 mseDecided;     /* incoming: plain or MSE known    */
    uint8_t                 plaintextOnly;  /* peerConnectOptions.plaintext    */
    uint8_t                 encryption;     /* PEER_ENCRYPTED_* once known     */
    uint8_t                 rc4Active;      /* encrypting everything both ways */
    uint8_t                 mseHashKnown;   /* incoming: mseInfoHash is set    */
    uint8_t                 msePrivate[CRYPTO_PRIVATE_LENGTH];
    uint8_t                 msePublic[CRYPTO_DH_LENGTH];
    uint8_t                 mseSecret[CRYPTO_DH_LENGTH];
    uint8_t                 mseInfoHash[PARSER_SHA1_LENGTH];
    uint8_t                 msePattern[PARSER_SHA1_LENGTH];  /* what we look for */
    uint32_t                msePos;         /* next field, in the buffer       */
    uint32_t                mseDecrypted;   /* bytes before this are decrypted */
    uint32_t                mseLength;      /* length of the current field     */
    uint32_t                mseProvide;     /* incoming: crypto_provide        */
    uint32_t                mseSelect;
    cryptoRc4               rc4In;
    cryptoRc4               rc4Out;

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
    uint8_t                 hashes[PEER_LISTENER_MAX_HASHES][PARSER_SHA1_LENGTH];
    uint32_t                hashCount;          /* for encrypted connections    */
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
    result.encryption = connection->encryption;
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
    if (connection->rc4Active) {
        cryptoRc4Apply(&connection->rc4Out, chunk->data, written);    /* in queue order */
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
        if (connection->pieceCount == 0) {
            break;                                  /* size unknown yet (magnet link) */
        }
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
        if (connection->pieceCount == 0) {
            break;                                  /* size unknown yet (magnet link) */
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

static int32_t mseCutShort(peerConnection *connection, int32_t status, const char *what);

static void onTimeout(uv_timer_t *timer)
{
    peerConnection *connection = timer->data;

    setError(connection, "no handshake within %" PRIu32 " seconds", connection->timeoutSeconds);
    terminate(connection, mseCutShort(connection, PEER_ETIMEDOUT, "no reply"));
}

static void onRead(uv_stream_t *stream, ssize_t count, const uv_buf_t *buffer);

/* Settings from peerConnectOptions, for either direction. */
static void configure(peerConnection *connection, const peerConnectOptions *options)
{
    uint32_t bitfieldMessage;

    connection->timeoutSeconds = options->timeoutSeconds;
    connection->pieceCount = options->pieceCount;
    connection->plaintextOnly = options->plaintext;
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
    if (connection->mseHashKnown &&
        memcmp(infoHash, connection->mseInfoHash, PARSER_SHA1_LENGTH) != 0) {
        setError(connection, "the encrypted and the BitTorrent handshakes name different "
                 "torrents");
        return PEER_EPROTO;
    }
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

/* ======================================================================== */
/* Protocol encryption (MSE)                                                */
/* ======================================================================== */

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/* SHA1(tag, a, b): tag is 4 characters ("req1", "keyA", ...). */
static void mseHash(const char *tag, const uint8_t *a, size_t aLength, const uint8_t *b,
                    size_t bLength, uint8_t out[PARSER_SHA1_LENGTH])
{
    uint8_t input[4u + CRYPTO_DH_LENGTH + PARSER_SHA1_LENGTH];

    memcpy(input, tag, 4u);
    memcpy(input + 4u, a, aLength);
    if (bLength > 0) {
        memcpy(input + 4u + aLength, b, bLength);
    }
    parserSha1(input, 4u + aLength + bLength, out);
}

/* Our public key and random padding (PadA or PadB). */
static int32_t mseSendKey(peerConnection *connection)
{
    uint8_t  message[CRYPTO_DH_LENGTH + MSE_MAX_PAD];
    uint16_t random = 0;
    uint32_t padLength;

    if (cryptoDhKeyPair(connection->msePrivate, connection->msePublic) != 0 ||
        cryptoRandom(&random, sizeof(random)) != 0) {
        setError(connection, "could not make an encryption key");
        return PEER_ERROR;
    }
    padLength = random % MSE_MAX_PAD;
    memcpy(message, connection->msePublic, CRYPTO_DH_LENGTH);
    if (padLength > 0 && cryptoRandom(message + CRYPTO_DH_LENGTH, padLength) != 0) {
        padLength = 0;
    }
    return sendBytes(connection, message, CRYPTO_DH_LENGTH + padLength, NULL);
}

/* The peer's public key is in the first 96 bytes: work out S. */
static int32_t mseSecret(peerConnection *connection)
{
    if (cryptoDhSecret(connection->msePrivate, connection->buffer, connection->mseSecret) != 0) {
        setError(connection, "encrypted handshake: the peer's key is not valid");
        return PEER_EPROTO;
    }
    return PEER_OK;
}

/* RC4 keys from S and the info hash; A sends with keyA, B with keyB. */
static void mseKeys(peerConnection *connection, const uint8_t infoHash[PARSER_SHA1_LENGTH])
{
    uint8_t keyA[PARSER_SHA1_LENGTH];
    uint8_t keyB[PARSER_SHA1_LENGTH];

    mseHash("keyA", connection->mseSecret, CRYPTO_DH_LENGTH, infoHash, PARSER_SHA1_LENGTH, keyA);
    mseHash("keyB", connection->mseSecret, CRYPTO_DH_LENGTH, infoHash, PARSER_SHA1_LENGTH, keyB);
    cryptoRc4Init(&connection->rc4Out, connection->mseInitiator ? keyA : keyB, PARSER_SHA1_LENGTH,
                  MSE_DISCARD);
    cryptoRc4Init(&connection->rc4In, connection->mseInitiator ? keyB : keyA, PARSER_SHA1_LENGTH,
                  MSE_DISCARD);
}

/* Decrypt the received bytes up to 'end' (each byte exactly once). */
static void mseDecryptTo(peerConnection *connection, uint32_t end)
{
    if (end > connection->mseDecrypted) {
        cryptoRc4Apply(&connection->rc4In, connection->buffer + connection->mseDecrypted,
                       end - connection->mseDecrypted);
        connection->mseDecrypted = end;
    }
}

/* Look for msePattern (length bytes) from msePos, no further than 'limit'. */
static int mseFind(peerConnection *connection, uint32_t length, uint32_t limit, uint32_t *at)
{
    uint32_t start;

    for (start = connection->msePos; start + length <= connection->received && start <= limit;
         start++) {
        if (memcmp(connection->buffer + start, connection->msePattern, length) == 0) {
            *at = start;
            return 1;
        }
    }
    return 0;
}

/* A: Yb has arrived: send the request with our handshake as IA. */
static int32_t mseSendRequest(peerConnection *connection)
{
    uint8_t  message[2u * PARSER_SHA1_LENGTH + MSE_VC_LENGTH + 8u + PEER_HANDSHAKE_LENGTH];
    uint8_t  req2[PARSER_SHA1_LENGTH];
    uint8_t  req3[PARSER_SHA1_LENGTH];
    uint8_t *encrypted = message + 2u * PARSER_SHA1_LENGTH;
    uint32_t i;

    mseHash("req1", connection->mseSecret, CRYPTO_DH_LENGTH, NULL, 0, message);
    mseHash("req2", connection->infoHash, PARSER_SHA1_LENGTH, NULL, 0, req2);
    mseHash("req3", connection->mseSecret, CRYPTO_DH_LENGTH, NULL, 0, req3);
    for (i = 0; i < PARSER_SHA1_LENGTH; i++) {
        message[PARSER_SHA1_LENGTH + i] = (uint8_t)(req2[i] ^ req3[i]);
    }
    memset(encrypted, 0, MSE_VC_LENGTH);                            /* VC */
    put32(encrypted + MSE_VC_LENGTH,
          (peerEncryption == PEER_ENCRYPTION_REQUIRE) ? MSE_CRYPTO_RC4
                                                      : (MSE_CRYPTO_RC4 | MSE_CRYPTO_PLAIN));
    encrypted[MSE_VC_LENGTH + 4u] = 0;                              /* len(PadC) = 0 */
    encrypted[MSE_VC_LENGTH + 5u] = 0;
    encrypted[MSE_VC_LENGTH + 6u] = 0;                              /* len(IA) */
    encrypted[MSE_VC_LENGTH + 7u] = (uint8_t)PEER_HANDSHAKE_LENGTH;
    memcpy(encrypted + MSE_VC_LENGTH + 8u, connection->handshake, PEER_HANDSHAKE_LENGTH);
    cryptoRc4Apply(&connection->rc4Out, encrypted, MSE_VC_LENGTH + 8u + PEER_HANDSHAKE_LENGTH);
    return sendBytes(connection, message, sizeof(message), NULL);
}

/* The exchange is over: the payload starts at buffer[start]. */
static void mseFinish(peerConnection *connection, uint32_t start)
{
    if (connection->mseSelect == MSE_CRYPTO_RC4) {
        mseDecryptTo(connection, connection->received);
        connection->rc4Active = 1;
        connection->encryption = PEER_ENCRYPTED_RC4;
    } else {
        connection->encryption = PEER_ENCRYPTED_HEADER;
    }
    connection->received -= start;
    memmove(connection->buffer, connection->buffer + start, connection->received);
    connection->msePhase = MSE_NONE;
    memset(connection->msePrivate, 0, sizeof(connection->msePrivate));
}

/*
 * Advance the exchange with what has arrived. PEER_OK when it is complete
 * (the buffer then holds the payload: the peer's BitTorrent handshake), 1
 * if more bytes are needed, or an error.
 */
static int32_t mseProcess(peerConnection *connection)
{
    uint8_t *buffer;
    uint32_t at;
    uint32_t i;
    int32_t  result;

    for (;;) {
        buffer = connection->buffer;
        switch (connection->msePhase) {
        case MSE_A_WAIT_Y:
            if (connection->received < CRYPTO_DH_LENGTH) {
                return 1;
            }
            result = mseSecret(connection);
            if (result != PEER_OK) {
                return result;
            }
            mseKeys(connection, connection->infoHash);
            if (connection->timeoutSeconds == 0 || connection->timeoutSeconds > MSE_REPLY_SECONDS) {
                /* It answered: back to the ordinary handshake time limit. */
                uv_timer_stop(&connection->timer);
                if (connection->timeoutSeconds > 0) {
                    uv_timer_start(&connection->timer, onTimeout,
                                   (uint64_t)connection->timeoutSeconds * 1000u, 0);
                }
            }
            {
                cryptoRc4 copy = connection->rc4In;   /* B's VC, as it will arrive */

                memset(connection->msePattern, 0, MSE_VC_LENGTH);
                cryptoRc4Apply(&copy, connection->msePattern, MSE_VC_LENGTH);
            }
            result = mseSendRequest(connection);
            if (result != PEER_OK) {
                setError(connection, "could not send the encrypted handshake");
                return result;
            }
            connection->msePos = CRYPTO_DH_LENGTH;
            connection->msePhase = MSE_A_FIND_VC;
            break;
        case MSE_A_FIND_VC:
            if (!mseFind(connection, MSE_VC_LENGTH, CRYPTO_DH_LENGTH + MSE_MAX_PAD, &at)) {
                if (connection->received >= CRYPTO_DH_LENGTH + MSE_MAX_PAD + MSE_VC_LENGTH) {
                    setError(connection, "encrypted handshake: no valid reply from the peer");
                    return PEER_EPROTO;
                }
                return 1;
            }
            connection->mseDecrypted = at;          /* B's encryption starts at VC */
            connection->msePos = at;
            connection->msePhase = MSE_A_SELECT;
            break;
        case MSE_A_SELECT:
            if (connection->received < connection->msePos + MSE_VC_LENGTH + 6u) {
                return 1;
            }
            mseDecryptTo(connection, connection->msePos + MSE_VC_LENGTH + 6u);
            connection->mseSelect = get32(buffer + connection->msePos + MSE_VC_LENGTH);
            connection->mseLength = ((uint32_t)buffer[connection->msePos + MSE_VC_LENGTH + 4u] << 8) |
                                    buffer[connection->msePos + MSE_VC_LENGTH + 5u];
            if (connection->mseLength > MSE_MAX_PAD ||
                (connection->mseSelect != MSE_CRYPTO_RC4 &&
                 (connection->mseSelect != MSE_CRYPTO_PLAIN ||
                  peerEncryption == PEER_ENCRYPTION_REQUIRE))) {
                setError(connection, "encrypted handshake: the peer chose a method we did not "
                         "offer (%" PRIu32 ")", connection->mseSelect);
                return PEER_EPROTO;
            }
            connection->msePos += MSE_VC_LENGTH + 6u;
            connection->msePhase = MSE_A_PAD;
            break;
        case MSE_A_PAD:
            if (connection->received < connection->msePos + connection->mseLength) {
                return 1;
            }
            mseDecryptTo(connection, connection->msePos + connection->mseLength);
            mseFinish(connection, connection->msePos + connection->mseLength);
            return PEER_OK;

        case MSE_B_WAIT_Y:
            if (connection->received < CRYPTO_DH_LENGTH) {
                return 1;
            }
            result = mseSendKey(connection);
            if (result == PEER_OK) {
                result = mseSecret(connection);
            }
            if (result != PEER_OK) {
                return result;
            }
            mseHash("req1", connection->mseSecret, CRYPTO_DH_LENGTH, NULL, 0,
                    connection->msePattern);
            connection->msePos = CRYPTO_DH_LENGTH;
            connection->msePhase = MSE_B_FIND_REQ1;
            break;
        case MSE_B_FIND_REQ1: {
            struct peerListener *listener = connection->listener;
            uint8_t              want[PARSER_SHA1_LENGTH];
            uint8_t              candidate[PARSER_SHA1_LENGTH];
            uint32_t             h;

            if (!mseFind(connection, PARSER_SHA1_LENGTH, CRYPTO_DH_LENGTH + MSE_MAX_PAD, &at)) {
                if (connection->received >= CRYPTO_DH_LENGTH + MSE_MAX_PAD + PARSER_SHA1_LENGTH) {
                    setError(connection, "not a BitTorrent peer (no encrypted handshake)");
                    return PEER_EPROTO;
                }
                return 1;
            }
            if (connection->received < at + 2u * PARSER_SHA1_LENGTH) {
                return 1;
            }
            /* SHA1("req2", infohash) = what was sent ^ SHA1("req3", S) */
            mseHash("req3", connection->mseSecret, CRYPTO_DH_LENGTH, NULL, 0, want);
            for (i = 0; i < PARSER_SHA1_LENGTH; i++) {
                want[i] ^= buffer[at + PARSER_SHA1_LENGTH + i];
            }
            for (h = 0; listener != NULL && h < listener->hashCount; h++) {
                mseHash("req2", listener->hashes[h], PARSER_SHA1_LENGTH, NULL, 0, candidate);
                if (memcmp(candidate, want, PARSER_SHA1_LENGTH) == 0) {
                    break;
                }
            }
            if (listener == NULL || h == listener->hashCount) {
                setError(connection, "not serving that torrent (encrypted connection)");
                return PEER_EHASH;
            }
            memcpy(connection->mseInfoHash, listener->hashes[h], PARSER_SHA1_LENGTH);
            connection->mseHashKnown = 1;
            mseKeys(connection, connection->mseInfoHash);
            connection->msePos = at + 2u * PARSER_SHA1_LENGTH;
            connection->mseDecrypted = connection->msePos;
            connection->msePhase = MSE_B_PROVIDE;
            break;
        }
        case MSE_B_PROVIDE:
            if (connection->received < connection->msePos + MSE_VC_LENGTH + 6u) {
                return 1;
            }
            mseDecryptTo(connection, connection->msePos + MSE_VC_LENGTH + 6u);
            for (i = 0; i < MSE_VC_LENGTH; i++) {
                if (buffer[connection->msePos + i] != 0) {
                    setError(connection, "encrypted handshake: bad verification constant");
                    return PEER_EPROTO;
                }
            }
            connection->mseProvide = get32(buffer + connection->msePos + MSE_VC_LENGTH);
            connection->mseLength = ((uint32_t)buffer[connection->msePos + MSE_VC_LENGTH + 4u] << 8) |
                                    buffer[connection->msePos + MSE_VC_LENGTH + 5u];
            if (connection->mseLength > MSE_MAX_PAD) {
                setError(connection, "encrypted handshake: padding too long");
                return PEER_EPROTO;
            }
            connection->msePos += MSE_VC_LENGTH + 6u;
            connection->msePhase = MSE_B_PAD;
            break;
        case MSE_B_PAD:
            if (connection->received < connection->msePos + connection->mseLength + 2u) {
                return 1;
            }
            mseDecryptTo(connection, connection->msePos + connection->mseLength + 2u);
            connection->msePos += connection->mseLength + 2u;
            connection->mseLength = ((uint32_t)buffer[connection->msePos - 2u] << 8) |
                                    buffer[connection->msePos - 1u];
            if (connection->mseLength > MSE_MAX_IA) {
                setError(connection, "encrypted handshake: initial payload too long");
                return PEER_EPROTO;
            }
            connection->msePhase = MSE_B_IA;
            break;
        case MSE_B_IA: {
            uint8_t reply[MSE_VC_LENGTH + 6u];

            if (connection->received < connection->msePos + connection->mseLength) {
                return 1;
            }
            mseDecryptTo(connection, connection->msePos + connection->mseLength);
            if (connection->mseProvide & MSE_CRYPTO_RC4) {
                connection->mseSelect = MSE_CRYPTO_RC4;
            } else if ((connection->mseProvide & MSE_CRYPTO_PLAIN) &&
                       peerEncryption != PEER_ENCRYPTION_REQUIRE) {
                connection->mseSelect = MSE_CRYPTO_PLAIN;
            } else {
                setError(connection, "encrypted handshake: no method in common (%" PRIu32 ")",
                         connection->mseProvide);
                return PEER_EPROTO;
            }
            memset(reply, 0, sizeof(reply));        /* VC, select, len(PadD) = 0 */
            put32(reply + MSE_VC_LENGTH, connection->mseSelect);
            cryptoRc4Apply(&connection->rc4Out, reply, sizeof(reply));
            result = sendBytes(connection, reply, sizeof(reply), NULL);
            if (result != PEER_OK) {
                setError(connection, "could not answer the encrypted handshake");
                return result;
            }
            /* The payload is IA (the peer's handshake), then whatever followed. */
            mseFinish(connection, connection->msePos);
            return PEER_OK;
        }
        default:
            return PEER_OK;
        }
    }
}

/*
 * Incoming, nothing known yet: plain BitTorrent or encrypted? 1 to wait for
 * more bytes, PEER_OK once decided (msePhase set if encrypted), or an error
 * if our policy refuses it.
 */
static int32_t decideIncoming(peerConnection *connection)
{
    uint32_t compare = (connection->received > 1u) ? connection->received - 1u : 0u;
    int      plain;

    if (compare > PEER_PROTOCOL_LENGTH) {
        compare = PEER_PROTOCOL_LENGTH;
    }
    plain = connection->buffer[0] == PEER_PROTOCOL_LENGTH &&
            memcmp(connection->buffer + 1, PEER_PROTOCOL_NAME, compare) == 0;
    if (plain && compare < PEER_PROTOCOL_LENGTH) {
        return 1;                                   /* could still be either */
    }
    connection->mseDecided = 1;
    if (plain) {
        if (peerEncryption == PEER_ENCRYPTION_REQUIRE) {
            setError(connection, "unencrypted connection refused (encryption is required)");
            return PEER_ECRYPTO;
        }
        return PEER_OK;
    }
    if (peerEncryption == PEER_ENCRYPTION_OFF) {
        setError(connection, "not a BitTorrent peer, or an encrypted connection "
                 "(encryption is off)");
        return PEER_EPROTO;
    }
    connection->msePhase = MSE_B_WAIT_Y;
    return PEER_OK;
}

/* The status to report when an outgoing exchange is cut short. */
static int32_t mseCutShort(peerConnection *connection, int32_t status, const char *what)
{
    if (connection->mseInitiator && connection->msePhase != MSE_NONE) {
        setError(connection, "peer did not accept an encrypted connection (%s)", what);
        return PEER_ECRYPTO;
    }
    return status;
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
        if (connection->rc4Active) {
            cryptoRc4Apply(&connection->rc4In, connection->buffer + connection->received,
                           (size_t)count);
        }
        connection->received += (uint32_t)count;
        connection->lastReceive = uv_now(peerModule.loop);
        if (connection->handshakeDone) {
            connection->bytesReceived += (uint64_t)count;
            processMessages(connection);
            return;
        }
        if (connection->incoming && !connection->accepted && !connection->mseDecided) {
            result = decideIncoming(connection);
            if (result == 1) {
                return;
            }
            if (result != PEER_OK) {
                terminate(connection, result);
                return;
            }
        }
        if (connection->msePhase != MSE_NONE) {
            result = mseProcess(connection);
            if (result == 1) {
                return;
            }
            if (result != PEER_OK) {
                terminate(connection, result);
                return;
            }
            if (connection->received == 0) {
                return;                             /* the handshake comes next */
            }
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
        } else if (connection->msePhase == MSE_NONE) {
            captureRemote(connection);
            setError(connection, "peer closed the connection during the handshake "
                     "(after %" PRIu32 " bytes)", connection->received);
        } else {
            setError(connection, "peer closed the connection during the encrypted handshake");
        }
        terminate(connection, mseCutShort(connection, PEER_ECLOSED, "it closed the connection"));
    } else if (count == UV_ENOBUFS) {
        terminate(connection, PEER_ENOMEM);
    } else if (count < 0) {
        setError(connection, "receive failed: %s", uv_strerror((int)count));
        terminate(connection, mseCutShort(connection, PEER_ERROR, uv_strerror((int)count)));
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

    /* Send our handshake (or start the encrypted exchange, which carries it)
     * and start reading theirs; peers may send theirs before or after
     * receiving ours. */
    if (peerEncryption != PEER_ENCRYPTION_OFF && !connection->plaintextOnly) {
        connection->mseInitiator = 1;
        connection->msePhase = MSE_A_WAIT_Y;
        result = mseSendKey(connection);
        if (connection->timeoutSeconds == 0 || connection->timeoutSeconds > MSE_REPLY_SECONDS) {
            uv_timer_stop(&connection->timer);
            uv_timer_start(&connection->timer, onTimeout, (uint64_t)MSE_REPLY_SECONDS * 1000u, 0);
        }
    } else {
        result = sendBytes(connection, connection->handshake, PEER_HANDSHAKE_LENGTH, NULL);
    }
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

int32_t peerListenerAddInfoHash(peerListener *listener, const uint8_t infoHash[PARSER_SHA1_LENGTH])
{
    if (listener == NULL || infoHash == NULL) {
        return PEER_EINVAL;
    }
    if (listener->hashCount == PEER_LISTENER_MAX_HASHES) {
        return PEER_ENOMEM;
    }
    memcpy(listener->hashes[listener->hashCount++], infoHash, PARSER_SHA1_LENGTH);
    return PEER_OK;
}

void peerSetEncryption(uint8_t mode)
{
    if (mode <= PEER_ENCRYPTION_REQUIRE) {
        peerEncryption = mode;
    }
}

uint8_t peerGetEncryption(void)
{
    return peerEncryption;
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
    state->encryption = connection->encryption;
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
    case PEER_ECRYPTO:    return "Encrypted connection refused";
    case PEER_ECLOSED:   return "Connection closed";
    default:             return "Unknown status";
    }
}
