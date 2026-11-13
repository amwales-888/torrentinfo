/*
 * tracker.c - Asynchronous BitTorrent tracker announces using libuv.
 *
 * Each announce is a trackerRequest that moves through:
 *
 *   HTTP/HTTPS: build the announce URL -> httpGet() (http.c) -> parse the
 *               bencoded body
 *   UDP:        resolve (uv_getaddrinfo) -> connect request -> connect
 *               response -> announce request -> announce response,
 *               retransmitting on a timer (BEP 15)
 *
 * A single timeout timer covers the whole announce, for every protocol.
 *
 * Lifetime: libuv handles must be closed with uv_close() and requests must
 * complete before their memory is freed. (An HTTP request manages its own
 * handles inside http.c; we only cancel it.) 'pending' counts every libuv
 * callback still to come (open handles plus in-flight requests). A request
 * is freed only when it has been closed by the user (detached) and pending
 * has reached zero. When an announce finishes, its handles are closed at
 * once, but its results stay until trackerClose().
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>

#include "http.h"
#include "tracker.h"

#define TRACKER_STATE_MAGIC         0x5452434Bu     /* "TRCK" */
#define TRACKER_PROTOCOL_HTTP       1u              /* http and https */
#define TRACKER_PROTOCOL_UDP        2u
#define TRACKER_MAX_ADDRESSES       8u
#define TRACKER_UDP_BUFFER          8192u
#define TRACKER_UDP_RETRY_MS        2000u           /* first retransmit      */
#define TRACKER_UDP_MAX_RETRY_MS    16000u          /* retransmit back-off cap */
#define TRACKER_UDP_PROTOCOL_ID     0x41727101980ull
#define TRACKER_UDP_ACTION_CONNECT  0u
#define TRACKER_UDP_ACTION_ANNOUNCE 1u
#define TRACKER_UDP_ACTION_ERROR    3u
#define TRACKER_UDP_EVENT_STARTED   2u
#define TRACKER_UDP_CONNECT_SIZE    16u
#define TRACKER_UDP_ANNOUNCE_SIZE   98u
#define TRACKER_CANCELLED           (-100)          /* internal only         */
/*
 * Bytes left to download, as reported to the tracker. The announce API has
 * no per-torrent statistics yet, so report a small non-zero amount: this
 * marks us as a leecher, so trackers include seeders in the peer list.
 */
#define TRACKER_ANNOUNCE_LEFT       16384u

typedef struct trackerRequest {
    uint32_t                id;
    void                   *privateArg;
    trackerCallback         callback;
    int32_t                 status;
    uint8_t                 finished;       /* status is final; handles closing */
    uint8_t                 detached;       /* trackerClose() called            */
    uint32_t                pending;        /* libuv callbacks still to come    */
    uint32_t                timeoutSeconds;

    /* Parsed URL */
    uint8_t                 protocol;
    char                   *url;
    char                   *host;           /* UDP: for DNS                     */
    uint16_t                port;
    uint8_t                 infoHash[PARSER_SHA1_LENGTH];

    /* HTTP and HTTPS: the request in http.c, or NULL when not running */
    httpRequest            *http;

    /* UDP: addresses from DNS */
    struct sockaddr_storage addresses[TRACKER_MAX_ADDRESSES];
    uint32_t                addressCount;

    /* libuv handles and requests; the flags say which handles are open */
    uv_getaddrinfo_t        resolver;
    uv_timer_t              timeoutTimer;
    uv_timer_t              retryTimer;
    uv_udp_t                udp;
    uint8_t                 resolving;
    uint8_t                 timeoutOpen;
    uint8_t                 retryOpen;
    uint8_t                 udpOpen;

    /* UDP */
    uint8_t                 udpStage;       /* TRACKER_UDP_ACTION_CONNECT/ANNOUNCE */
    uint8_t                 udpIpv6;
    uint32_t                transactionId;
    uint64_t                connectionId;
    uint32_t                retryCount;
    uint8_t                 packet[TRACKER_UDP_ANNOUNCE_SIZE];
    uint32_t                packetLength;
    uint8_t                 ioBuffer[TRACKER_UDP_BUFFER];

    /* Results */
    uint32_t                interval;
    uint32_t                minInterval;
    int64_t                 seeders;
    int64_t                 leechers;
    char                   *failureReason;
    char                   *warningMessage;
    char                    errorMessage[TRACKER_ERROR_MESSAGE_LENGTH];
    trackerPeer            *peers;
    uint32_t                peerCount;
    uint32_t                peerCapacity;

    TAILQ_ENTRY(trackerRequest) entries;
} trackerRequest;
TAILQ_HEAD(trackerRequestList, trackerRequest);

