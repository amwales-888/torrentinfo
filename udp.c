/*
 * udp.c - Asynchronous UDP tracker announces (BEP 15) using libuv.
 *
 * Packets (all integers big-endian):
 *
 *   connect request   (16): protocol id 0x41727101980 (64), action 0 (32),
 *                           transaction id (32)
 *   connect response  (16): action 0, transaction id, connection id (64)
 *   announce request  (98): connection id, action 1, transaction id,
 *                           info hash (20), peer id (20), downloaded (64),
 *                           left (64), uploaded (64), event (32), IP (32),
 *                           key (32), num want (32), port (16)
 *   announce response (20+): action 1, transaction id, interval, leechers,
 *                           seeders, then compact peers
 *   error             (8+): action 3, transaction id, message
 *
 * Lifetime: as in http.c. 'pending' counts libuv callbacks still to come;
 * a request is freed when detached (callback returned, or cancelled) and
 * pending is zero.
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>

#include "http.h"   /* httpUrlParse() */
#include "udp.h"

#define UDP_STATE_MAGIC         0x55445054u     /* "UDPT" */
#define UDP_BUFFER_SIZE         8192u
#define UDP_RETRY_MS            2000u           /* first retransmit          */
#define UDP_MAX_RETRY_MS        16000u          /* retransmit back-off cap   */
#define UDP_PROTOCOL_ID         0x41727101980ull
#define UDP_ACTION_CONNECT      0u
#define UDP_ACTION_ANNOUNCE     1u
#define UDP_ACTION_ERROR        3u
#define UDP_CONNECT_SIZE        16u
#define UDP_ANNOUNCE_SIZE       98u
#define UDP_ANNOUNCE_HEADER     20u

struct udpRequest {
    udpCallback             callback;
    void                   *privateArg;
    int32_t                 status;
    uint8_t                 finished;       /* status final; handles closing */
    uint8_t                 detached;       /* callback done, or cancelled   */
    uint8_t                 inCallback;
    uint32_t                pending;        /* libuv callbacks still to come */
    uint32_t                timeoutSeconds;

    char                   *host;
    udpAnnounceParams       params;

    uv_getaddrinfo_t        resolver;
    uv_timer_t              timeoutTimer;
    uv_timer_t              retryTimer;
    uv_udp_t                udp;
    uint8_t                 resolving;
    uint8_t                 timeoutOpen;
    uint8_t                 retryOpen;
    uint8_t                 udpOpen;
    uint8_t                 ipv6;           /* reached the tracker over IPv6 */

    uint8_t                 stage;          /* UDP_ACTION_CONNECT or _ANNOUNCE */
    uint32_t                transactionId;
    uint64_t                connectionId;
    uint32_t                retryCount;
    uint8_t                 packet[UDP_ANNOUNCE_SIZE];
    uint32_t                packetLength;
    uint8_t                 buffer[UDP_BUFFER_SIZE];    /* received datagram */
    uint64_t                received;                   /* final reply size  */

    char                   *failureReason;
    char                    errorMessage[UDP_ERROR_MESSAGE_LENGTH];
    TAILQ_ENTRY(udpRequest) entries;
};
TAILQ_HEAD(udpRequestList, udpRequest);

static struct {
    uv_loop_t            *loop;
    parserContext        *context;
    uint32_t              users;            /* udpInit() calls outstanding */
    uint32_t              magic;
    struct udpRequestList requests;
} udpState;

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static void *allocate(uint64_t size)
{
    void *memory;

    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    memory = udpState.context->allocator.realloc(NULL, (size_t)size);
    if (memory != NULL) {
        memset(memory, 0, (size_t)size);
    }
    return memory;
}

static void release(void *memory)
{
    if (memory != NULL) {
        udpState.context->allocator.free(memory);
    }
}

static char *copyBytes(const void *data, uint64_t length)
{
    char *copy;

    if (length == UINT64_MAX) {
        return NULL;
    }
    copy = allocate(length + 1u);
    if (copy != NULL && length > 0) {
        memcpy(copy, data, (size_t)length);
    }
    return copy;
}

static void setError(udpRequest *request, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(request->errorMessage, sizeof(request->errorMessage), format, arguments);
    va_end(arguments);
}

static uint32_t random32(void)
{
    uint32_t value = 0;

    if (uv_random(NULL, NULL, &value, sizeof(value), 0, NULL) != 0) {
        value = (uint32_t)uv_hrtime() ^ (uint32_t)(uintptr_t)&value;
    }
    return value;
}

static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void put64(uint8_t *out, uint64_t value)
{
    put32(out, (uint32_t)(value >> 32));
    put32(out + 4, (uint32_t)value);
}

static uint32_t get32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static uint64_t get64(const uint8_t *in)
{
    return ((uint64_t)get32(in) << 32) | (uint64_t)get32(in + 4);
}

