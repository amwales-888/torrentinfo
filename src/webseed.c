/*
 * webseed.c - Fetching torrent data from web seeds. See webseed.h.
 *
 * A fetch is a sequence of segments, one per file the range touches; each
 * segment is one HTTP request (plus any redirects). Padding files are
 * filled with zeros without asking the server.
 */
#include <stdio.h>
#include <string.h>

#include "http.h"
#include "webseed.h"

#define WEBSEED_WHOLE_FILE_LIMIT  (16u * 1024u * 1024u)   /* 200 replies we accept */
#define WEBSEED_HEADER_ROOM       (64u * 1024u)

struct webSeed {
    parserContext       *context;
    const parserTorrent *torrent;
    char                *url;               /* as given                        */
    char                *current;           /* the request's URL (redirected)  */
    httpRequest         *request;
    webSeedCallback      callback;
    void                *privateArg;
    uint8_t             *buffer;
    uint64_t             offset;            /* torrent offset of buffer[0]     */
    uint32_t             length;
    uint32_t             got;               /* bytes of the buffer filled      */
    const parserFile    *file;              /* the segment being fetched       */
    uint64_t             fileStart;         /* its first byte, in the file     */
    uint32_t             segment;           /* its length                      */
    uint32_t             redirects;
    uint32_t             timeoutSeconds;
    uint8_t              busy;
    char                 message[WEBSEED_MESSAGE_LENGTH];
};