static struct {
    uv_loop_t                *loop;
    parserContext            *context;
    trackerConfig             config;
    uint32_t                  key;          /* random, sent in announces */
    uint32_t                  nextId;
    uint32_t                  magic;
    struct trackerRequestList requests;
} trackerState;

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static void *allocate(uint64_t size)
{
    void *memory;

    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    memory = trackerState.context->allocator.realloc(NULL, (size_t)size);
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
    return trackerState.context->allocator.realloc(memory, (size_t)size);
}

static void release(void *memory)
{
    if (memory != NULL) {
        trackerState.context->allocator.free(memory);
    }
}

/* Copy bytes into a new NUL-terminated string, or return NULL. */
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

/* Replace *field with a copy of the given bytes. */
static int32_t replaceString(char **field, const uint8_t *data, uint64_t length)
{
    char *copy = copyBytes(data, length);

    if (copy == NULL) {
        return TRACKER_ENOMEM;
    }
    release(*field);
    *field = copy;
    return TRACKER_OK;
}

static void setError(trackerRequest *request, const char *format, ...)
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

static uint32_t numWant(void)
{
    return (trackerState.config.numWant != 0) ? trackerState.config.numWant
                                              : TRACKER_DEFAULT_NUM_WANT;
}

static trackerRequest *findRequest(uint32_t id)
{
    trackerRequest *request;

    TAILQ_FOREACH(request, &trackerState.requests, entries) {
        if (request->id == id) {
            return request;
        }
    }
    return NULL;
}

static uint32_t allocateId(void)
{
    uint32_t id;

    do {
        id = trackerState.nextId++;
        if (trackerState.nextId == 0) {
            trackerState.nextId = 1;
        }
    } while (id == 0 || findRequest(id) != NULL);
    return id;
}

/* ======================================================================== */
/* Request lifetime                                                         */
/* ======================================================================== */

static void freeRequest(trackerRequest *request)
{
    release(request->url);
    release(request->host);
    release(request->failureReason);
    release(request->warningMessage);
    release(request->peers);
    release(request);
}

static void maybeFree(trackerRequest *request)
{
    if (request->detached && request->pending == 0) {
        freeRequest(request);
    }
}