/* ======================================================================== */
/* Request lifetime                                                         */
/* ======================================================================== */

static void freeRequest(udpRequest *request)
{
    release(request->host);
    release(request->failureReason);
    release(request);
}

static void maybeFree(udpRequest *request)
{
    if (request->detached && !request->inCallback && request->pending == 0) {
        freeRequest(request);
    }
}

static void onHandleClosed(uv_handle_t *handle)
{
    udpRequest *request = handle->data;

    if (handle == (uv_handle_t *)&request->udp) {
        request->udpOpen = 0;
    } else if (handle == (uv_handle_t *)&request->timeoutTimer) {
        request->timeoutOpen = 0;
    } else if (handle == (uv_handle_t *)&request->retryTimer) {
        request->retryOpen = 0;
    }
    request->pending--;
    maybeFree(request);
}

static void closeHandle(uv_handle_t *handle, uint8_t open)
{
    if (open && !uv_is_closing(handle)) {
        uv_close(handle, onHandleClosed);
    }
}

/* Stop all network activity. Callbacks still to come will see 'finished'. */
static void closeAll(udpRequest *request)
{
    if (request->resolving) {
        uv_cancel((uv_req_t *)&request->resolver); /* may fail if running */
    }
    if (request->timeoutOpen) {
        uv_timer_stop(&request->timeoutTimer);
    }
    if (request->retryOpen) {
        uv_timer_stop(&request->retryTimer);
    }
    if (request->udpOpen) {
        uv_udp_recv_stop(&request->udp);
    }
    closeHandle((uv_handle_t *)&request->timeoutTimer, request->timeoutOpen);
    closeHandle((uv_handle_t *)&request->retryTimer, request->retryOpen);
    closeHandle((uv_handle_t *)&request->udp, request->udpOpen);
}

static void detach(udpRequest *request)
{
    if (!request->detached) {
        request->detached = 1;
        TAILQ_REMOVE(&udpState.requests, request, entries);
    }
}

/* Record the outcome, close the handles, and run the callback. */
static void finish(udpRequest *request, int32_t status)
{
    udpResponse response;

    if (request->finished) {
        return;
    }
    request->finished = 1;
    request->status = status;
    if (status != UDP_OK && request->errorMessage[0] == '\0') {
        setError(request, "%s", udpStatusString(status));
    }
    closeAll(request);

    if (!request->detached && request->callback != NULL) {
        memset(&response, 0, sizeof(response));
        response.status = status;
        response.failureReason = request->failureReason;
        response.errorMessage = (status == UDP_OK) ? NULL : request->errorMessage;
        response.peerFamily = request->ipv6 ? UDP_FAMILY_IPV6 : UDP_FAMILY_IPV4;
        if (status == UDP_OK) {
            response.interval = get32(request->buffer + 8);
            response.leechers = get32(request->buffer + 12);
            response.seeders = get32(request->buffer + 16);
            response.peers = request->buffer + UDP_ANNOUNCE_HEADER;
            response.peersLength = request->received - UDP_ANNOUNCE_HEADER;
        }

        /* inCallback stops udpCancel()/udpShutdown() from freeing the
         * request while we are still using it. */
        request->inCallback = 1;
        request->callback(request->privateArg, &response);
        request->inCallback = 0;
    }
    detach(request);
    maybeFree(request);
}

static void onTimeout(uv_timer_t *timer)
{
    udpRequest *request = timer->data;

    setError(request, "no response within %" PRIu32 " seconds", request->timeoutSeconds);
    finish(request, UDP_ETIMEDOUT);
}

/* ======================================================================== */
/* Protocol                                                                 */
/* ======================================================================== */

static void transmit(udpRequest *request);

static void onRetry(uv_timer_t *timer)
{
    udpRequest *request = timer->data;

    if (request->finished) {
        return;
    }
    if (request->retryCount < 16u) {
        request->retryCount++;
    }
    transmit(request);
}

/* Send the current packet and arm the retransmit timer (2s, 4s, 8s, 16s...). */
static void transmit(udpRequest *request)
{
    uv_buf_t buffer = uv_buf_init((char *)request->packet, request->packetLength);
    uint64_t delay;
    int      result;

    result = uv_udp_try_send(&request->udp, &buffer, 1, NULL);
    if (result < 0 && result != UV_EAGAIN) {        /* EAGAIN: the retry resends */
        setError(request, "send failed: %s", uv_strerror(result));
        finish(request, UDP_ERROR);
        return;
    }

    delay = (uint64_t)UDP_RETRY_MS << request->retryCount;
    if (delay > UDP_MAX_RETRY_MS) {
        delay = UDP_MAX_RETRY_MS;
    }
    uv_timer_start(&request->retryTimer, onRetry, delay, 0);
}

