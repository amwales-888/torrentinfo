/*
 * peer.c - BitTorrent peer connections using libuv (BEP 3 peer wire protocol).
 *
 * Step 1: TCP connect, then exchange 68-byte handshakes:
 *
 *   offset  0: 19 (length of the protocol name)
 *   offset  1: "BitTorrent protocol"
 *   offset 20: 8 reserved bytes (extension flags)
 *   offset 28: info hash (20)
 *   offset 48: peer id (20)
 *
 * The peer's handshake is checked as it arrives, so a server that is not
 * speaking BitTorrent fails after its first 20 bytes rather than at the
 * timeout.
 *
 * Lifetime: as in http.c, 'pending' counts libuv callbacks still to come.
 * Unlike an HTTP request, a connection is freed only once peerClose() has
 * been called (and pending is zero), because a successful connection stays
 * open for the messages that follow.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "peer.h"
#include "queue.h"

#define PEER_STATE_MAGIC        0x50454552u     /* "PEER" */
#define PEER_PROTOCOL_NAME      "BitTorrent protocol"
#define PEER_PROTOCOL_LENGTH    19u
#define PEER_OFFSET_RESERVED    20u
#define PEER_OFFSET_INFO_HASH   28u
#define PEER_OFFSET_PEER_ID     48u
#define PEER_BUFFER_SIZE        16384u          /* handshake plus early messages */

struct peerConnection {
    peerCallback            callback;
    void                   *privateArg;
    int32_t                 status;
    uint8_t                 finished;       /* handshake outcome known        */
    uint8_t                 closed;         /* peerClose() called             */
    uint8_t                 inCallback;
    uint32_t                pending;        /* libuv callbacks still to come  */
    uint32_t                timeoutSeconds;

    struct sockaddr_storage address;
    uint8_t                 infoHash[PARSER_SHA1_LENGTH];
    uint8_t                 localPeerId[PEER_ID_LENGTH];

    uv_tcp_t                tcp;
    uv_timer_t              timer;
    uv_connect_t            connectRequest;
    uv_write_t              writeRequest;
    uint8_t                 tcpOpen;
    uint8_t                 timerOpen;

    uint8_t                 handshake[PEER_HANDSHAKE_LENGTH];   /* ours, as sent */
    uint8_t                 buffer[PEER_BUFFER_SIZE];          /* received      */
    uint32_t                received;

    char                    errorMessage[PEER_ERROR_MESSAGE_LENGTH];
    TAILQ_ENTRY(peerConnection) entries;
};
TAILQ_HEAD(peerConnectionList, peerConnection);

static struct {
    uv_loop_t                *loop;
    parserContext            *context;
    uint32_t                  users;        /* peerInit() calls outstanding */
    uint32_t                  magic;
    struct peerConnectionList connections;
} peerState;

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static void *allocate(uint64_t size)
{
    void *memory;

    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    memory = peerState.context->allocator.realloc(NULL, (size_t)size);
    if (memory != NULL) {
        memset(memory, 0, (size_t)size);
    }
    return memory;
}

