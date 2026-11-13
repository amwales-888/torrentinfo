/*
 * tracker.c - Asynchronous BitTorrent tracker announces using libuv.
 *
 * Each announce is a trackerRequest that moves through:
 *
 *   HTTP/HTTPS: build the announce URL -> httpGet() (http.c) -> parse the
 *               bencoded body
 *   UDP:        udpAnnounce() (udp.c) -> copy the compact peers
 *
 * A single timeout timer covers the whole announce, for every protocol.
 *
 * Lifetime: libuv handles must be closed with uv_close() and requests must
 * complete before their memory is freed. (HTTP and UDP requests manage
 * their own handles inside http.c and udp.c; we only cancel them.) The only
 * handle here is the timeout timer. 'pending' counts every libuv
 * callback still to come (open handles plus in-flight requests). A request
 * is freed only when it has been closed by the user (detached) and pending
 * has reached zero. When an announce finishes, its handles are closed at
 * once, but its results stay until trackerClose().
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "http.h"
#include "tracker.h"
#include "udp.h"

#define TRACKER_STATE_MAGIC         0x5452434Bu     /* "TRCK" */
#define TRACKER_PROTOCOL_HTTP       1u              /* http and https */
#define TRACKER_PROTOCOL_UDP        2u
#define TRACKER_CANCELLED           (-100)          /* internal only         */
/*
 * Bytes left to download, as trackerConnect() reports it (trackerAnnounce()
 * reports real figures): a small non-zero amount marks us as a leecher, so
 * trackers include seeders in the peer list.
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
    uint8_t                 infoHash[PARSER_SHA1_LENGTH];
    trackerAnnounceParams   params;         /* event and statistics          */

    /* The transport request in http.c or udp.c, or NULL when not running */
    httpRequest            *http;
    udpRequest             *udp;

    uv_timer_t              timeoutTimer;
    uint8_t                 timeoutOpen;

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

static void onTimerClosed(uv_handle_t *handle)
{
    trackerRequest *request = handle->data;

    request->timeoutOpen = 0;
    request->pending--;
    maybeFree(request);
}

/* Stop the transport and the timer. Callbacks still to come see 'finished'. */
static void closeAll(trackerRequest *request)
{
    if (request->http != NULL) {
        httpCancel(request->http);                  /* its callback will not run */
        request->http = NULL;
    }
    if (request->udp != NULL) {
        udpCancel(request->udp);                    /* its callback will not run */
        request->udp = NULL;
    }
    if (request->timeoutOpen && !uv_is_closing((uv_handle_t *)&request->timeoutTimer)) {
        uv_timer_stop(&request->timeoutTimer);
        uv_close((uv_handle_t *)&request->timeoutTimer, onTimerClosed);
    }
}

/*
 * Record the final status and tell the user. The timer closes
 * asynchronously, so 'pending' stays above zero and the request cannot be
 * freed during the callback, even if the callback calls trackerClose().
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

static int32_t fromUdpStatus(int32_t status)
{
    switch (status) {
    case UDP_OK:        return TRACKER_OK;
    case UDP_EINVAL:    return TRACKER_EINVAL;
    case UDP_ENOMEM:    return TRACKER_ENOMEM;
    case UDP_ETIMEDOUT: return TRACKER_ETIMEDOUT;
    case UDP_ENOTSUP:   return TRACKER_ENOTSUP;
    case UDP_EFAILURE:  return TRACKER_EFAILURE;
    case UDP_EPROTO:    return TRACKER_EPROTO;
    case UDP_ENOTINIT:  return TRACKER_ENOTINIT;
    default:            return TRACKER_ERROR;
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
    request->url = copyBytes(url, strlen(url));
    return (request->url != NULL) ? TRACKER_OK : TRACKER_ENOMEM;
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
                       "%.*s%cinfo_hash=%s&peer_id=%s&port=%u&uploaded=%" PRIu64
                       "&downloaded=%" PRIu64 "&left=%" PRIu64 "&compact=1&numwant=%" PRIu32
                       "&key=%08" PRIx32 "%s",
                       (int)baseLength, request->url,
                       (memchr(parts->path, '?', parts->pathLength) != NULL) ? '&' : '?',
                       infoHashText, peerIdText, (unsigned)trackerState.config.port,
                       request->params.uploaded, request->params.downloaded,
                       request->params.left,
                       (request->params.event == TRACKER_EVENT_STOPPED) ? 0u : numWant(),
                       trackerState.key,
                       (request->params.event == TRACKER_EVENT_STARTED)     ? "&event=started"
                       : (request->params.event == TRACKER_EVENT_COMPLETED) ? "&event=completed"
                       : (request->params.event == TRACKER_EVENT_STOPPED)   ? "&event=stopped"
                                                                            : "");
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
/* UDP (the protocol is in udp.c)                                           */
/* ======================================================================== */