static void beginStage(udpRequest *request, uint8_t stage)
{
    const udpAnnounceParams *params = &request->params;
    uint8_t                 *p = request->packet;

    request->stage = stage;
    request->retryCount = 0;
    request->transactionId = random32();

    if (stage == UDP_ACTION_CONNECT) {
        put64(p, UDP_PROTOCOL_ID);
        put32(p + 8, UDP_ACTION_CONNECT);
        put32(p + 12, request->transactionId);
        request->packetLength = UDP_CONNECT_SIZE;
    } else {
        put64(p, request->connectionId);
        put32(p + 8, UDP_ACTION_ANNOUNCE);
        put32(p + 12, request->transactionId);
        memcpy(p + 16, params->infoHash, PARSER_SHA1_LENGTH);
        memcpy(p + 36, params->peerId, UDP_PEER_ID_LENGTH);
        put64(p + 56, params->downloaded);
        put64(p + 64, params->left);
        put64(p + 72, params->uploaded);
        put32(p + 80, params->event);
        put32(p + 84, 0);                           /* IP: use the sender's */
        put32(p + 88, params->key);
        put32(p + 92, (uint32_t)params->numWant);
        put16(p + 96, params->port);
        request->packetLength = UDP_ANNOUNCE_SIZE;
    }
    transmit(request);
}

static void onAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    udpRequest *request = handle->data;

    (void)suggested;
    buffer->base = (char *)request->buffer;
    buffer->len = sizeof(request->buffer);
}

static void onRead(uv_udp_t *udp, ssize_t count, const uv_buf_t *buffer,
                   const struct sockaddr *from, unsigned flags)
{
    udpRequest    *request = udp->data;
    const uint8_t *data = request->buffer;
    uint64_t       length;
    uint32_t       action;

    (void)buffer;
    (void)from;
    if (request->finished) {
        return;
    }
    if (count < 0) {                                /* e.g. ECONNREFUSED via ICMP */
        setError(request, "receive failed: %s", uv_strerror((int)count));
        finish(request, UDP_ERROR);
        return;
    }
    length = (uint64_t)count;
    if (length < 8u || (flags & UV_UDP_PARTIAL) || get32(data + 4) != request->transactionId) {
        return;                                     /* not for us: ignore */
    }
    action = get32(data);

    if (action == UDP_ACTION_ERROR) {
        request->failureReason = copyBytes(data + 8, length - 8u);
        if (request->failureReason == NULL) {
            finish(request, UDP_ENOMEM);
            return;
        }
        setError(request, "tracker refused the announce");
        finish(request, UDP_EFAILURE);
    } else if (request->stage == UDP_ACTION_CONNECT &&
               action == UDP_ACTION_CONNECT && length >= 16u) {
        request->connectionId = get64(data + 8);
        beginStage(request, UDP_ACTION_ANNOUNCE);
    } else if (request->stage == UDP_ACTION_ANNOUNCE &&
               action == UDP_ACTION_ANNOUNCE && length >= UDP_ANNOUNCE_HEADER) {
        request->received = length;
        finish(request, UDP_OK);
    }
}

static void start(udpRequest *request, const struct sockaddr *address)
{
    int result;

    result = uv_udp_init(udpState.loop, &request->udp);
    if (result < 0) {
        setError(request, "socket failed: %s", uv_strerror(result));
        finish(request, UDP_ERROR);
        return;
    }
    request->udp.data = request;
    request->udpOpen = 1;
    request->pending++;
    request->ipv6 = (uint8_t)(address->sa_family == AF_INET6);

    result = uv_udp_connect(&request->udp, address);
    if (result == 0) {
        result = uv_udp_recv_start(&request->udp, onAlloc, onRead);
    }
    if (result == 0) {
        result = uv_timer_init(udpState.loop, &request->retryTimer);
    }
    if (result < 0) {
        setError(request, "could not reach %s: %s", request->host, uv_strerror(result));
        finish(request, UDP_ERROR);
        return;
    }
    request->retryTimer.data = request;
    request->retryOpen = 1;
    request->pending++;

    beginStage(request, UDP_ACTION_CONNECT);
}

