/*
 * http.h - Minimal asynchronous HTTP/HTTPS GET client using libuv.
 *
 * Enough HTTP for BitTorrent trackers and similar simple services:
 *   - GET requests over HTTP/1.0 (so replies are never chunked), reading
 *     the reply until the server closes the connection
 *   - HTTPS over TLS 1.2+ with OpenSSL, verifying the certificate chain
 *     and that the certificate names the host (sent as SNI too)
 *   - Each address from DNS is tried in turn until one connects
 *   - Optional timeout covering the whole request
 *   - Redirects are not followed; a 3xx reply is returned as it is
 *
 * libuv does all the socket I/O. For HTTPS, OpenSSL works only in memory
 * (memory BIOs): bytes from the socket are fed to it, and whatever it
 * produces is written to the socket.
 *
 * Memory comes from the allocator in the parserContext given to httpInit().
 * OpenSSL has its own allocator hook, CRYPTO_set_mem_functions().
 *
 * Usage:
 *     httpInit(loop, &context, &config);
 *     httpGet("https://example.com/path?q=1", 15, 0, onDone, me, &request);
 *     uv_run(loop, UV_RUN_DEFAULT);
 *
 *     static void onDone(void *privateArg, const httpResponse *response) {
 *         if (response->status == HTTP_OK && response->statusCode == 200) {
 *             ... response->body, response->bodyLength ...
 *         }
 *     }   // the request is freed after the callback returns
 *
 * Building: as for tracker.h. On Windows also link crypt32.lib (for the
 * system certificate store).
 */
#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <uv.h>

#include "net.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define HTTP_MAX_URL_LENGTH         4096u
#define HTTP_DEFAULT_MAX_RESPONSE   (1024u * 1024u)
#define HTTP_DEFAULT_USER_AGENT     "torrentinfo/0.1"
#define HTTP_ERROR_MESSAGE_LENGTH   160u

/* Status codes (the same values as the matching TRACKER_* codes) */
#define HTTP_OK                     0       /* A reply was received          */
#define HTTP_ERROR                  (-1)    /* Network error: see message    */
#define HTTP_EINVAL                 (-3)    /* Invalid argument or URL       */
#define HTTP_ENOMEM                 (-4)    /* Allocator returned NULL       */
#define HTTP_ETIMEDOUT              (-5)    /* No complete reply in time     */
#define HTTP_ENOTSUP                (-6)    /* Scheme is not http or https   */
#define HTTP_EPROTO                 (-8)    /* Malformed or too large reply  */
#define HTTP_ENOTINIT               (-9)    /* httpInit() not called         */
#define HTTP_ETLS                   (-10)   /* TLS or certificate failure    */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* Settings for the module. Copied by httpInit(); strings are only read there. */
typedef struct httpConfig {
    const char *caFile;     /* PEM file of trusted CAs, or NULL for the
                               system's trusted CAs ($SSL_CERT_FILE; else
                               the Windows certificate store, or the usual
                               Linux bundle locations). Either way they are
                               loaded into memory here, so handshakes never
                               read from disk.                             */
    const char *userAgent;  /* NULL for HTTP_DEFAULT_USER_AGENT; at most 127
                               characters                                  */
    const netBinding *binding;  /* interface to use (net.h; copied),
                                           or NULL for any               */
} httpConfig;

/*
 * The parts of a URL, as found by httpUrlParse(). Every pointer points into
 * the original string, which is not modified; lengths exclude any NUL.
 */
typedef struct httpUrl {
    const char *scheme;         /* "http", "https", "udp", ...              */
    uint32_t    schemeLength;
    const char *authority;      /* host[:port], as written                  */
    uint32_t    authorityLength;
    const char *host;           /* without [ ] for IPv6 literals            */
    uint32_t    hostLength;
    const char *path;           /* path and query, without #fragment; may be
                                   empty or start with '?'                  */
    uint32_t    pathLength;
    uint16_t    port;           /* explicit port, else 80 for http and 443
                                   for https, else 0                        */
} httpUrl;

/* What a finished request produced. Valid only during the callback. */
typedef struct httpResponse {
    int32_t         status;         /* HTTP_OK or an HTTP_E* code           */
    uint32_t        statusCode;     /* e.g. 200; 0 if no reply was parsed   */
    const uint8_t  *headers;        /* raw header block, including the status
                                       line, or NULL                        */
    uint64_t        headersLength;
    const uint8_t  *body;           /* or NULL                              */
    uint64_t        bodyLength;
    const char     *errorMessage;   /* NULL when status is HTTP_OK          */
} httpResponse;

/* Opaque request handle. */
typedef struct httpRequest httpRequest;

/* Called exactly once per request unless it is cancelled first. */
typedef void (*httpCallback)(void *privateArg, const httpResponse *response);

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Set up the module. Calls are counted, so several users (e.g. the tracker
 * module and the application) may each call httpInit() and httpShutdown();
 * the settings from the first call apply. Returns HTTP_ETLS if the TLS
 * context or CA certificates cannot be loaded, HTTP_EINVAL if a later call
 * passes a different loop or context.
 */
int32_t httpInit(uv_loop_t *loop, parserContext *context, const httpConfig *config);

/*
 * Start a GET request. On HTTP_OK the request is under way, *request (if
 * not NULL) receives its handle, and the callback will run once from the
 * libuv loop. Any other return value means nothing was started.
 *
 * timeoutSeconds: 0 for no timeout. maxResponse: limit on the whole reply
 * (headers and body) in bytes, 0 for HTTP_DEFAULT_MAX_RESPONSE.
 *
 * The handle is valid until the callback runs or httpCancel() is called;
 * do not use it after either.
 */
int32_t httpGet(const char *url, uint32_t timeoutSeconds, uint64_t maxResponse,
                httpCallback callback, void *privateArg, httpRequest **request);

/* Stop a request that is still in progress; its callback will not run. */
void httpCancel(httpRequest *request);

/*
 * Release one httpInit(). The last call cancels every request still in
 * progress (their callbacks do not run). Keep running the loop until
 * uv_run() returns so that all memory is released.
 */
void httpShutdown(void);

/*
 * Split a URL into its parts without allocating. Accepts any scheme;
 * rejects URLs with characters outside printable ASCII, user information
 * ("user@host"), an empty host, or an invalid port.
 */
int32_t httpUrlParse(const char *url, httpUrl *parts);

/* Human-readable description of an HTTP_* status. Never NULL. */
const char *httpStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_H */
