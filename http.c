/*
 * http.c - Minimal asynchronous HTTP/HTTPS GET client using libuv.
 *
 * A request moves through:
 *
 *   resolve (uv_getaddrinfo)
 *   connect (uv_tcp, trying each address in turn)
 *   HTTP:  write the request -> read until the server closes
 *   HTTPS: TLS handshake -> write the request -> read until close_notify or
 *          close, with all bytes passing through OpenSSL memory BIOs
 *   split the reply into status line, headers and body -> callback
 *
 * Lifetime: libuv handles must be closed with uv_close() and libuv requests
 * must complete before their memory is freed. 'pending' counts every libuv
 * callback still to come. A request is freed when it is detached (its
 * callback has returned, or it was cancelled) and pending is zero.
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "http.h"

#define HTTP_STATE_MAGIC        0x48545450u     /* "HTTP" */
#define HTTP_MAX_ADDRESSES      8u
#define HTTP_READ_CHUNK         16384u
#define HTTP_IO_BUFFER          16384u          /* raw TLS records in */
#define HTTP_USER_AGENT_LENGTH  128u

struct httpRequest {
    httpCallback            callback;
    void                   *privateArg;
    int32_t                 status;
    uint8_t                 finished;       /* status final; handles closing */
    uint8_t                 detached;       /* callback done, or cancelled   */
    uint8_t                 inCallback;
    uint32_t                pending;        /* libuv callbacks still to come */
    uint32_t                timeoutSeconds;
    uint64_t                maxResponse;

    uint8_t                 secure;         /* https */
    char                   *host;
    char                   *requestText;
    uint64_t                requestLength;

    struct sockaddr_storage addresses[HTTP_MAX_ADDRESSES];
    uint32_t                addressCount;
    uint32_t                addressIndex;
    const char             *lastError;      /* static text from uv_strerror */

    uv_getaddrinfo_t        resolver;
    uv_timer_t              timer;
    uv_tcp_t                tcp;
    uv_connect_t            connectRequest;
    uv_write_t              writeRequest;
    uint8_t                 resolving;
    uint8_t                 timerOpen;
    uint8_t                 tcpOpen;
    uint8_t                 tcpRetry;       /* reconnect once tcp has closed */

    uint8_t                *response;
    uint64_t                responseLength;
    uint64_t                responseCapacity;
    uint32_t                statusCode;
    uint64_t                headersLength;

    SSL                    *ssl;
    uint8_t                 tlsHandshakeDone;
    uint8_t                 tlsRequestSent;
    uint8_t                 ioBuffer[HTTP_IO_BUFFER];

    char                    errorMessage[HTTP_ERROR_MESSAGE_LENGTH];
    TAILQ_ENTRY(httpRequest) entries;
};
TAILQ_HEAD(httpRequestList, httpRequest);

/* One chunk of TLS output on its way to the socket. */
typedef struct httpTlsWrite {
    uv_write_t   write;
    httpRequest *request;
    uint8_t      data[];
} httpTlsWrite;

static struct {
    uv_loop_t             *loop;
    parserContext         *context;
    SSL_CTX               *tls;
    char                   userAgent[HTTP_USER_AGENT_LENGTH];
    uint32_t               users;           /* httpInit() calls outstanding */
    uint32_t               magic;
    struct httpRequestList requests;
} httpState;

static void tcpConnectNext(httpRequest *request);

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static void *allocate(uint64_t size)
{
    void *memory;

    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    memory = httpState.context->allocator.realloc(NULL, (size_t)size);
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
    return httpState.context->allocator.realloc(memory, (size_t)size);
}