static void onHandleClosed(uv_handle_t *handle)
{
    trackerRequest *request = handle->data;

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
static void closeAll(trackerRequest *request)
{
    if (request->http != NULL) {
        httpCancel(request->http);                  /* its callback will not run */
        request->http = NULL;
    }
    if (request->resolving) {
        /* If the lookup is already running this fails, and its callback
         * simply arrives later. */
        uv_cancel((uv_req_t *)&request->resolver);
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

/*
 * Record the final status and tell the user. Handles close asynchronously,
 * so 'pending' stays above zero and the request cannot be freed during the
 * callback, even if the callback calls trackerClose().
 */
static void finish(trackerRequest *request, int32_t status)
{
    if (request->finished) {
        return;
    }
    request->finished = 1;
    request->status = status;
    if (status != TRACKER_OK && request->errorMessage[0] == '\0') {
        setError(request, "%s", trackerStatusString(status));
    }
    closeAll(request);

    if (!request->detached && request->callback != NULL) {
        request->callback(request->privateArg, request->id);
    }
}

static void onTimeout(uv_timer_t *timer)
{
    trackerRequest *request = timer->data;

    setError(request, "no response within %" PRIu32 " seconds", request->timeoutSeconds);
    finish(request, TRACKER_ETIMEDOUT);
}

/* ======================================================================== */
/* URL handling                                                             */
/* ======================================================================== */

static int32_t fromHttpStatus(int32_t status)
{
    switch (status) {
    case HTTP_OK:        return TRACKER_OK;
    case HTTP_EINVAL:    return TRACKER_EINVAL;
    case HTTP_ENOMEM:    return TRACKER_ENOMEM;
    case HTTP_ETIMEDOUT: return TRACKER_ETIMEDOUT;
    case HTTP_ENOTSUP:   return TRACKER_ENOTSUP;
    case HTTP_EPROTO:    return TRACKER_EPROTO;
    case HTTP_ENOTINIT:  return TRACKER_ENOTINIT;
    case HTTP_ETLS:      return TRACKER_ETLS;
    default:             return TRACKER_ERROR;
    }
}

static uint8_t schemeIs(const httpUrl *url, const char *scheme)
{
    size_t   length = strlen(scheme);
    uint32_t i;

    if (url->schemeLength != length) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        char c = url->scheme[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != scheme[i]) {
            return 0;
        }
    }
    return 1;
}

/* Check the URL and record what the request needs from it. */
static int32_t parseUrl(trackerRequest *request, const char *url, httpUrl *parts)
{
    if (strlen(url) > TRACKER_MAX_URL_LENGTH) {
        return TRACKER_EINVAL;
    }
    if (httpUrlParse(url, parts) != HTTP_OK) {
        return TRACKER_EINVAL;
    }
    if (schemeIs(parts, "http") || schemeIs(parts, "https")) {
        request->protocol = TRACKER_PROTOCOL_HTTP;
    } else if (schemeIs(parts, "udp")) {
        request->protocol = TRACKER_PROTOCOL_UDP;
        if (parts->port == 0) {
            return TRACKER_EINVAL;                  /* UDP needs a port */
        }
    } else {
        return TRACKER_ENOTSUP;
    }
    request->port = parts->port;
    request->url = copyBytes(url, strlen(url));
    request->host = copyBytes(parts->host, parts->hostLength);
    return (request->url != NULL && request->host != NULL) ? TRACKER_OK : TRACKER_ENOMEM;
}

/* Percent-encode binary data: 20 bytes need at most 61 characters. */
static void urlEncode(const uint8_t *data, uint32_t length, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t          i;

    for (i = 0; i < length; i++) {
        uint8_t c = data[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            *out++ = (char)c;
        } else {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 0x0Fu];
        }
    }
    *out = '\0';
}

/*
 * The tracker URL (without any #fragment) plus the announce parameters,
 * joined with '&' if the URL already has a query (e.g. a passkey).
 */
static char *buildAnnounceUrl(const trackerRequest *request, const httpUrl *parts)
{
    char     infoHashText[PARSER_SHA1_LENGTH * 3u + 1u];
    char     peerIdText[TRACKER_PEER_ID_LENGTH * 3u + 1u];
    /* parts points into the caller's copy of the URL; measure within it. */
    uint64_t baseLength = (uint64_t)(parts->path + parts->pathLength - parts->scheme);
    uint64_t size = baseLength + 320u;
    char    *url;
    int      written;

    urlEncode(request->infoHash, PARSER_SHA1_LENGTH, infoHashText);
    urlEncode(trackerState.config.peerId, TRACKER_PEER_ID_LENGTH, peerIdText);

    url = allocate(size);
    if (url == NULL) {
        return NULL;
    }
    written = snprintf(url, (size_t)size,
                       "%.*s%cinfo_hash=%s&peer_id=%s&port=%u&uploaded=0"
                       "&downloaded=0&left=%u&compact=1&numwant=%" PRIu32
                       "&key=%08" PRIx32 "&event=started",
                       (int)baseLength, request->url,
                       (memchr(parts->path, '?', parts->pathLength) != NULL) ? '&' : '?',
                       infoHashText, peerIdText, (unsigned)trackerState.config.port,
                       (unsigned)TRACKER_ANNOUNCE_LEFT, numWant(), trackerState.key);
    if (written < 0 || (uint64_t)written >= size) {
        release(url);
        return NULL;
    }
    return url;
}

/* ======================================================================== */
/* Announce response parsing                                                */
/* ======================================================================== */

static int32_t appendPeer(trackerRequest *request, uint8_t family,
                          const uint8_t *address, uint16_t port)
{
    trackerPeer *peers;
    uint32_t     capacity;

    if (request->peerCount == request->peerCapacity) {
        if (request->peerCapacity > UINT32_MAX / 2u) {
            return TRACKER_EPROTO;
        }
        capacity = (request->peerCapacity != 0) ? request->peerCapacity * 2u : 64u;
        peers = resize(request->peers, (uint64_t)capacity * sizeof(*peers));
        if (peers == NULL) {
            return TRACKER_ENOMEM;
        }
        request->peers = peers;
        request->peerCapacity = capacity;
    }
    peers = &request->peers[request->peerCount++];
    memset(peers, 0, sizeof(*peers));
    peers->family = family;
    memcpy(peers->address, address, (family == TRACKER_FAMILY_IPV4) ? 4u : 16u);
    peers->port = port;
    return TRACKER_OK;
}

/* Compact peers: 4-byte IPv4 or 16-byte IPv6 address, then 2-byte port. */
static int32_t addCompactPeers(trackerRequest *request, const uint8_t *data,
                               uint64_t length, uint8_t family)
{
    uint64_t addressSize = (family == TRACKER_FAMILY_IPV4) ? 4u : 16u;
    uint64_t entrySize = addressSize + 2u;
    uint64_t offset;

    for (offset = 0; offset + entrySize <= length; offset += entrySize) {
        uint16_t port = (uint16_t)(((uint32_t)data[offset + addressSize] << 8) |
                                   data[offset + addressSize + 1u]);
        int32_t  result = appendPeer(request, family, data + offset, port);

        if (result != TRACKER_OK) {
            return result;
        }
    }
    return TRACKER_OK;
}

/* Non-compact peers: a list of { "ip": string, "port": int, ... } */
static int32_t addPeerList(trackerRequest *request, parserCursor *cursor)
{
    const uint8_t *key;
    const uint8_t *ip;
    uint64_t       keyLength;
    uint64_t       ipLength;
    int64_t        port;
    uint8_t        more;
    uint8_t        address[16];
    char           text[64];
    int32_t        result;

    if (parserContainerBegin(cursor, 'l') != PARSER_OK) {
        return TRACKER_EPROTO;
    }
    for (;;) {
        if (parserContainerNext(cursor, &more) != PARSER_OK) {
            return TRACKER_EPROTO;
        }
        if (!more) {
            break;
        }
        if (parserContainerBegin(cursor, 'd') != PARSER_OK) {
            return TRACKER_EPROTO;
        }
        ip = NULL;
        ipLength = 0;
        port = -1;
        for (;;) {
            if (parserContainerNext(cursor, &more) != PARSER_OK) {
                return TRACKER_EPROTO;
            }
            if (!more) {
                break;
            }
            if (parserReadString(cursor, &key, &keyLength) != PARSER_OK) {
                return TRACKER_EPROTO;
            }
            if (keyLength == 2 && memcmp(key, "ip", 2) == 0 &&
                parserCursorPeek(cursor) >= '0' && parserCursorPeek(cursor) <= '9') {
                if (parserReadString(cursor, &ip, &ipLength) != PARSER_OK) {
                    return TRACKER_EPROTO;
                }
            } else if (keyLength == 4 && memcmp(key, "port", 4) == 0 &&
                       parserCursorPeek(cursor) == 'i') {
                if (parserReadInteger(cursor, &port) != PARSER_OK) {
                    return TRACKER_EPROTO;
                }
            } else if (parserSkipValue(cursor) != PARSER_OK) {
                return TRACKER_EPROTO;
            }
        }

        /* Peers given by host name, or without a valid port, are skipped. */
        if (ip == NULL || ipLength >= sizeof(text) || port < 0 || port > 65535) {
            continue;
        }
        memcpy(text, ip, (size_t)ipLength);
        text[ipLength] = '\0';
        if (uv_inet_pton(AF_INET, text, address) == 0) {
            result = appendPeer(request, TRACKER_FAMILY_IPV4, address, (uint16_t)port);
        } else if (uv_inet_pton(AF_INET6, text, address) == 0) {
            result = appendPeer(request, TRACKER_FAMILY_IPV6, address, (uint16_t)port);
        } else {
            continue;
        }
        if (result != TRACKER_OK) {
            return result;
        }
    }
    return TRACKER_OK;
}

static uint32_t clampInterval(int64_t value)
{
    if (value < 0) {
        return 0;
    }
    return (value > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)value;
}

static uint8_t keyIs(const uint8_t *key, uint64_t keyLength, const char *literal)
{
    size_t literalLength = strlen(literal);

    return (uint8_t)(keyLength == literalLength && memcmp(key, literal, literalLength) == 0);
}

/* The bencoded body of an HTTP announce response. */
static int32_t parseAnnounce(trackerRequest *request, const uint8_t *data, uint64_t length)
{
    parserCursor   cursor = { data, length, 0, 0 };
    const uint8_t *key;
    const uint8_t *value;
    uint64_t       keyLength;
    uint64_t       valueLength;
    int64_t        integer;
    uint8_t        more;
    int32_t        result = TRACKER_OK;

    if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
        goto malformed;
    }
    for (;;) {
        if (parserContainerNext(&cursor, &more) != PARSER_OK) {
            goto malformed;
        }
        if (!more) {
            break;
        }
        if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK) {
            goto malformed;
        }

        if (keyIs(key, keyLength, "failure reason") ||
            keyIs(key, keyLength, "warning message")) {
            if (parserReadString(&cursor, &value, &valueLength) != PARSER_OK) {
                goto malformed;
            }
            result = replaceString(keyIs(key, keyLength, "failure reason")
                                       ? &request->failureReason
                                       : &request->warningMessage,
                                   value, valueLength);
        } else if (keyIs(key, keyLength, "interval") ||
                   keyIs(key, keyLength, "min interval") ||
                   keyIs(key, keyLength, "complete") ||
                   keyIs(key, keyLength, "incomplete")) {
            if (parserReadInteger(&cursor, &integer) != PARSER_OK) {
                goto malformed;
            }
            if (keyIs(key, keyLength, "interval")) {
                request->interval = clampInterval(integer);
            } else if (keyIs(key, keyLength, "min interval")) {
                request->minInterval = clampInterval(integer);
            } else if (keyIs(key, keyLength, "complete")) {
                request->seeders = integer;
            } else {
                request->leechers = integer;
            }
        } else if (keyIs(key, keyLength, "peers")) {
            if (parserCursorPeek(&cursor) == 'l') {
                result = addPeerList(request, &cursor);
            } else if (parserReadString(&cursor, &value, &valueLength) == PARSER_OK) {
                result = addCompactPeers(request, value, valueLength, TRACKER_FAMILY_IPV4);
            } else {
                goto malformed;
            }
        } else if (keyIs(key, keyLength, "peers6")) {
            if (parserReadString(&cursor, &value, &valueLength) != PARSER_OK) {
                goto malformed;
            }
            result = addCompactPeers(request, value, valueLength, TRACKER_FAMILY_IPV6);
        } else if (parserSkipValue(&cursor) != PARSER_OK) {
            goto malformed;
        }

        if (result == TRACKER_EPROTO) {
            goto malformed;
        }
        if (result != TRACKER_OK) {
            return result;
        }
    }

    if (request->failureReason != NULL) {
        setError(request, "tracker refused the announce");
        return TRACKER_EFAILURE;
    }
    return TRACKER_OK;