static void *allocate(webSeed *seed, size_t size)
{
    void *memory = seed->context->allocator.realloc(NULL, size);

    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static char *copyText(parserContext *context, const char *text)
{
    size_t length = strlen(text);
    char  *copy = context->allocator.realloc(NULL, length + 1u);

    if (copy != NULL) {
        memcpy(copy, text, length + 1u);
    }
    return copy;
}

/* ------------------------------------------------------------------------ */
/* URLs                                                                     */
/* ------------------------------------------------------------------------ */

/* Append 'text' percent-encoded (all but unreserved characters). 0 if it fit. */
static int appendEncoded(char *out, size_t size, size_t *used, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";

    for (; *text != '\0'; text++) {
        unsigned char c = (unsigned char)*text;
        int           plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                              c == '~';

        if (*used + (plain ? 1u : 3u) >= size) {
            return -1;
        }
        if (plain) {
            out[(*used)++] = (char)c;
        } else {
            out[(*used)++] = '%';
            out[(*used)++] = hex[c >> 4];
            out[(*used)++] = hex[c & 15u];
        }
    }
    out[*used] = '\0';
    return 0;
}

static int appendText(char *out, size_t size, size_t *used, const char *text)
{
    size_t length = strlen(text);

    if (*used + length >= size) {
        return -1;
    }
    memcpy(out + *used, text, length + 1u);
    *used += length;
    return 0;
}

int32_t webSeedFileUrl(const webSeed *seed, const parserFile *file, char *out, size_t size)
{
    const parserPathComponent *component;
    size_t                     used = 0;
    size_t                     urlLength;

    if (seed == NULL || file == NULL || out == NULL || size == 0) {
        return WEBSEED_EINVAL;
    }
    urlLength = strlen(seed->url);
    if (appendText(out, size, &used, seed->url) != 0) {
        return WEBSEED_EINVAL;
    }
    if (!seed->torrent->isMultiFile) {
        /* A URL ending in '/' is a directory holding the file. */
        if (urlLength > 0 && seed->url[urlLength - 1u] == '/' &&
            appendEncoded(out, size, &used, seed->torrent->name) != 0) {
            return WEBSEED_EINVAL;
        }
        return WEBSEED_OK;
    }
    if ((urlLength == 0 || seed->url[urlLength - 1u] != '/') &&
        appendText(out, size, &used, "/") != 0) {
        return WEBSEED_EINVAL;
    }
    if (appendEncoded(out, size, &used, seed->torrent->name) != 0) {
        return WEBSEED_EINVAL;
    }
    TAILQ_FOREACH(component, &file->path, entries) {
        if (appendText(out, size, &used, "/") != 0 ||
            appendEncoded(out, size, &used, component->name) != 0) {
            return WEBSEED_EINVAL;
        }
    }
    return WEBSEED_OK;
}

/* Make 'location' (from a redirect) absolute, relative to seed->current. */
static int resolveLocation(webSeed *seed, const char *location, size_t locationLength,
                           char *out, size_t size)
{
    httpUrl parts;

    if (locationLength >= size) {
        return -1;
    }
    if ((locationLength > 7u && strncmp(location, "http://", 7u) == 0) ||
        (locationLength > 8u && strncmp(location, "https://", 8u) == 0)) {
        memcpy(out, location, locationLength);
        out[locationLength] = '\0';
        return 0;
    }
    if (httpUrlParse(seed->current, &parts) != HTTP_OK) {
        return -1;
    }
    if (locationLength >= 2u && location[0] == '/' && location[1] == '/') {
        return (snprintf(out, size, "%.*s:%.*s", (int)parts.schemeLength, parts.scheme,
                         (int)locationLength, location) < (int)size) ? 0 : -1;
    }
    if (locationLength >= 1u && location[0] == '/') {
        return (snprintf(out, size, "%.*s://%.*s%.*s", (int)parts.schemeLength, parts.scheme,
                         (int)parts.authorityLength, parts.authority, (int)locationLength,
                         location) < (int)size) ? 0 : -1;
    }
    {
        /* Relative to the current path's directory. */
        uint32_t directory = parts.pathLength;

        while (directory > 0 && parts.path[directory - 1u] != '/') {
            directory--;
        }
        return (snprintf(out, size, "%.*s://%.*s%s%.*s%.*s", (int)parts.schemeLength,
                         parts.scheme, (int)parts.authorityLength, parts.authority,
                         (directory == 0) ? "/" : "", (int)directory, parts.path,
                         (int)locationLength, location) < (int)size) ? 0 : -1;
    }
}

/* The value of header 'name' in the raw header block, or NULL. */
static const char *findHeader(const httpResponse *response, const char *name, size_t *length)
{
    const char *text = (const char *)response->headers;
    const char *end = text + response->headersLength;
    size_t      nameLength = strlen(name);

    while (text != NULL && text < end) {
        const char *lineEnd = memchr(text, '\n', (size_t)(end - text));
        const char *next = (lineEnd != NULL) ? lineEnd + 1 : end;
        size_t      i;

        if (lineEnd == NULL) {
            lineEnd = end;
        }
        if ((size_t)(lineEnd - text) > nameLength && text[nameLength] == ':') {
            for (i = 0; i < nameLength; i++) {
                char a = text[i];
                char b = name[i];

                if ((a >= 'A' && a <= 'Z' ? a + 32 : a) != (b >= 'A' && b <= 'Z' ? b + 32 : b)) {
                    break;
                }
            }
            if (i == nameLength) {
                const char *value = text + nameLength + 1;

                while (value < lineEnd && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                while (lineEnd > value && (lineEnd[-1] == '\r' || lineEnd[-1] == ' ')) {
                    lineEnd--;
                }
                *length = (size_t)(lineEnd - value);
                return value;
            }
        }
        text = next;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Fetching                                                                 */
/* ------------------------------------------------------------------------ */

static void fetchNext(webSeed *seed);

/* The fetch is over: tell the caller (who may start another). */
static void done(webSeed *seed, int32_t status)
{
    webSeedCallback callback = seed->callback;

    seed->busy = 0;
    seed->request = NULL;
    callback(seed->privateArg, seed, status, (status == WEBSEED_OK) ? NULL : seed->message);
}

static void failed(webSeed *seed, int32_t status, const char *what)
{
    snprintf(seed->message, sizeof(seed->message), "%s: %s", seed->current, what);
    done(seed, status);
}

static void onResponse(void *privateArg, const httpResponse *response);

static void sendRequest(webSeed *seed)
{
    char     range[96];
    uint64_t room = (seed->file->length < WEBSEED_WHOLE_FILE_LIMIT) ? seed->file->length
                                                                     : seed->segment;
    int32_t  status;

    if (room < seed->segment) {
        room = seed->segment;
    }
    snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
             (unsigned long long)seed->fileStart,
             (unsigned long long)(seed->fileStart + seed->segment - 1u));
    status = httpGetWithHeaders(seed->current, range, seed->timeoutSeconds,
                                room + WEBSEED_HEADER_ROOM, onResponse, seed, &seed->request);
    if (status != HTTP_OK) {
        seed->request = NULL;
        failed(seed, (status == HTTP_ENOMEM) ? WEBSEED_ENOMEM : WEBSEED_ERROR,
               httpStatusString(status));
    }
}

static void onResponse(void *privateArg, const httpResponse *response)
{
    webSeed       *seed = privateArg;
    const uint8_t *data = NULL;
    const char    *value;
    size_t         valueLength;
    char           what[160];

    seed->request = NULL;
    if (response->status != HTTP_OK) {
        failed(seed, WEBSEED_ERROR, (response->errorMessage != NULL)
                                        ? response->errorMessage
                                        : httpStatusString(response->status));
        return;
    }
    switch (response->statusCode) {
    case 301: case 302: case 303: case 307: case 308: {
        char location[HTTP_MAX_URL_LENGTH];

        value = findHeader(response, "Location", &valueLength);
        if (value == NULL || valueLength == 0) {
            failed(seed, WEBSEED_EPROTO, "redirect without a Location");
            return;
        }
        if (++seed->redirects > WEBSEED_MAX_REDIRECTS) {
            failed(seed, WEBSEED_EPROTO, "too many redirects");
            return;
        }
        if (resolveLocation(seed, value, valueLength, location, sizeof(location)) != 0) {
            failed(seed, WEBSEED_EPROTO, "redirect to an unusable address");
            return;
        }
        memcpy(seed->current, location, strlen(location) + 1u);
        sendRequest(seed);
        return;
    }
    case 206: {
        /* Partial content: it must be exactly the range we asked for. */
        value = findHeader(response, "Content-Range", &valueLength);
        if (value != NULL) {
            unsigned long long first = 0;

            if (valueLength < 7u || strncmp(value, "bytes ", 6u) != 0 ||
                sscanf(value + 6, "%llu", &first) != 1 || first != seed->fileStart) {
                failed(seed, WEBSEED_EPROTO, "sent a different range than asked for");
                return;
            }
        }
        if (response->bodyLength < seed->segment) {
            snprintf(what, sizeof(what), "sent %llu bytes instead of %u",
                     (unsigned long long)response->bodyLength, (unsigned)seed->segment);
            failed(seed, WEBSEED_EPROTO, what);
            return;
        }
        data = response->body;
        break;
    }
    case 200:
        /* The whole file: the server does not do ranges. */
        if (response->bodyLength != seed->file->length) {
            failed(seed, WEBSEED_EPROTO, (response->bodyLength < seed->file->length)
                                             ? "file on the server is too short"
                                             : "file on the server is a different size");
            return;
        }
        data = response->body + seed->fileStart;
        break;
    default:
        snprintf(what, sizeof(what), "HTTP %u", (unsigned)response->statusCode);
        failed(seed, WEBSEED_EHTTP, what);
        return;
    }
    memcpy(seed->buffer + seed->got, data, seed->segment);
    seed->got += seed->segment;
    fetchNext(seed);
}

/* Start the next segment, or finish. */
static void fetchNext(webSeed *seed)
{
    const parserFile *file;

    while (seed->got < seed->length) {
        uint64_t position = seed->offset + seed->got;
        uint64_t left;

        TAILQ_FOREACH(file, &seed->torrent->files, entries) {
            if (position >= file->offset && position < file->offset + file->length) {
                break;
            }
        }
        if (file == NULL) {
            snprintf(seed->message, sizeof(seed->message), "%s: range beyond the torrent",
                     seed->url);
            done(seed, WEBSEED_EINVAL);
            return;
        }
        left = file->offset + file->length - position;
        seed->segment = (left < (uint64_t)(seed->length - seed->got))
                            ? (uint32_t)left : seed->length - seed->got;
        if (file->isPadding) {
            memset(seed->buffer + seed->got, 0, seed->segment);
            seed->got += seed->segment;
            continue;
        }
        seed->file = file;
        seed->fileStart = position - file->offset;
        seed->redirects = 0;
        if (webSeedFileUrl(seed, file, seed->current, HTTP_MAX_URL_LENGTH) != WEBSEED_OK) {
            snprintf(seed->message, sizeof(seed->message), "%s: URL too long", seed->url);
            done(seed, WEBSEED_EINVAL);
            return;
        }
        sendRequest(seed);
        return;
    }
    done(seed, WEBSEED_OK);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t webSeedBegin(parserContext *context, const parserTorrent *torrent, const char *url,
                     webSeed **handle)
{
    webSeed *seed;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (context == NULL || torrent == NULL || url == NULL || handle == NULL ||
        strlen(url) >= HTTP_MAX_URL_LENGTH) {
        return WEBSEED_EINVAL;
    }
    if (strncmp(url, "http://", 7u) != 0 && strncmp(url, "https://", 8u) != 0) {
        return WEBSEED_EINVAL;
    }
    seed = context->allocator.realloc(NULL, sizeof(*seed));
    if (seed == NULL) {
        return WEBSEED_ENOMEM;
    }
    memset(seed, 0, sizeof(*seed));
    seed->context = context;
    seed->torrent = torrent;
    seed->url = copyText(context, url);
    seed->current = allocate(seed, HTTP_MAX_URL_LENGTH);
    if (seed->url == NULL || seed->current == NULL) {
        webSeedEnd(seed);
        return WEBSEED_ENOMEM;
    }
    *handle = seed;
    return WEBSEED_OK;
}

int32_t webSeedFetch(webSeed *seed, uint64_t offset, uint32_t length, uint8_t *buffer,
                     uint32_t timeoutSeconds, webSeedCallback callback, void *privateArg)
{
    if (seed == NULL || buffer == NULL || callback == NULL || length == 0 ||
        offset + length > seed->torrent->totalLength) {
        return WEBSEED_EINVAL;
    }
    if (seed->busy) {
        return WEBSEED_EBUSY;
    }
    seed->busy = 1;
    seed->callback = callback;
    seed->privateArg = privateArg;
    seed->buffer = buffer;
    seed->offset = offset;
    seed->length = length;
    seed->got = 0;
    seed->timeoutSeconds = timeoutSeconds;
    fetchNext(seed);
    return WEBSEED_OK;
}

void webSeedCancel(webSeed *seed)
{
    if (seed == NULL || !seed->busy) {
        return;
    }
    if (seed->request != NULL) {
        httpCancel(seed->request);
        seed->request = NULL;
    }
    seed->busy = 0;
}

void webSeedEnd(webSeed *seed)
{
    if (seed == NULL) {
        return;
    }
    webSeedCancel(seed);
    if (seed->url != NULL) {
        seed->context->allocator.free(seed->url);
    }
    if (seed->current != NULL) {
        seed->context->allocator.free(seed->current);
    }
    seed->context->allocator.free(seed);
}

const char *webSeedUrl(const webSeed *seed)
{
    return (seed != NULL) ? seed->url : "";
}