static void release(void *memory)
{
    if (memory != NULL) {
        peerState.context->allocator.free(memory);
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

static void closeTimer(peerConnection *connection)
{
    if (connection->timerOpen && !uv_is_closing((uv_handle_t *)&connection->timer)) {
        uv_timer_stop(&connection->timer);
        uv_close((uv_handle_t *)&connection->timer, onHandleClosed);
    }
}

static void closeAll(peerConnection *connection)
{
    closeTimer(connection);
    if (connection->tcpOpen && !uv_is_closing((uv_handle_t *)&connection->tcp)) {
        uv_close((uv_handle_t *)&connection->tcp, onHandleClosed);
    }
}

/*
 * Record the handshake outcome and tell the user. On failure the socket is
 * closed; on success it stays open (only the timeout timer goes).
 */
static void finish(peerConnection *connection, int32_t status)
{
    peerHandshakeResult result;
    const uint8_t      *reserved = connection->buffer + PEER_OFFSET_RESERVED;

    if (connection->finished) {
        return;
    }
    connection->finished = 1;
    connection->status = status;
    if (status != PEER_OK && connection->errorMessage[0] == '\0') {
        setError(connection, "%s", peerStatusString(status));
    }
    if (status == PEER_OK) {
        closeTimer(connection);
    } else {
        closeAll(connection);
    }

    if (connection->closed || connection->callback == NULL) {
        return;
    }
    memset(&result, 0, sizeof(result));
    result.status = status;
    if (connection->received >= PEER_OFFSET_INFO_HASH) {
        memcpy(result.reserved, reserved, PEER_RESERVED_LENGTH);
        result.extensions = ((reserved[5] & 0x10u) ? PEER_EXTENSION_PROTOCOL : 0u) |
                            ((reserved[7] & 0x04u) ? PEER_EXTENSION_FAST : 0u) |
                            ((reserved[7] & 0x01u) ? PEER_EXTENSION_DHT : 0u);
    }
    if (connection->received >= PEER_HANDSHAKE_LENGTH) {
        memcpy(result.peerId, connection->buffer + PEER_OFFSET_PEER_ID, PEER_ID_LENGTH);
        result.extraBytes = connection->received - PEER_HANDSHAKE_LENGTH;
    }
    result.errorMessage = (status == PEER_OK) ? NULL : connection->errorMessage;

    /* inCallback stops peerClose()/peerShutdown() freeing the connection
     * while we are still using it. */
    connection->inCallback = 1;
    connection->callback(connection->privateArg, connection, &result);
    connection->inCallback = 0;
    maybeFree(connection);
}

static void onTimeout(uv_timer_t *timer)
{
    peerConnection *connection = timer->data;

    setError(connection, "no handshake within %" PRIu32 " seconds", connection->timeoutSeconds);
    finish(connection, PEER_ETIMEDOUT);
}

/* ======================================================================== */
/* Handshake                                                                */
/* ======================================================================== */

/*
 * Check what has arrived so far. Returns PEER_OK when the handshake is
 * complete, 1 if more bytes are needed, or an error status.
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

static void onAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    peerConnection *connection = handle->data;

    (void)suggested;
    *buffer = uv_buf_init((char *)connection->buffer + connection->received,
                          PEER_BUFFER_SIZE - connection->received);
}

static void onRead(uv_stream_t *stream, ssize_t count, const uv_buf_t *buffer)
{
    peerConnection *connection = stream->data;
    int32_t         result;

    (void)buffer;
    if (connection->finished) {
        return;
    }
    if (count > 0) {
        connection->received += (uint32_t)count;
        result = checkHandshake(connection);
        if (result == 1) {
            return;                                 /* wait for more */
        }
        if (result == PEER_OK) {
            /* Keep the connection, but stop reading until messages are
             * handled; anything already received stays in the buffer. */
            uv_read_stop(stream);
        }
        finish(connection, result);
    } else if (count == UV_EOF) {
        setError(connection, "peer closed the connection during the handshake "
                 "(after %" PRIu32 " bytes)", connection->received);
        finish(connection, PEER_ECLOSED);
    } else if (count < 0) {
        setError(connection, "receive failed: %s", uv_strerror((int)count));
        finish(connection, PEER_ERROR);
    }
}

static void onWrite(uv_write_t *write, int status)
{
    peerConnection *connection = write->data;

    connection->pending--;
    if (connection->finished) {
        maybeFree(connection);
        return;
    }
    if (status < 0) {
        setError(connection, "send failed: %s", uv_strerror(status));
        finish(connection, PEER_ERROR);
    }
}

