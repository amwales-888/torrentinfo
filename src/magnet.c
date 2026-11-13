/*
 * magnet.c - Magnet links. See magnet.h.
 */
#include <stdio.h>
#include <string.h>

#include "magnet.h"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int startsWith(const char *text, size_t length, const char *prefix)
{
    size_t i;
    size_t prefixLength = strlen(prefix);

    if (length < prefixLength) {
        return 0;
    }
    for (i = 0; i < prefixLength; i++) {
        if (lower((unsigned char)text[i]) != lower((unsigned char)prefix[i])) {
            return 0;
        }
    }
    return 1;
}

static int hexValue(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = lower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Percent-decode text[0..length) into a new string; '+' is a space. */
static char *decode(parserContext *context, const char *text, size_t length)
{
    char  *out = context->allocator.realloc(NULL, length + 1u);
    size_t used = 0;
    size_t i;

    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < length; i++) {
        if (text[i] == '%' && i + 2u < length && hexValue((unsigned char)text[i + 1u]) >= 0 &&
            hexValue((unsigned char)text[i + 2u]) >= 0) {
            out[used++] = (char)(hexValue((unsigned char)text[i + 1u]) * 16 +
                                 hexValue((unsigned char)text[i + 2u]));
            i += 2u;
        } else if (text[i] == '+') {
            out[used++] = ' ';
        } else {
            out[used++] = text[i];
        }
    }
    out[used] = '\0';
    return out;
}

/* Append a string to a list (taking ownership); drops it past the limit. */
static int32_t append(parserContext *context, char ***list, uint32_t *count, char *item)
{
    char **grown;

    if (item == NULL) {
        return MAGNET_ENOMEM;
    }
    if (*count >= MAGNET_MAX_ITEMS || item[0] == '\0') {
        context->allocator.free(item);
        return MAGNET_OK;
    }
    grown = context->allocator.realloc(*list, sizeof(char *) * (*count + 1u));
    if (grown == NULL) {
        context->allocator.free(item);
        return MAGNET_ENOMEM;
    }
    *list = grown;
    (*list)[(*count)++] = item;
    return MAGNET_OK;
}