malformed:
    setError(request, "malformed announce response");
    return TRACKER_EPROTO;
}

/* ======================================================================== */
/* HTTP and HTTPS (the transport is in http.c)                              */
/* ======================================================================== */

static void onHttpDone(void *privateArg, const httpResponse *response)
{
    trackerRequest *request = privateArg;

    request->http = NULL;                           /* http.c frees it after this */
    if (response->status != HTTP_OK) {
        setError(request, "%s", response->errorMessage);
        finish(request, fromHttpStatus(response->status));
    } else if (response->statusCode != 200u) {
        setError(request, "HTTP status %" PRIu32, response->statusCode);
        finish(request, TRACKER_EPROTO);
    } else {
        finish(request, parseAnnounce(request, response->body, response->bodyLength));
    }
}

/* ======================================================================== */
/* UDP (BEP 15)                                                             */
/* ======================================================================== */

static void udpTransmit(trackerRequest *request);

static void onUdpRetry(uv_timer_t *timer)
{
    trackerRequest *request = timer->data;

    if (request->finished) {
        return;
    }
    if (request->retryCount < 16u) {
        request->retryCount++;
    }
    udpTransmit(request);
}

/* Send the current packet and arm the retransmit timer (2s, 4s, 8s, 16s...). */
static void udpTransmit(trackerRequest *request)
{
    uv_buf_t buffer = uv_buf_init((char *)request->packet, request->packetLength);
    uint64_t delay;
    int      result;

    result = uv_udp_try_send(&request->udp, &buffer, 1, NULL);
    if (result < 0 && result != UV_EAGAIN) {        /* EAGAIN: the retry resends */
        setError(request, "send failed: %s", uv_strerror(result));
        finish(request, TRACKER_ERROR);
        return;
    }

    delay = (uint64_t)TRACKER_UDP_RETRY_MS << request->retryCount;
    if (delay > TRACKER_UDP_MAX_RETRY_MS) {
        delay = TRACKER_UDP_MAX_RETRY_MS;
    }
    uv_timer_start(&request->retryTimer, onUdpRetry, delay, 0);
}