static void onConnect(uv_connect_t *connect, int status)
{
    peerConnection *connection = connect->data;
    uv_buf_t        buffer;
    int             result;

    connection->pending--;
    if (connection->finished) {
        maybeFree(connection);
        return;
    }
    if (status < 0) {
        setError(connection, "connect failed: %s", uv_strerror(status));
        finish(connection, PEER_ERROR);
        return;
    }

    /* Send our handshake and start reading theirs; peers may send theirs
     * before or after receiving ours. */
    buffer = uv_buf_init((char *)connection->handshake, PEER_HANDSHAKE_LENGTH);
    connection->writeRequest.data = connection;
    result = uv_write(&connection->writeRequest, (uv_stream_t *)&connection->tcp,
                      &buffer, 1, onWrite);
    if (result < 0) {
        setError(connection, "send failed: %s", uv_strerror(result));
        finish(connection, PEER_ERROR);
        return;
    }
    connection->pending++;

    result = uv_read_start((uv_stream_t *)&connection->tcp, onAlloc, onRead);
    if (result < 0) {
        setError(connection, "receive failed: %s", uv_strerror(result));
        finish(connection, PEER_ERROR);
    }
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t peerInit(uv_loop_t *loop, parserContext *context)
{
    if (loop == NULL || context == NULL || context->magic != PARSER_CONTEXT_MAGIC) {
        return PEER_EINVAL;
    }
    if (peerState.magic == PEER_STATE_MAGIC) {
        if (loop != peerState.loop || context != peerState.context) {
            return PEER_EINVAL;
        }
        peerState.users++;
        return PEER_OK;
    }
    peerState.loop = loop;
    peerState.context = context;
    peerState.users = 1;
    TAILQ_INIT(&peerState.connections);
    peerState.magic = PEER_STATE_MAGIC;
    return PEER_OK;
}

int32_t peerConnect(const struct sockaddr *address,
                    const uint8_t infoHash[PARSER_SHA1_LENGTH],
                    const uint8_t peerId[PEER_ID_LENGTH],
                    uint32_t timeoutSeconds, peerCallback callback,
                    void *privateArg, peerConnection **handle)
{
    peerConnection *connection;
    size_t          addressLength;
    int             result;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (peerState.magic != PEER_STATE_MAGIC) {
        return PEER_ENOTINIT;
    }
    if (address == NULL || infoHash == NULL || peerId == NULL || handle == NULL) {
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
    connection->callback = callback;
    connection->privateArg = privateArg;
    connection->timeoutSeconds = timeoutSeconds;
    memcpy(&connection->address, address, addressLength);
    memcpy(connection->infoHash, infoHash, PARSER_SHA1_LENGTH);
    memcpy(connection->localPeerId, peerId, PEER_ID_LENGTH);

    /* Our handshake. Reserved bytes are zero: no extensions yet. */
    connection->handshake[0] = (uint8_t)PEER_PROTOCOL_LENGTH;
    memcpy(connection->handshake + 1, PEER_PROTOCOL_NAME, PEER_PROTOCOL_LENGTH);
    memcpy(connection->handshake + PEER_OFFSET_INFO_HASH, infoHash, PARSER_SHA1_LENGTH);
    memcpy(connection->handshake + PEER_OFFSET_PEER_ID, peerId, PEER_ID_LENGTH);

    /* From here on, failures must close what has been opened. */
    connection->closed = 1;                         /* not listed until started */
    if (timeoutSeconds > 0) {
        uv_timer_init(peerState.loop, &connection->timer);
        connection->timer.data = connection;
        connection->timerOpen = 1;
        connection->pending++;
        uv_timer_start(&connection->timer, onTimeout, (uint64_t)timeoutSeconds * 1000u, 0);
    }

    result = uv_tcp_init(peerState.loop, &connection->tcp);
    if (result == 0) {
        connection->tcp.data = connection;
        connection->tcpOpen = 1;
        connection->pending++;
        connection->connectRequest.data = connection;
        result = uv_tcp_connect(&connection->connectRequest, &connection->tcp,
                                (const struct sockaddr *)&connection->address, onConnect);
    }
    if (result < 0) {
        connection->finished = 1;
        closeAll(connection);
        maybeFree(connection);                      /* now, or once handles close */
        return PEER_ERROR;
    }
    connection->pending++;

    connection->closed = 0;
    TAILQ_INSERT_TAIL(&peerState.connections, connection, entries);
    *handle = connection;
    return PEER_OK;
}

void peerClose(peerConnection *connection)
{
    if (connection == NULL || connection->closed) {
        return;
    }
    connection->closed = 1;
    TAILQ_REMOVE(&peerState.connections, connection, entries);
    connection->finished = 1;                       /* no callback from now on */
    closeAll(connection);
    maybeFree(connection);
}

void peerShutdown(void)
{
    peerConnection *connection;

    if (peerState.magic != PEER_STATE_MAGIC || --peerState.users > 0) {
        return;
    }
    while ((connection = TAILQ_FIRST(&peerState.connections)) != NULL) {
        peerClose(connection);
    }
    /* loop and context stay set: closing connections still need the allocator. */
    peerState.magic = 0;
}

const char *peerStatusString(int32_t status)
{
    switch (status) {
    case PEER_OK:        return "OK";
    case PEER_ERROR:     return "Network error";
    case PEER_EINVAL:    return "Invalid argument";
    case PEER_ENOMEM:    return "Out of memory";
    case PEER_ETIMEDOUT: return "Timed out";
    case PEER_EPROTO:    return "Not a BitTorrent peer";
    case PEER_ENOTINIT:  return "Peer module not initialised";
    case PEER_EHASH:     return "Different torrent";
    case PEER_ESELF:     return "Connected to ourselves";
    case PEER_ECLOSED:   return "Peer closed the connection";
    default:             return "Unknown status";
    }
}