static void release(void *memory)
{
    if (memory != NULL) {
        httpState.context->allocator.free(memory);
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

static void setError(httpRequest *request, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(request->errorMessage, sizeof(request->errorMessage), format, arguments);
    va_end(arguments);
}

static uint8_t schemeIs(const httpUrl *url, const char *scheme)
{
    size_t   length = strlen(scheme);
    uint32_t i;

    if (url->schemeLength != length) {
        return 0;
    }
    for (i = 0; i < url->schemeLength; i++) {
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

/* ======================================================================== */
/* Request lifetime                                                         */
/* ======================================================================== */

static void freeRequest(httpRequest *request)
{
    release(request->host);
    release(request->requestText);
    release(request->response);
    if (request->ssl != NULL) {
        SSL_free(request->ssl);                     /* also frees its BIOs */
    }
    release(request);
}

static void maybeFree(httpRequest *request)
{
    if (request->detached && !request->inCallback && request->pending == 0) {
        freeRequest(request);
    }
}

static void onHandleClosed(uv_handle_t *handle)
{
    httpRequest *request = handle->data;

    if (handle == (uv_handle_t *)&request->tcp) {
        request->tcpOpen = 0;
        if (request->tcpRetry) {
            request->tcpRetry = 0;
            if (!request->finished) {
                request->pending--;
                tcpConnectNext(request);            /* try the next address */
                return;
            }
        }
    } else if (handle == (uv_handle_t *)&request->timer) {
        request->timerOpen = 0;
    }
    request->pending--;
    maybeFree(request);
}

/* Stop all network activity. Callbacks still to come will see 'finished'. */
static void closeAll(httpRequest *request)
{
    if (request->resolving) {
        uv_cancel((uv_req_t *)&request->resolver); /* may fail if running */
    }
    if (request->timerOpen && !uv_is_closing((uv_handle_t *)&request->timer)) {
        uv_timer_stop(&request->timer);
        uv_close((uv_handle_t *)&request->timer, onHandleClosed);
    }
    if (request->tcpOpen && !uv_is_closing((uv_handle_t *)&request->tcp)) {
        uv_close((uv_handle_t *)&request->tcp, onHandleClosed);
    }
}

static void detach(httpRequest *request)
{
    if (!request->detached) {
        request->detached = 1;
        TAILQ_REMOVE(&httpState.requests, request, entries);
    }
}

/* Record the outcome, close the network handles, and run the callback. */
static void finish(httpRequest *request, int32_t status)
{
    httpResponse response;

    if (request->finished) {
        return;
    }
    request->finished = 1;
    request->status = status;
    if (status != HTTP_OK && request->errorMessage[0] == '\0') {
        setError(request, "%s", httpStatusString(status));
    }
    closeAll(request);

    if (!request->detached && request->callback != NULL) {
        memset(&response, 0, sizeof(response));
        response.status = status;
        response.statusCode = request->statusCode;
        if (request->statusCode != 0) {
            response.headers = request->response;
            response.headersLength = request->headersLength;
            response.body = request->response + request->headersLength;
            response.bodyLength = request->responseLength - request->headersLength;
        }
        response.errorMessage = (status == HTTP_OK) ? NULL : request->errorMessage;

        /* inCallback stops httpCancel()/httpShutdown() from freeing the
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
    httpRequest *request = timer->data;

    setError(request, "no complete reply within %" PRIu32 " seconds", request->timeoutSeconds);
    finish(request, HTTP_ETIMEDOUT);
}

/* ======================================================================== */
/* Reply handling                                                           */
/* ======================================================================== */

/* Split the reply into status line + headers and body, then finish. */
static void handleReply(httpRequest *request)
{
    const uint8_t *data = request->response;
    uint64_t       length = request->responseLength;
    uint64_t       i;

    for (i = 0; i + 4u <= length; i++) {
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) {
            request->headersLength = i + 4u;
            break;
        }
    }
    if (request->headersLength == 0 || length < 12u || memcmp(data, "HTTP/1.", 7) != 0 ||
        data[8] != ' ' ||
        data[9] < '0' || data[9] > '9' ||
        data[10] < '0' || data[10] > '9' ||
        data[11] < '0' || data[11] > '9') {
        request->headersLength = 0;
        setError(request, "malformed HTTP reply");
        finish(request, HTTP_EPROTO);
        return;
    }
    request->statusCode = (uint32_t)(data[9] - '0') * 100u +
                          (uint32_t)(data[10] - '0') * 10u + (uint32_t)(data[11] - '0');
    finish(request, HTTP_OK);
}

/* Make room at the end of the reply buffer. Returns bytes free, 0 at the limit. */
static uint64_t growResponse(httpRequest *request)
{
    uint64_t capacity;
    uint8_t *data;

    if (request->responseLength >= request->maxResponse) {
        return 0;
    }
    if (request->responseCapacity - request->responseLength < HTTP_READ_CHUNK) {
        capacity = request->responseLength + HTTP_READ_CHUNK;
        if (capacity > request->maxResponse) {
            capacity = request->maxResponse;
        }
        data = resize(request->response, capacity);
        if (data == NULL) {
            return 0;
        }
        request->response = data;
        request->responseCapacity = capacity;
    }
    return request->responseCapacity - request->responseLength;
}

static void failTooLarge(httpRequest *request)
{
    setError(request, "reply too large (limit %" PRIu64 " bytes) or out of memory",
             request->maxResponse);
    finish(request, HTTP_EPROTO);
}

/* ======================================================================== */
/* Plain HTTP                                                               */
/* ======================================================================== */

static void onPlainAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    httpRequest *request = handle->data;
    uint64_t     space = growResponse(request);

    (void)suggested;
    /* A zero-length buffer makes libuv report UV_ENOBUFS to onPlainRead(). */
    buffer->base = (space > 0) ? (char *)request->response + request->responseLength : NULL;
    buffer->len = (size_t)space;
}

static void onPlainRead(uv_stream_t *stream, ssize_t count, const uv_buf_t *buffer)
{
    httpRequest *request = stream->data;

    (void)buffer;
    if (request->finished) {
        return;
    }
    if (count > 0) {
        request->responseLength += (uint64_t)count;
    } else if (count == UV_EOF) {
        handleReply(request);
    } else if (count == UV_ENOBUFS) {
        failTooLarge(request);
    } else if (count < 0) {
        setError(request, "receive failed: %s", uv_strerror((int)count));
        finish(request, HTTP_ERROR);
    }
}

static void onPlainWrite(uv_write_t *write, int status)
{
    httpRequest *request = write->data;

    request->pending--;
    if (request->finished) {
        maybeFree(request);
        return;
    }
    if (status < 0) {
        setError(request, "send failed: %s", uv_strerror(status));
        finish(request, HTTP_ERROR);
    }
}

static void plainStart(httpRequest *request)
{
    uv_buf_t buffer = uv_buf_init(request->requestText, (unsigned int)request->requestLength);
    int      result;

    request->writeRequest.data = request;
    result = uv_write(&request->writeRequest, (uv_stream_t *)&request->tcp,
                      &buffer, 1, onPlainWrite);
    if (result < 0) {
        setError(request, "send failed: %s", uv_strerror(result));
        finish(request, HTTP_ERROR);
        return;
    }
    request->pending++;

    result = uv_read_start((uv_stream_t *)&request->tcp, onPlainAlloc, onPlainRead);
    if (result < 0) {
        setError(request, "receive failed: %s", uv_strerror(result));
        finish(request, HTTP_ERROR);
    }
}

/* ======================================================================== */
/* HTTPS: TLS over the TCP stream                                           */
/* ======================================================================== */

/* Describe a TLS failure as precisely as OpenSSL allows. */
static void tlsFail(httpRequest *request, const char *stage)
{
    char          reason[120];
    unsigned long error = ERR_get_error();
    long          verify = (request->ssl != NULL) ? SSL_get_verify_result(request->ssl)
                                                  : X509_V_OK;

    if (verify != X509_V_OK) {
        setError(request, "TLS certificate rejected: %s", X509_verify_cert_error_string(verify));
    } else if (error != 0) {
        ERR_error_string_n(error, reason, sizeof(reason));
        setError(request, "TLS %s failed: %s", stage, reason);
    } else {
        setError(request, "TLS %s failed", stage);
    }
    ERR_clear_error();
    finish(request, HTTP_ETLS);
}

static void onTlsWrite(uv_write_t *write, int status)
{
    httpTlsWrite *chunk = write->data;
    httpRequest  *request = chunk->request;

    release(chunk);
    request->pending--;
    if (request->finished) {
        maybeFree(request);
        return;
    }
    if (status < 0) {
        setError(request, "send failed: %s", uv_strerror(status));
        finish(request, HTTP_ERROR);
    }
}

/* Send whatever OpenSSL has queued in its output BIO. */
static void tlsFlush(httpRequest *request)
{
    BIO          *output = SSL_get_wbio(request->ssl);
    httpTlsWrite *chunk;
    uv_buf_t      buffer;
    size_t        waiting;
    int           count;
    int           result;

    while ((waiting = BIO_ctrl_pending(output)) > 0) {
        if (waiting > HTTP_IO_BUFFER) {
            waiting = HTTP_IO_BUFFER;
        }
        chunk = allocate(sizeof(*chunk) + waiting);
        if (chunk == NULL) {
            finish(request, HTTP_ENOMEM);
            return;
        }
        count = BIO_read(output, chunk->data, (int)waiting);
        if (count <= 0) {
            release(chunk);
            tlsFail(request, "write");
            return;
        }
        chunk->request = request;
        chunk->write.data = chunk;
        buffer = uv_buf_init((char *)chunk->data, (unsigned int)count);
        result = uv_write(&chunk->write, (uv_stream_t *)&request->tcp, &buffer, 1, onTlsWrite);
        if (result < 0) {
            release(chunk);
            setError(request, "send failed: %s", uv_strerror(result));
            finish(request, HTTP_ERROR);
            return;
        }
        request->pending++;
    }
}

/*
 * Move the TLS session forward after new input: finish the handshake, send
 * the request once, then decrypt any reply data available.
 */
static void tlsAdvance(httpRequest *request)
{
    uint64_t space;
    int      result;
    int      error;

    if (!request->tlsHandshakeDone) {
        result = SSL_do_handshake(request->ssl);
        if (result != 1) {
            error = SSL_get_error(request->ssl, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                tlsFlush(request);                  /* send our handshake data */
                return;
            }
            tlsFail(request, "handshake");
            return;
        }
        request->tlsHandshakeDone = 1;
    }

    if (!request->tlsRequestSent) {
        /* A memory BIO accepts everything, so this completes in one call. */
        result = SSL_write(request->ssl, request->requestText, (int)request->requestLength);
        if (result <= 0) {
            tlsFail(request, "write");
            return;
        }
        request->tlsRequestSent = 1;
    }

    for (;;) {
        space = growResponse(request);
        if (space == 0) {
            failTooLarge(request);
            return;
        }
        if (space > INT32_MAX) {
            space = INT32_MAX;
        }
        result = SSL_read(request->ssl, request->response + request->responseLength, (int)space);
        if (result > 0) {
            request->responseLength += (uint64_t)result;
            continue;
        }
        error = SSL_get_error(request->ssl, result);
        if (error == SSL_ERROR_WANT_READ) {
            break;                                  /* need more from the socket */
        }
        if (error == SSL_ERROR_ZERO_RETURN) {       /* server sent close_notify */
            handleReply(request);
            return;
        }
        tlsFail(request, "read");
        return;
    }
    tlsFlush(request);
}

static void onTlsAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    httpRequest *request = handle->data;

    (void)suggested;
    buffer->base = (char *)request->ioBuffer;
    buffer->len = sizeof(request->ioBuffer);
}

static void onTlsRead(uv_stream_t *stream, ssize_t count, const uv_buf_t *buffer)
{
    httpRequest *request = stream->data;

    if (request->finished) {
        return;
    }
    if (count > 0) {
        if (BIO_write(SSL_get_rbio(request->ssl), buffer->base, (int)count) != (int)count) {
            finish(request, HTTP_ENOMEM);
            return;
        }
        tlsAdvance(request);
    } else if (count == UV_EOF) {
        /*
         * Many servers close without a TLS close_notify. Treat the close as
         * the end of the reply, as for plain HTTP/1.0; callers that parse
         * the body still detect a reply that was cut short.
         */
        if (request->tlsRequestSent) {
            handleReply(request);
        } else {
            setError(request, "connection closed during TLS handshake");
            finish(request, HTTP_ETLS);
        }
    } else if (count < 0) {
        setError(request, "receive failed: %s", uv_strerror((int)count));
        finish(request, HTTP_ERROR);
    }
}

static void tlsStart(httpRequest *request)
{
    uint8_t address[16];
    BIO    *input = BIO_new(BIO_s_mem());
    BIO    *output = BIO_new(BIO_s_mem());
    int     result;

    request->ssl = SSL_new(httpState.tls);
    if (request->ssl == NULL || input == NULL || output == NULL) {
        BIO_free(input);
        BIO_free(output);
        tlsFail(request, "setup");
        return;
    }
    SSL_set_bio(request->ssl, input, output);       /* the SSL now owns both */
    SSL_set_connect_state(request->ssl);

    /* The certificate must name this host: an IP address, or a DNS name
     * (which is also sent as SNI). */
    if (uv_inet_pton(AF_INET, request->host, address) == 0 ||
        uv_inet_pton(AF_INET6, request->host, address) == 0) {
        result = X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(request->ssl), request->host);
    } else {
        result = (SSL_set_tlsext_host_name(request->ssl, request->host) == 1 &&
                  SSL_set1_host(request->ssl, request->host) == 1);
    }
    if (result != 1) {
        tlsFail(request, "setup");
        return;
    }

    result = uv_read_start((uv_stream_t *)&request->tcp, onTlsAlloc, onTlsRead);
    if (result < 0) {
        setError(request, "receive failed: %s", uv_strerror(result));
        finish(request, HTTP_ERROR);
        return;
    }
    tlsAdvance(request);                            /* sends the ClientHello */
}

/* ======================================================================== */
/* Connecting                                                               */
/* ======================================================================== */

static void onTcpConnect(uv_connect_t *connect, int status)
{
    httpRequest *request = connect->data;

    request->pending--;
    if (request->finished) {
        maybeFree(request);
        return;
    }
    if (status < 0) {
        /* Close this socket; onHandleClosed() then tries the next address. */
        request->lastError = uv_strerror(status);
        request->addressIndex++;
        request->tcpRetry = 1;
        uv_close((uv_handle_t *)&request->tcp, onHandleClosed);
        return;
    }
    if (request->secure) {
        tlsStart(request);
    } else {
        plainStart(request);
    }
}

static void tcpConnectNext(httpRequest *request)
{
    int result;

    if (request->addressIndex >= request->addressCount) {
        setError(request, "could not connect to %s: %s", request->host,
                 (request->lastError != NULL) ? request->lastError : "no address");
        finish(request, HTTP_ERROR);
        return;
    }

    result = uv_tcp_init(httpState.loop, &request->tcp);
    if (result < 0) {
        setError(request, "socket failed: %s", uv_strerror(result));
        finish(request, HTTP_ERROR);
        return;
    }
    request->tcp.data = request;
    request->tcpOpen = 1;
    request->pending++;

    request->connectRequest.data = request;
    result = uv_tcp_connect(&request->connectRequest, &request->tcp,
                            (const struct sockaddr *)&request->addresses[request->addressIndex],
                            onTcpConnect);
    if (result < 0) {
        request->lastError = uv_strerror(result);
        request->addressIndex++;
        request->tcpRetry = 1;
        uv_close((uv_handle_t *)&request->tcp, onHandleClosed);
        return;
    }
    request->pending++;
}

static void onResolved(uv_getaddrinfo_t *resolver, int status, struct addrinfo *addresses)
{
    httpRequest     *request = resolver->data;
    struct addrinfo *entry;

    request->resolving = 0;
    request->pending--;

    if (request->finished || status < 0) {
        if (addresses != NULL) {
            uv_freeaddrinfo(addresses);
        }
        if (request->finished) {
            maybeFree(request);
        } else {
            setError(request, "DNS lookup for %s failed: %s", request->host, uv_strerror(status));
            finish(request, HTTP_ERROR);
        }
        return;
    }

    for (entry = addresses; entry != NULL && request->addressCount < HTTP_MAX_ADDRESSES;
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
        finish(request, HTTP_ERROR);
        return;
    }
    tcpConnectNext(request);
}

/* GET <path> HTTP/1.0 with the headers we always send. */
static int32_t buildRequestText(httpRequest *request, const httpUrl *url)
{
    const char *slash = (url->pathLength == 0 || url->path[0] != '/') ? "/" : "";
    uint64_t    size = (uint64_t)url->pathLength + url->authorityLength +
                       strlen(httpState.userAgent) + 128u;
    int         written;

    request->requestText = allocate(size);
    if (request->requestText == NULL) {
        return HTTP_ENOMEM;
    }
    written = snprintf(request->requestText, (size_t)size,
                       "GET %s%.*s HTTP/1.0\r\n"
                       "Host: %.*s\r\n"
                       "User-Agent: %s\r\n"
                       "Accept: */*\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       slash, (int)url->pathLength, url->path,
                       (int)url->authorityLength, url->authority,
                       httpState.userAgent);
    if (written < 0 || (uint64_t)written >= size) {
        return HTTP_EINVAL;
    }
    request->requestLength = (uint64_t)written;
    return HTTP_OK;
}

/* ======================================================================== */
/* Trusted certificates                                                     */
/* ======================================================================== */

/*
 * Load the system's CA bundle into memory, so that verifying a server never
 * has to read certificate files during a handshake (on the loop thread).
 * Order: $SSL_CERT_FILE, OpenSSL's built-in default file, then the bundle
 * locations used by the major Linux distributions. If none exists, fall
 * back to OpenSSL's default paths, which may look up certificates on disk
 * during handshakes.
 */
static int loadSystemCertificates(SSL_CTX *tls)
{
    static const char *const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                   /* Debian, Ubuntu, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",                     /* Fedora, RHEL       */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",    /* RHEL 7+            */
        "/etc/ssl/ca-bundle.pem",                               /* openSUSE           */
        "/etc/ssl/cert.pem",                                    /* Alpine, macOS      */
        NULL
    };
    const char *environment = getenv(X509_get_default_cert_file_env());
    const char *defaultFile = X509_get_default_cert_file();
    uint32_t    i;

    if (environment != NULL && environment[0] != '\0') {
        return SSL_CTX_load_verify_locations(tls, environment, NULL) == 1;
    }
    if (SSL_CTX_load_verify_locations(tls, defaultFile, NULL) == 1) {
        return 1;
    }
    for (i = 0; bundles[i] != NULL; i++) {
        if (SSL_CTX_load_verify_locations(tls, bundles[i], NULL) == 1) {
            return 1;
        }
    }
    ERR_clear_error();                              /* failures from the probes */
    return SSL_CTX_set_default_verify_paths(tls) == 1;
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t httpUrlParse(const char *url, httpUrl *parts)
{
    const char *separator;
    const char *authority;
    const char *authorityEnd;
    const char *hostEnd;
    const char *portStart = NULL;
    const char *colon;
    size_t      length;
    size_t      i;
    uint32_t    port = 0;

    if (url == NULL || parts == NULL) {
        return HTTP_EINVAL;
    }
    memset(parts, 0, sizeof(*parts));
    length = strlen(url);
    if (length == 0 || length > HTTP_MAX_URL_LENGTH) {
        return HTTP_EINVAL;
    }
    /* Printable ASCII only: stops CR/LF being smuggled into a request. */
    for (i = 0; i < length; i++) {
        if ((unsigned char)url[i] <= 0x20u || (unsigned char)url[i] >= 0x7Fu) {
            return HTTP_EINVAL;
        }
    }

    separator = strstr(url, "://");
    if (separator == NULL || separator == url) {
        return HTTP_EINVAL;
    }
    for (i = 0; url + i < separator; i++) {
        char c = url[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')))) {
            return HTTP_EINVAL;
        }
    }
    parts->scheme = url;
    parts->schemeLength = (uint32_t)(separator - url);

    authority = separator + 3;
    authorityEnd = authority + strcspn(authority, "/?#");
    if (authorityEnd == authority ||
        memchr(authority, '@', (size_t)(authorityEnd - authority)) != NULL) {
        return HTTP_EINVAL;
    }
    parts->authority = authority;
    parts->authorityLength = (uint32_t)(authorityEnd - authority);

    if (*authority == '[') {                        /* [IPv6]:port */
        parts->host = authority + 1;
        hostEnd = memchr(parts->host, ']', (size_t)(authorityEnd - parts->host));
        if (hostEnd == NULL) {
            return HTTP_EINVAL;
        }
        if (hostEnd + 1 < authorityEnd) {
            if (hostEnd[1] != ':') {
                return HTTP_EINVAL;
            }
            portStart = hostEnd + 2;
        }
    } else {
        parts->host = authority;
        colon = memchr(authority, ':', (size_t)(authorityEnd - authority));
        hostEnd = (colon != NULL) ? colon : authorityEnd;
        if (colon != NULL) {
            portStart = colon + 1;
        }
    }
    if (hostEnd == parts->host) {
        return HTTP_EINVAL;
    }
    parts->hostLength = (uint32_t)(hostEnd - parts->host);

    if (portStart != NULL) {
        if (portStart == authorityEnd) {
            return HTTP_EINVAL;
        }
        for (; portStart < authorityEnd; portStart++) {
            if (*portStart < '0' || *portStart > '9') {
                return HTTP_EINVAL;
            }
            port = port * 10u + (uint32_t)(*portStart - '0');
            if (port > 65535u) {
                return HTTP_EINVAL;
            }
        }
        if (port == 0) {
            return HTTP_EINVAL;
        }
    } else if (schemeIs(parts, "http")) {
        port = 80;
    } else if (schemeIs(parts, "https")) {
        port = 443;
    }
    parts->port = (uint16_t)port;

    parts->path = authorityEnd;
    parts->pathLength = (uint32_t)strcspn(authorityEnd, "#");
    return HTTP_OK;
}

int32_t httpInit(uv_loop_t *loop, parserContext *context, const httpConfig *config)
{
    const char *userAgent;

    if (loop == NULL || context == NULL || config == NULL ||
        context->magic != PARSER_CONTEXT_MAGIC) {
        return HTTP_EINVAL;
    }
    if (httpState.magic == HTTP_STATE_MAGIC) {
        if (loop != httpState.loop || context != httpState.context) {
            return HTTP_EINVAL;
        }
        httpState.users++;
        return HTTP_OK;
    }

    userAgent = (config->userAgent != NULL) ? config->userAgent : HTTP_DEFAULT_USER_AGENT;
    if (strlen(userAgent) >= sizeof(httpState.userAgent) ||
        strpbrk(userAgent, "\r\n") != NULL) {
        return HTTP_EINVAL;
    }

    /* Shared TLS settings: TLS 1.2 or later, certificates must verify. */
    httpState.tls = SSL_CTX_new(TLS_client_method());
    if (httpState.tls == NULL) {
        ERR_clear_error();
        return HTTP_ETLS;
    }
    SSL_CTX_set_min_proto_version(httpState.tls, TLS1_2_VERSION);
    SSL_CTX_set_verify(httpState.tls, SSL_VERIFY_PEER, NULL);
    if ((config->caFile != NULL)
            ? SSL_CTX_load_verify_locations(httpState.tls, config->caFile, NULL) != 1
            : !loadSystemCertificates(httpState.tls)) {
        SSL_CTX_free(httpState.tls);
        httpState.tls = NULL;
        ERR_clear_error();
        return HTTP_ETLS;
    }
    ERR_clear_error();

    snprintf(httpState.userAgent, sizeof(httpState.userAgent), "%s", userAgent);
    httpState.loop = loop;
    httpState.context = context;
    httpState.users = 1;
    TAILQ_INIT(&httpState.requests);
    httpState.magic = HTTP_STATE_MAGIC;
    return HTTP_OK;
}

int32_t httpGet(const char *url, uint32_t timeoutSeconds, uint64_t maxResponse,
                httpCallback callback, void *privateArg, httpRequest **handle)
{
    httpRequest    *request;
    httpUrl         parts;
    struct addrinfo hints;
    char            portText[8];
    int32_t         result;
    int             status;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (httpState.magic != HTTP_STATE_MAGIC) {
        return HTTP_ENOTINIT;
    }
    result = httpUrlParse(url, &parts);
    if (result != HTTP_OK) {
        return result;
    }
    if (!schemeIs(&parts, "http") && !schemeIs(&parts, "https")) {
        return HTTP_ENOTSUP;
    }

    request = allocate(sizeof(*request));
    if (request == NULL) {
        return HTTP_ENOMEM;
    }
    request->callback = callback;
    request->privateArg = privateArg;
    request->timeoutSeconds = timeoutSeconds;
    request->maxResponse = (maxResponse != 0) ? maxResponse : HTTP_DEFAULT_MAX_RESPONSE;
    request->secure = schemeIs(&parts, "https");
    request->host = copyBytes(parts.host, parts.hostLength);
    result = (request->host != NULL) ? buildRequestText(request, &parts) : HTTP_ENOMEM;
    if (result != HTTP_OK) {
        freeRequest(request);                       /* no handles open yet */
        return result;
    }

    /* From here on, failures must close what has been opened. */
    request->detached = 1;                          /* not listed until started */
    if (timeoutSeconds > 0) {
        uv_timer_init(httpState.loop, &request->timer);
        request->timer.data = request;
        request->timerOpen = 1;
        request->pending++;
        uv_timer_start(&request->timer, onTimeout, (uint64_t)timeoutSeconds * 1000u, 0);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portText, sizeof(portText), "%u", (unsigned)parts.port);

    request->resolver.data = request;
    status = uv_getaddrinfo(httpState.loop, &request->resolver, onResolved,
                            request->host, portText, &hints);
    if (status < 0) {
        request->finished = 1;
        closeAll(request);
        maybeFree(request);                         /* now, or when the timer closes */
        return HTTP_ERROR;
    }
    request->resolving = 1;
    request->pending++;

    request->detached = 0;
    TAILQ_INSERT_TAIL(&httpState.requests, request, entries);
    if (handle != NULL) {
        *handle = request;
    }
    return HTTP_OK;
}

void httpCancel(httpRequest *request)
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

void httpShutdown(void)
{
    httpRequest *request;

    if (httpState.magic != HTTP_STATE_MAGIC || --httpState.users > 0) {
        return;
    }
    while ((request = TAILQ_FIRST(&httpState.requests)) != NULL) {
        httpCancel(request);
    }
    /* loop and context stay set: closing requests still need the allocator.
     * The TLS context is reference counted, so open sessions keep it alive. */
    SSL_CTX_free(httpState.tls);
    httpState.tls = NULL;
    httpState.magic = 0;
}

const char *httpStatusString(int32_t status)
{
    switch (status) {
    case HTTP_OK:        return "OK";
    case HTTP_ERROR:     return "Network error";
    case HTTP_EINVAL:    return "Invalid argument or URL";
    case HTTP_ENOMEM:    return "Out of memory";
    case HTTP_ETIMEDOUT: return "Timed out";
    case HTTP_ENOTSUP:   return "Unsupported URL scheme";
    case HTTP_EPROTO:    return "Invalid HTTP reply";
    case HTTP_ENOTINIT:  return "HTTP module not initialised";
    case HTTP_ETLS:      return "TLS error";
    default:             return "Unknown status";
    }
}
