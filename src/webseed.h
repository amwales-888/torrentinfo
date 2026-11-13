/*
 * webseed.h - Fetching torrent data from web seeds (BEP 19).
 *
 * A web seed is an HTTP or HTTPS server that holds the torrent's files.
 * webSeedFetch() reads any byte range of the torrent's data (the files
 * one after another, as for pieces) into a buffer, with one Range request
 * per file the range touches; BEP 47 padding files are zeros and never
 * fetched. URLs follow BEP 19:
 *
 *   single-file torrent:  the URL itself, or URL + name if it ends in '/'
 *   multi-file torrent:   URL [+ '/'] + name + '/' + path/inside/torrent
 *
 * with each name and path component percent-encoded. Redirects (301, 302,
 * 303, 307, 308) are followed, up to WEBSEED_MAX_REDIRECTS per request. A
 * server that ignores the Range header and sends the whole file (200) is
 * accepted if the file is small enough to hold; a wrong Content-Range or
 * length is an error.
 *
 * Uses the http module, which the caller must have started (httpInit()).
 * One fetch at a time per web seed. Memory comes from the parser context.
 */
#ifndef WEBSEED_H
#define WEBSEED_H

#include <stdint.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBSEED_OK              0
#define WEBSEED_ERROR           (-1)    /* network failure or timeout        */
#define WEBSEED_EINVAL          (-3)
#define WEBSEED_ENOMEM          (-4)
#define WEBSEED_EHTTP           (-5)    /* the server answered with an error  */
#define WEBSEED_EPROTO          (-6)    /* wrong length, range, or redirects  */
#define WEBSEED_EBUSY           (-7)    /* a fetch is already in progress     */

#define WEBSEED_MAX_REDIRECTS   5u
#define WEBSEED_MESSAGE_LENGTH  256u

typedef struct webSeed webSeed;

/* status: WEBSEED_OK when 'buffer' holds the whole range; otherwise
 * 'message' says what went wrong (with the URL). */
typedef void (*webSeedCallback)(void *privateArg, webSeed *seed, int32_t status,
                                const char *message);

/* torrent must outlive the web seed. */
int32_t webSeedBegin(parserContext *context, const parserTorrent *torrent, const char *url,
                     webSeed **seed);

/* Fetch torrent bytes [offset, offset + length) into buffer, which must stay
 * valid until the callback runs (or webSeedCancel()). */
int32_t webSeedFetch(webSeed *seed, uint64_t offset, uint32_t length, uint8_t *buffer,
                     uint32_t timeoutSeconds, webSeedCallback callback, void *privateArg);

/* Stop the fetch in progress, if any; its callback will not run. */
void webSeedCancel(webSeed *seed);

/* Cancel and free. */
void webSeedEnd(webSeed *seed);

const char *webSeedUrl(const webSeed *seed);

/* The URL a file is fetched from (for messages and tests); 0 if it fits. */
int32_t webSeedFileUrl(const webSeed *seed, const parserFile *file, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* WEBSEED_H */