static void udpBeginStage(trackerRequest *request, uint8_t stage)
{
    uint8_t *p = request->packet;

    request->udpStage = stage;
    request->retryCount = 0;
    request->transactionId = random32();

    if (stage == TRACKER_UDP_ACTION_CONNECT) {
        put64(p, TRACKER_UDP_PROTOCOL_ID);
        put32(p + 8, TRACKER_UDP_ACTION_CONNECT);
        put32(p + 12, request->transactionId);
        request->packetLength = TRACKER_UDP_CONNECT_SIZE;
    } else {
        put64(p, request->connectionId);
        put32(p + 8, TRACKER_UDP_ACTION_ANNOUNCE);
        put32(p + 12, request->transactionId);
        memcpy(p + 16, request->infoHash, PARSER_SHA1_LENGTH);
        memcpy(p + 36, trackerState.config.peerId, TRACKER_PEER_ID_LENGTH);
        put64(p + 56, 0);                           /* downloaded */
        put64(p + 64, TRACKER_ANNOUNCE_LEFT);       /* left       */
        put64(p + 72, 0);                           /* uploaded   */
        put32(p + 80, TRACKER_UDP_EVENT_STARTED);
        put32(p + 84, 0);                           /* IP: use sender's */
        put32(p + 88, trackerState.key);
        put32(p + 92, numWant());
        put16(p + 96, trackerState.config.port);
        request->packetLength = TRACKER_UDP_ANNOUNCE_SIZE;
    }
    udpTransmit(request);
}