static void onUdpDone(void *privateArg, const udpResponse *response)
{
    trackerRequest *request = privateArg;

    request->udp = NULL;                            /* udp.c frees it after this */
    if (response->failureReason != NULL &&
        replaceString(&request->failureReason, (const uint8_t *)response->failureReason,
                      strlen(response->failureReason)) != TRACKER_OK) {
        finish(request, TRACKER_ENOMEM);
        return;
    }
    if (response->status != UDP_OK) {
        setError(request, "%s", response->errorMessage);
        finish(request, fromUdpStatus(response->status));
        return;
    }
    request->interval = response->interval;
    request->leechers = (int64_t)response->leechers;
    request->seeders = (int64_t)response->seeders;
    finish(request, addCompactPeers(request, response->peers, response->peersLength,
                                    (response->peerFamily == UDP_FAMILY_IPV6)
                                        ? TRACKER_FAMILY_IPV6 : TRACKER_FAMILY_IPV4));
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
    httpConfiguration.binding = config->binding;
    result = httpInit(loop, context, &httpConfiguration);
    if (result != HTTP_OK) {
        return fromHttpStatus(result);
    }
    result = udpInit(loop, context, config->binding);
    if (result != UDP_OK) {
        httpShutdown();
        return fromUdpStatus(result);
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
    trackerAnnounceParams params;

    memset(&params, 0, sizeof(params));
    params.event = TRACKER_EVENT_STARTED;
    params.left = TRACKER_ANNOUNCE_LEFT;
    return trackerAnnounce(infoHash, trackerUrl, &params, uniqueId, timeoutSeconds, callback,
                           privateArg);
}

int32_t trackerAnnounce(const uint8_t infoHash[PARSER_SHA1_LENGTH], const char *trackerUrl,
                        const trackerAnnounceParams *announceParams, uint32_t *uniqueId,
                        uint32_t timeoutSeconds, trackerCallback callback, void *privateArg)
{
    trackerRequest   *request;
    httpUrl           parts;
    udpAnnounceParams params;
    char             *announceUrl;
    int32_t           result;

    if (trackerState.magic != TRACKER_STATE_MAGIC) {
        return TRACKER_ENOTINIT;
    }
    if (infoHash == NULL || trackerUrl == NULL || uniqueId == NULL || timeoutSeconds == 0 ||
        announceParams == NULL || announceParams->event > TRACKER_EVENT_STOPPED) {
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
    request->params = *announceParams;

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
    } else if (result == TRACKER_OK) {
        memset(&params, 0, sizeof(params));
        memcpy(params.infoHash, request->infoHash, PARSER_SHA1_LENGTH);
        memcpy(params.peerId, trackerState.config.peerId, TRACKER_PEER_ID_LENGTH);
        params.downloaded = request->params.downloaded;
        params.left = request->params.left;
        params.uploaded = request->params.uploaded;
        params.event = (request->params.event == TRACKER_EVENT_STARTED)     ? UDP_EVENT_STARTED
                     : (request->params.event == TRACKER_EVENT_COMPLETED)   ? UDP_EVENT_COMPLETED
                     : (request->params.event == TRACKER_EVENT_STOPPED)     ? UDP_EVENT_STOPPED
                                                                            : UDP_EVENT_NONE;
        params.key = trackerState.key;
        params.numWant = (request->params.event == TRACKER_EVENT_STOPPED)
                             ? 0 : (int32_t)numWant();
        params.port = trackerState.config.port;
        result = fromUdpStatus(udpAnnounce(trackerUrl, &params, 0, onUdpDone, request,
                                           &request->udp));
    }
    if (result != TRACKER_OK) {
        freeRequest(request);                       /* nothing is running yet */
        return result;
    }

    /* One timer covers the whole announce, whatever the transport. */
    uv_timer_init(trackerState.loop, &request->timeoutTimer);
    request->timeoutTimer.data = request;
    request->timeoutOpen = 1;
    request->pending++;
    uv_timer_start(&request->timeoutTimer, onTimeout, (uint64_t)timeoutSeconds * 1000u, 0);

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
    udpShutdown();
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