static void onResolved(uv_getaddrinfo_t *resolver, int status, struct addrinfo *addresses)
{
    udpRequest      *request = resolver->data;
    struct addrinfo *entry;

    request->resolving = 0;
    request->pending--;

    if (request->finished) {
        if (addresses != NULL) {
            uv_freeaddrinfo(addresses);
        }
        maybeFree(request);
        return;
    }
    if (status < 0) {
        if (addresses != NULL) {
            uv_freeaddrinfo(addresses);
        }
        setError(request, "DNS lookup for %s failed: %s", request->host, uv_strerror(status));
        finish(request, UDP_ERROR);
        return;
    }

    for (entry = addresses; entry != NULL; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET || entry->ai_family == AF_INET6) {
            break;
        }
    }
    if (entry == NULL) {
        uv_freeaddrinfo(addresses);
        setError(request, "DNS lookup for %s returned no usable address", request->host);
        finish(request, UDP_ERROR);
        return;
    }
    start(request, entry->ai_addr);                 /* uses the first address */
    uv_freeaddrinfo(addresses);
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t udpInit(uv_loop_t *loop, parserContext *context)
{
    if (loop == NULL || context == NULL || context->magic != PARSER_CONTEXT_MAGIC) {
        return UDP_EINVAL;
    }
    if (udpState.magic == UDP_STATE_MAGIC) {
        if (loop != udpState.loop || context != udpState.context) {
            return UDP_EINVAL;
        }
        udpState.users++;
        return UDP_OK;
    }
    udpState.loop = loop;
    udpState.context = context;
    udpState.users = 1;
    TAILQ_INIT(&udpState.requests);
    udpState.magic = UDP_STATE_MAGIC;
    return UDP_OK;
}

int32_t udpAnnounce(const char *url, const udpAnnounceParams *params,
                    uint32_t timeoutSeconds, udpCallback callback,
                    void *privateArg, udpRequest **handle)
{
    udpRequest     *request;
    httpUrl         parts;
    struct addrinfo hints;
    char            portText[8];
    int             status;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (udpState.magic != UDP_STATE_MAGIC) {
        return UDP_ENOTINIT;
    }
    if (params == NULL || httpUrlParse(url, &parts) != HTTP_OK) {
        return UDP_EINVAL;
    }
    if (parts.schemeLength != 3 ||
        (parts.scheme[0] | 0x20) != 'u' || (parts.scheme[1] | 0x20) != 'd' ||
        (parts.scheme[2] | 0x20) != 'p') {
        return UDP_ENOTSUP;                         /* scheme is not udp (any case) */
    }
    if (parts.port == 0) {
        return UDP_EINVAL;                          /* a port is required */
    }

    request = allocate(sizeof(*request));
    if (request == NULL) {
        return UDP_ENOMEM;
    }
    request->callback = callback;
    request->privateArg = privateArg;
    request->timeoutSeconds = timeoutSeconds;
    request->params = *params;
    request->host = copyBytes(parts.host, parts.hostLength);
    if (request->host == NULL) {
        freeRequest(request);
        return UDP_ENOMEM;
    }

    /* From here on, failures must close what has been opened. */
    request->detached = 1;                          /* not listed until started */
    if (timeoutSeconds > 0) {
        uv_timer_init(udpState.loop, &request->timeoutTimer);
        request->timeoutTimer.data = request;
        request->timeoutOpen = 1;
        request->pending++;
        uv_timer_start(&request->timeoutTimer, onTimeout, (uint64_t)timeoutSeconds * 1000u, 0);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portText, sizeof(portText), "%u", (unsigned)parts.port);

    request->resolver.data = request;
    status = uv_getaddrinfo(udpState.loop, &request->resolver, onResolved,
                            request->host, portText, &hints);
    if (status < 0) {
        request->finished = 1;
        closeAll(request);
        maybeFree(request);                         /* now, or when the timer closes */
        return UDP_ERROR;
    }
    request->resolving = 1;
    request->pending++;

    request->detached = 0;
    TAILQ_INSERT_TAIL(&udpState.requests, request, entries);
    if (handle != NULL) {
        *handle = request;
    }
    return UDP_OK;
}

void udpCancel(udpRequest *request)
{
    if (request == NULL || request->detached) {
        return;
    }
    detach(request);
    if (!request->finished) {
        request->finished = 1;
        closeAll(request);
    }
    maybeFree(request);
}

void udpShutdown(void)
{
    udpRequest *request;

    if (udpState.magic != UDP_STATE_MAGIC || --udpState.users > 0) {
        return;
    }
    while ((request = TAILQ_FIRST(&udpState.requests)) != NULL) {
        udpCancel(request);
    }
    /* loop and context stay set: closing requests still need the allocator. */
    udpState.magic = 0;
}

const char *udpStatusString(int32_t status)
{
    switch (status) {
    case UDP_OK:        return "OK";
    case UDP_ERROR:     return "Network error";
    case UDP_EINVAL:    return "Invalid argument or URL";
    case UDP_ENOMEM:    return "Out of memory";
    case UDP_ETIMEDOUT: return "Timed out";
    case UDP_ENOTSUP:   return "Not a udp:// URL";
    case UDP_EFAILURE:  return "Tracker returned a failure";
    case UDP_EPROTO:    return "Invalid tracker response";
    case UDP_ENOTINIT:  return "UDP module not initialised";
    default:            return "Unknown status";
    }
}