static void onUdpAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    trackerRequest *request = handle->data;

    (void)suggested;
    buffer->base = (char *)request->ioBuffer;
    buffer->len = sizeof(request->ioBuffer);
}

static void onUdpRead(uv_udp_t *udp, ssize_t count, const uv_buf_t *buffer,
                      const struct sockaddr *from, unsigned flags)
{
    trackerRequest *request = udp->data;
    const uint8_t  *data = request->ioBuffer;
    uint64_t        length;
    uint32_t        action;
    int32_t         result;

    (void)buffer;
    (void)from;
    if (request->finished) {
        return;
    }
    if (count < 0) {                                /* e.g. ECONNREFUSED via ICMP */
        setError(request, "receive failed: %s", uv_strerror((int)count));
        finish(request, TRACKER_ERROR);
        return;
    }
    length = (uint64_t)count;
    if (length < 8u || (flags & UV_UDP_PARTIAL) || get32(data + 4) != request->transactionId) {
        return;                                     /* not for us: ignore */
    }
    action = get32(data);

    if (action == TRACKER_UDP_ACTION_ERROR) {
        result = replaceString(&request->failureReason, data + 8, length - 8u);
        if (result == TRACKER_OK) {
            setError(request, "tracker refused the announce");
            result = TRACKER_EFAILURE;
        }
        finish(request, result);
    } else if (request->udpStage == TRACKER_UDP_ACTION_CONNECT &&
               action == TRACKER_UDP_ACTION_CONNECT && length >= 16u) {
        request->connectionId = get64(data + 8);
        udpBeginStage(request, TRACKER_UDP_ACTION_ANNOUNCE);
    } else if (request->udpStage == TRACKER_UDP_ACTION_ANNOUNCE &&
               action == TRACKER_UDP_ACTION_ANNOUNCE && length >= 20u) {
        request->interval = get32(data + 8);
        request->leechers = (int64_t)get32(data + 12);
        request->seeders = (int64_t)get32(data + 16);
        /* Peer address size follows the address family we reached the tracker on. */
        finish(request, addCompactPeers(request, data + 20, length - 20u,
                                        request->udpIpv6 ? TRACKER_FAMILY_IPV6
                                                         : TRACKER_FAMILY_IPV4));
    }
}

static void udpStart(trackerRequest *request)
{
    const struct sockaddr *address = (const struct sockaddr *)&request->addresses[0];
    int                    result;

    result = uv_udp_init(trackerState.loop, &request->udp);
    if (result < 0) {
        setError(request, "socket failed: %s", uv_strerror(result));
        finish(request, TRACKER_ERROR);
        return;
    }
    request->udp.data = request;
    request->udpOpen = 1;
    request->pending++;
    request->udpIpv6 = (uint8_t)(address->sa_family == AF_INET6);

    result = uv_udp_connect(&request->udp, address);
    if (result == 0) {
        result = uv_udp_recv_start(&request->udp, onUdpAlloc, onUdpRead);
    }
    if (result == 0) {
        result = uv_timer_init(trackerState.loop, &request->retryTimer);
    }
    if (result < 0) {
        setError(request, "could not reach %s: %s", request->host, uv_strerror(result));
        finish(request, TRACKER_ERROR);
        return;
    }
    request->retryTimer.data = request;
    request->retryOpen = 1;
    request->pending++;

    udpBeginStage(request, TRACKER_UDP_ACTION_CONNECT);
}