/* "urn:btih:" followed by 40 hex digits or 32 base32 digits. */
static int parseInfoHash(const char *value, uint8_t hash[PARSER_SHA1_LENGTH])
{
    size_t length = strlen(value);
    size_t i;

    if (!startsWith(value, length, "urn:btih:")) {
        return 0;
    }
    value += 9;
    length -= 9;
    if (length == 40) {
        for (i = 0; i < 20; i++) {
            int high = hexValue((unsigned char)value[2 * i]);
            int low = hexValue((unsigned char)value[2 * i + 1]);

            if (high < 0 || low < 0) {
                return 0;
            }
            hash[i] = (uint8_t)(high * 16 + low);
        }
        return 1;
    }
    if (length == 32) {
        uint64_t bits = 0;
        uint32_t bitCount = 0;
        size_t   out = 0;

        for (i = 0; i < 32; i++) {
            int c = lower((unsigned char)value[i]);
            int digit = (c >= 'a' && c <= 'z') ? c - 'a' : (c >= '2' && c <= '7') ? c - '2' + 26 : -1;

            if (digit < 0) {
                return 0;
            }
            bits = (bits << 5) | (uint64_t)digit;
            bitCount += 5;
            if (bitCount >= 8) {
                hash[out++] = (uint8_t)(bits >> (bitCount - 8));
                bitCount -= 8;
            }
        }
        return out == 20;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int magnetIs(const char *text)
{
    return text != NULL && startsWith(text, strlen(text), "magnet:");
}

int32_t magnetParse(parserContext *context, const char *uri, magnetLink *link)
{
    const char *at;
    int         haveHash = 0;
    int32_t     status = MAGNET_OK;

    if (context == NULL || uri == NULL || link == NULL) {
        return MAGNET_EINVAL;
    }
    memset(link, 0, sizeof(*link));
    if (!magnetIs(uri)) {
        return MAGNET_EINVAL;
    }
    at = strchr(uri, '?');
    if (at == NULL) {
        return MAGNET_EINVAL;
    }
    at++;
    while (*at != '\0' && status == MAGNET_OK) {
        const char *end = strchr(at, '&');
        const char *equals;
        size_t      length = (end != NULL) ? (size_t)(end - at) : strlen(at);
        size_t      keyLength;
        char       *value;

        equals = memchr(at, '=', length);
        if (equals != NULL) {
            keyLength = (size_t)(equals - at);
            value = decode(context, equals + 1, length - keyLength - 1u);
            if (value == NULL) {
                status = MAGNET_ENOMEM;
                break;
            }
            if (keyLength == 2 && startsWith(at, 2, "xt")) {
                if (!haveHash && parseInfoHash(value, link->infoHash)) {
                    haveHash = 1;
                }
                context->allocator.free(value);
            } else if (keyLength == 2 && startsWith(at, 2, "dn") && link->name == NULL) {
                link->name = value;
            } else if (keyLength == 2 && startsWith(at, 2, "tr")) {
                status = append(context, &link->trackers, &link->trackerCount, value);
            } else if (keyLength == 4 && startsWith(at, 4, "x.pe")) {
                status = append(context, &link->peers, &link->peerCount, value);
            } else if (keyLength == 2 && startsWith(at, 2, "ws")) {
                status = append(context, &link->webSeeds, &link->webSeedCount, value);
            } else {
                context->allocator.free(value);
            }
        }
        at += length;
        if (*at == '&') {
            at++;
        }
    }
    if (status == MAGNET_OK && !haveHash) {
        status = MAGNET_EINVAL;                     /* e.g. only a v2 (btmh) hash */
    }
    if (status != MAGNET_OK) {
        magnetFree(context, link);
    }
    return status;
}

static void freeList(parserContext *context, char **list, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        context->allocator.free(list[i]);
    }
    if (list != NULL) {
        context->allocator.free(list);
    }
}

void magnetFree(parserContext *context, magnetLink *link)
{
    if (context == NULL || link == NULL) {
        return;
    }
    if (link->name != NULL) {
        context->allocator.free(link->name);
    }
    freeList(context, link->trackers, link->trackerCount);
    freeList(context, link->peers, link->peerCount);
    freeList(context, link->webSeeds, link->webSeedCount);
    memset(link, 0, sizeof(*link));
}

/* Append bencode to a growing buffer. */
typedef struct builder {
    parserContext *context;
    uint8_t       *data;
    uint64_t       used;
    uint64_t       size;
    int            failed;
} builder;

static void add(builder *b, const void *bytes, uint64_t length)
{
    if (b->failed) {
        return;
    }
    if (b->used + length > b->size) {
        uint64_t size = (b->size == 0) ? 1024u : b->size;
        uint8_t *grown;

        while (size < b->used + length) {
            size *= 2u;
        }
        grown = b->context->allocator.realloc(b->data, (size_t)size);
        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->size = size;
    }
    memcpy(b->data + b->used, bytes, (size_t)length);
    b->used += length;
}

static void addString(builder *b, const char *text)
{
    char prefix[24];

    snprintf(prefix, sizeof(prefix), "%lu:", (unsigned long)strlen(text));
    add(b, prefix, strlen(prefix));
    add(b, text, strlen(text));
}

int32_t magnetBuildTorrent(parserContext *context, const magnetLink *link, const uint8_t *info,
                           uint64_t infoLength, uint8_t **torrent, uint64_t *length)
{
    builder  b;
    uint32_t i;

    if (context == NULL || link == NULL || info == NULL || torrent == NULL || length == NULL) {
        return MAGNET_EINVAL;
    }
    memset(&b, 0, sizeof(b));
    b.context = context;
    /* Keys in sorted order: announce, announce-list, created by, info, url-list. */
    add(&b, "d", 1);
    if (link->trackerCount > 0) {
        addString(&b, "announce");
        addString(&b, link->trackers[0]);
        addString(&b, "announce-list");
        add(&b, "l", 1);
        for (i = 0; i < link->trackerCount; i++) {
            add(&b, "l", 1);                        /* each tracker its own tier */
            addString(&b, link->trackers[i]);
            add(&b, "e", 1);
        }
        add(&b, "e", 1);
    }
    addString(&b, "created by");
    addString(&b, "torrentinfo (from a magnet link)");
    addString(&b, "info");
    add(&b, info, infoLength);
    if (link->webSeedCount > 0) {
        addString(&b, "url-list");
        add(&b, "l", 1);
        for (i = 0; i < link->webSeedCount; i++) {
            addString(&b, link->webSeeds[i]);
        }
        add(&b, "e", 1);
    }
    add(&b, "e", 1);
    if (b.failed) {
        if (b.data != NULL) {
            context->allocator.free(b.data);
        }
        return MAGNET_ENOMEM;
    }
    *torrent = b.data;
    *length = b.used;
    return MAGNET_OK;
}

/* ------------------------------------------------------------------------ */
/* Making a magnet link from a torrent                                      */
/* ------------------------------------------------------------------------ */

/* "&key=" then 'value' percent-encoded (all but unreserved characters). */
static void addParameter(builder *b, const char *key, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *at;

    add(b, "&", 1);
    add(b, key, strlen(key));
    add(b, "=", 1);
    for (at = (const unsigned char *)value; *at != '\0'; at++) {
        if ((*at >= 'A' && *at <= 'Z') || (*at >= 'a' && *at <= 'z') ||
            (*at >= '0' && *at <= '9') || *at == '-' || *at == '.' || *at == '_' || *at == '~') {
            add(b, at, 1);
        } else {
            char escaped[3];

            escaped[0] = '%';
            escaped[1] = hex[*at >> 4];
            escaped[2] = hex[*at & 0x0fu];
            add(b, escaped, 3);
        }
    }
}

int32_t magnetFromTorrent(parserContext *context, const parserTorrent *torrent, char **uri)
{
    const parserTier *tier;
    const parserUrl  *url;
    builder           b;
    char              text[64];
    uint32_t          i;

    if (context == NULL || torrent == NULL || uri == NULL) {
        return MAGNET_EINVAL;
    }
    *uri = NULL;
    memset(&b, 0, sizeof(b));
    b.context = context;
    add(&b, "magnet:?xt=urn:btih:", 20);
    for (i = 0; i < PARSER_SHA1_LENGTH; i++) {
        snprintf(text, sizeof(text), "%02x", torrent->infoHash[i]);
        add(&b, text, 2);
    }
    if (torrent->name != NULL && torrent->name[0] != '\0') {
        addParameter(&b, "dn", torrent->name);
    }
    /* BEP 12: announce-list replaces announce when present. */
    if (torrent->tierCount > 0) {
        TAILQ_FOREACH(tier, &torrent->announceList, entries) {
            TAILQ_FOREACH(url, &tier->urls, entries) {
                addParameter(&b, "tr", url->url);
            }
        }
    } else if (torrent->announce != NULL) {
        addParameter(&b, "tr", torrent->announce);
    }
    TAILQ_FOREACH(url, &torrent->webSeeds, entries) {
        addParameter(&b, "ws", url->url);
    }
    add(&b, "", 1);                                 /* the terminating zero */
    if (b.failed) {
        if (b.data != NULL) {
            context->allocator.free(b.data);
        }
        return MAGNET_ENOMEM;
    }
    *uri = (char *)b.data;
    return MAGNET_OK;
}