/* ======================================================================== */
/* DNS                                                                      */
/* ======================================================================== */

static void onResolved(uv_getaddrinfo_t *resolver, int status, struct addrinfo *addresses)
{
    trackerRequest  *request = resolver->data;
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
        finish(request, TRACKER_ERROR);
        return;
    }

    for (entry = addresses; entry != NULL && request->addressCount < TRACKER_MAX_ADDRESSES;
         entry = entry->ai_next) {
        if ((entry->ai_family == AF_INET || entry->ai_family == AF_INET6) &&
            entry->ai_addrlen <= sizeof(request->addresses[0])) {
            memcpy(&request->addresses[request->addressCount++], entry->ai_addr,
                   entry->ai_addrlen);
        }
    }
    uv_freeaddrinfo(addresses);

    if (request->addressCount == 0) {
        setError(request, "DNS lookup for %s returned no usable address", request->host);
        finish(request, TRACKER_ERROR);
    } else {
        udpStart(request);
    }
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t trackerInit(uv_loop_t *loop, parserContext *context, const trackerConfig *config)
{
    httpConfig httpConfiguration;
    int32_t    result;

    if (loop == NULL || context == NULL || config == NULL ||
        context->magic != PARSER_CONTEXT_MAGIC) {
        return TRACKER_EINVAL;
    }
    if (trackerState.magic == TRACKER_STATE_MAGIC) {
        return TRACKER_EINVAL;                      /* already initialised */
    }

    /* HTTP and HTTPS trackers go through the http module. */
    httpConfiguration.caFile = config->caFile;
    httpConfiguration.userAgent = NULL;
    result = httpInit(loop, context, &httpConfiguration);
    if (result != HTTP_OK) {
        return fromHttpStatus(result);
    }

    trackerState.loop = loop;
    trackerState.context = context;
    trackerState.config = *config;
    trackerState.key = random32();
    trackerState.nextId = 1;
    TAILQ_INIT(&trackerState.requests);
    trackerState.magic = TRACKER_STATE_MAGIC;
    return TRACKER_OK;
}

int32_t trackerConnect(const uint8_t infoHash[PARSER_SHA1_LENGTH],
                       const char *trackerUrl, uint32_t *uniqueId,
                       uint32_t timeoutSeconds, trackerCallback callback,
                       void *privateArg)
{
    trackerRequest *request;
    struct addrinfo hints;
    httpUrl         parts;
    char            portText[8];
    char           *announceUrl;
    int32_t         result;
    int             status;

    if (trackerState.magic != TRACKER_STATE_MAGIC) {
        return TRACKER_ENOTINIT;
    }
    if (infoHash == NULL || trackerUrl == NULL || uniqueId == NULL || timeoutSeconds == 0) {
        return TRACKER_EINVAL;
    }
    *uniqueId = 0;

    request = allocate(sizeof(*request));
    if (request == NULL) {
        return TRACKER_ENOMEM;
    }
    request->status = TRACKER_PENDING;
    request->seeders = -1;
    request->leechers = -1;
    request->callback = callback;
    request->privateArg = privateArg;
    request->timeoutSeconds = timeoutSeconds;
    memcpy(request->infoHash, infoHash, PARSER_SHA1_LENGTH);

    result = parseUrl(request, trackerUrl, &parts);
    if (result == TRACKER_OK && request->protocol == TRACKER_PROTOCOL_HTTP) {
        /* http.c does DNS, TCP, TLS and reading; our timer covers it all. */
        announceUrl = buildAnnounceUrl(request, &parts);
        if (announceUrl == NULL) {
            result = TRACKER_ENOMEM;
        } else {
            result = fromHttpStatus(httpGet(announceUrl, 0, TRACKER_MAX_RESPONSE,
                                            onHttpDone, request, &request->http));
            release(announceUrl);
        }
    }
    if (result != TRACKER_OK) {
        freeRequest(request);                       /* nothing is running yet */
        return result;
    }

    /* From here on, handles are open: failures must close them. */
    uv_timer_init(trackerState.loop, &request->timeoutTimer);
    request->timeoutTimer.data = request;
    request->timeoutOpen = 1;
    request->pending++;
    uv_timer_start(&request->timeoutTimer, onTimeout, (uint64_t)timeoutSeconds * 1000u, 0);

    if (request->protocol == TRACKER_PROTOCOL_UDP) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        snprintf(portText, sizeof(portText), "%u", (unsigned)request->port);

        request->resolver.data = request;
        status = uv_getaddrinfo(trackerState.loop, &request->resolver, onResolved,
                                request->host, portText, &hints);
        if (status < 0) {
            request->finished = 1;
            request->detached = 1;
            closeAll(request);                      /* freed once the timer closes */
            return TRACKER_ERROR;
        }
        request->resolving = 1;
        request->pending++;
    }

    request->id = allocateId();
    TAILQ_INSERT_TAIL(&trackerState.requests, request, entries);
    *uniqueId = request->id;
    return TRACKER_OK;
}

int32_t trackerInfo(void *privateArg, uint32_t uniqueId, trackerResult *result)
{
    trackerRequest *request;

    if (trackerState.magic != TRACKER_STATE_MAGIC) {
        return TRACKER_ENOTINIT;
    }
    request = findRequest(uniqueId);
    if (request == NULL || request->privateArg != privateArg) {
        return TRACKER_ENOENT;
    }
    if (result == NULL) {
        return TRACKER_EINVAL;
    }

    memset(result, 0, sizeof(*result));
    result->status = request->status;
    result->url = request->url;
    memcpy(result->infoHash, request->infoHash, PARSER_SHA1_LENGTH);
    result->seeders = -1;
    result->leechers = -1;

    if (request->status != TRACKER_PENDING) {
        result->interval = request->interval;
        result->minInterval = request->minInterval;
        result->seeders = request->seeders;
        result->leechers = request->leechers;
        result->failureReason = request->failureReason;
        result->warningMessage = request->warningMessage;
        result->errorMessage = (request->status == TRACKER_OK) ? NULL : request->errorMessage;
        result->peers = request->peers;
        result->peerCount = request->peerCount;
    }
    return request->status;
}

int32_t trackerClose(uint32_t uniqueId)
{
    trackerRequest *request;

    if (trackerState.magic != TRACKER_STATE_MAGIC) {
        return TRACKER_ENOTINIT;
    }
    request = findRequest(uniqueId);
    if (request == NULL) {
        return TRACKER_ENOENT;
    }

    TAILQ_REMOVE(&trackerState.requests, request, entries);
    request->detached = 1;
    if (!request->finished) {
        request->finished = 1;
        request->status = TRACKER_CANCELLED;
        closeAll(request);
    }
    maybeFree(request);
    return TRACKER_OK;
}

int32_t trackerList(uint32_t *ids, uint32_t capacity, uint32_t *count)
{
    trackerRequest *request;
    uint32_t        total = 0;

    if (trackerState.magic != TRACKER_STATE_MAGIC) {
        return TRACKER_ENOTINIT;
    }
    if (count == NULL || (capacity > 0 && ids == NULL)) {
        return TRACKER_EINVAL;
    }
    TAILQ_FOREACH(request, &trackerState.requests, entries) {
        if (total < capacity) {
            ids[total] = request->id;
        }
        total++;
    }
    *count = total;
    return TRACKER_OK;
}

void trackerShutdown(void)
{
    trackerRequest *request;

    if (trackerState.magic != TRACKER_STATE_MAGIC) {
        return;
    }
    while ((request = TAILQ_FIRST(&trackerState.requests)) != NULL) {
        trackerClose(request->id);
    }
    /* loop and context stay set: closing requests still need the allocator. */
    trackerState.magic = 0;
    httpShutdown();
}

const char *trackerStatusString(int32_t status)
{
    switch (status) {
    case TRACKER_OK:        return "OK";
    case TRACKER_PENDING:   return "Pending";
    case TRACKER_ERROR:     return "Network error";
    case TRACKER_ENOENT:    return "No such tracker request";
    case TRACKER_EINVAL:    return "Invalid argument or URL";
    case TRACKER_ENOMEM:    return "Out of memory";
    case TRACKER_ETIMEDOUT: return "Timed out";
    case TRACKER_ENOTSUP:   return "Unsupported tracker protocol";
    case TRACKER_EFAILURE:  return "Tracker returned a failure";
    case TRACKER_EPROTO:    return "Invalid tracker response";
    case TRACKER_ENOTINIT:  return "Tracker module not initialised";
    case TRACKER_ETLS:      return "TLS error";
    default:                return "Unknown status";
    }
}
