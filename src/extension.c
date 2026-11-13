/*
 * extension.c - BEP 10 extended handshake and BEP 11 peer exchange
 * messages. See extension.h.
 */
#include <stdio.h>
#include <string.h>

#include "extension.h"
#include "parser.h"

/* ------------------------------------------------------------------------ */
/* Writing bencode into a fixed buffer                                      */
/* ------------------------------------------------------------------------ */

typedef struct writer {
    uint8_t *out;
    size_t   size;
    size_t   used;
    int      failed;
} writer;

static void put(writer *w, const void *data, size_t length)
{
    if (w->failed || length > w->size - w->used) {
        w->failed = 1;
        return;
    }
    memcpy(w->out + w->used, data, length);
    w->used += length;
}

static void putText(writer *w, const char *text)
{
    put(w, text, strlen(text));
}

static void putString(writer *w, const void *data, size_t length)
{
    char prefix[24];

    snprintf(prefix, sizeof(prefix), "%lu:", (unsigned long)length);
    putText(w, prefix);
    put(w, data, length);
}

static void putKey(writer *w, const char *key)
{
    putString(w, key, strlen(key));
}

static void putInteger(writer *w, uint64_t value)
{
    char text[32];

    snprintf(text, sizeof(text), "i%llue", (unsigned long long)value);
    putText(w, text);
}

/* The compact form of the peers of one family, and optionally their flags. */
static void putPeers(writer *w, const char *key, const extensionPeer *peers, uint32_t count,
                     uint8_t family, int flags)
{
    uint8_t  entry[18];
    size_t   width = (family == 4u) ? 6u : 18u;
    uint32_t total = 0;
    uint32_t i;
    char     prefix[24];

    for (i = 0; i < count && i < EXTENSION_MAX_PEX_PEERS; i++) {
        total += (peers[i].family == family) ? 1u : 0u;
    }
    if (total == 0) {
        return;
    }
    putKey(w, key);
    snprintf(prefix, sizeof(prefix), "%lu:", (unsigned long)(flags ? total : total * width));
    putText(w, prefix);
    for (i = 0; i < count && i < EXTENSION_MAX_PEX_PEERS; i++) {
        if (peers[i].family != family) {
            continue;
        }
        if (flags) {
            put(w, &peers[i].flags, 1);
        } else {
            memcpy(entry, peers[i].address, width - 2u);
            entry[width - 2u] = (uint8_t)(peers[i].port >> 8);
            entry[width - 1u] = (uint8_t)(peers[i].port & 0xffu);
            put(w, entry, width);
        }
    }
}

static int32_t finish(writer *w, size_t *length)
{
    if (w->failed) {
        return EXTENSION_EINVAL;
    }
    *length = w->used;
    return EXTENSION_OK;
}

/* ------------------------------------------------------------------------ */
/* Handshake                                                                */
/* ------------------------------------------------------------------------ */

int32_t extensionEncodeHandshake(const extensionHandshake *handshake, uint8_t *out, size_t size,
                                 size_t *length)
{
    writer w = { out, size, 0, 0 };

    if (handshake == NULL || out == NULL || length == NULL) {
        return EXTENSION_EINVAL;
    }
    /* Keys in sorted order: m, metadata_size, p, reqq, v. */
    putText(&w, "d");
    putKey(&w, "m");
    putText(&w, "d");
    if (handshake->metadataId != 0) {
        putKey(&w, "ut_metadata");
        putInteger(&w, handshake->metadataId);
    }
    if (handshake->pexId != 0) {
        putKey(&w, "ut_pex");
        putInteger(&w, handshake->pexId);
    }
    putText(&w, "e");
    if (handshake->metadataSize != 0) {
        putKey(&w, "metadata_size");
        putInteger(&w, handshake->metadataSize);
    }
    if (handshake->port != 0) {
        putKey(&w, "p");
        putInteger(&w, handshake->port);
    }
    if (handshake->requestQueue != 0) {
        putKey(&w, "reqq");
        putInteger(&w, handshake->requestQueue);
    }
    if (handshake->client[0] != '\0') {
        putKey(&w, "v");
        putString(&w, handshake->client, strlen(handshake->client));
    }
    putText(&w, "e");
    return finish(&w, length);
}

static int keyIs(const uint8_t *key, uint64_t keyLength, const char *name)
{
    return keyLength == strlen(name) && memcmp(key, name, keyLength) == 0;
}

int32_t extensionDecodeHandshake(const uint8_t *data, size_t length, extensionHandshake *handshake)
{
    parserCursor   cursor;
    const uint8_t *key;
    const uint8_t *text;
    uint64_t       keyLength;
    uint64_t       textLength;
    int64_t        value;
    uint8_t        more;
    uint64_t       i;

    if (data == NULL || handshake == NULL) {
        return EXTENSION_EINVAL;
    }
    memset(handshake, 0, sizeof(*handshake));
    memset(&cursor, 0, sizeof(cursor));
    cursor.buffer = data;
    cursor.length = length;
    if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
        return EXTENSION_EPROTO;
    }
    for (;;) {
        if (parserContainerNext(&cursor, &more) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
        if (!more) {
            break;
        }
        if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
        if (keyIs(key, keyLength, "m") && parserCursorPeek(&cursor) == 'd') {
            if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
                return EXTENSION_EPROTO;
            }
            for (;;) {
                if (parserContainerNext(&cursor, &more) != PARSER_OK) {
                    return EXTENSION_EPROTO;
                }
                if (!more) {
                    break;
                }
                if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK) {
                    return EXTENSION_EPROTO;
                }
                if ((keyIs(key, keyLength, "ut_pex") || keyIs(key, keyLength, "ut_metadata")) &&
                    parserCursorPeek(&cursor) == 'i') {
                    int pex = keyIs(key, keyLength, "ut_pex");

                    if (parserReadInteger(&cursor, &value) != PARSER_OK) {
                        return EXTENSION_EPROTO;
                    }
                    /* 0 means "disabled"; ids are one byte. */
                    if (pex) {
                        handshake->pexId = (value > 0 && value <= 255) ? (uint8_t)value : 0u;
                    } else {
                        handshake->metadataId = (value > 0 && value <= 255) ? (uint8_t)value : 0u;
                    }
                } else if (parserSkipValue(&cursor) != PARSER_OK) {
                    return EXTENSION_EPROTO;
                }
            }
        } else if (keyIs(key, keyLength, "metadata_size") && parserCursorPeek(&cursor) == 'i') {
            if (parserReadInteger(&cursor, &value) != PARSER_OK) {
                return EXTENSION_EPROTO;
            }
            handshake->metadataSize =
                (value > 0 && value <= (int64_t)EXTENSION_MAX_METADATA) ? (uint32_t)value : 0u;
        } else if (keyIs(key, keyLength, "p") && parserCursorPeek(&cursor) == 'i') {
            if (parserReadInteger(&cursor, &value) != PARSER_OK) {
                return EXTENSION_EPROTO;
            }
            handshake->port = (value > 0 && value <= 65535) ? (uint16_t)value : 0u;
        } else if (keyIs(key, keyLength, "reqq") && parserCursorPeek(&cursor) == 'i') {
            if (parserReadInteger(&cursor, &value) != PARSER_OK) {
                return EXTENSION_EPROTO;
            }
            handshake->requestQueue = (value > 0 && value <= 100000) ? (uint32_t)value : 0u;
        } else if (keyIs(key, keyLength, "v") && parserCursorPeek(&cursor) >= '0' &&
                   parserCursorPeek(&cursor) <= '9') {
            if (parserReadString(&cursor, &text, &textLength) != PARSER_OK) {
                return EXTENSION_EPROTO;
            }
            for (i = 0; i < textLength && i < EXTENSION_CLIENT_LENGTH - 1u; i++) {
                handshake->client[i] = (text[i] >= 0x20 && text[i] < 0x7f) ? (char)text[i] : '?';
            }
            handshake->client[i] = '\0';
        } else if (parserSkipValue(&cursor) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
    }
    return EXTENSION_OK;
}

/* ------------------------------------------------------------------------ */
/* Peer exchange                                                            */
/* ------------------------------------------------------------------------ */

int32_t extensionEncodePex(const extensionPeer *added, uint32_t addedCount,
                           const extensionPeer *dropped, uint32_t droppedCount, uint8_t *out,
                           size_t size, size_t *length)
{
    writer w = { out, size, 0, 0 };

    if (out == NULL || length == NULL || (addedCount > 0 && added == NULL) ||
        (droppedCount > 0 && dropped == NULL)) {
        return EXTENSION_EINVAL;
    }
    /* Keys in sorted order. */
    putText(&w, "d");
    putPeers(&w, "added", added, addedCount, 4u, 0);
    putPeers(&w, "added.f", added, addedCount, 4u, 1);
    putPeers(&w, "added6", added, addedCount, 6u, 0);
    putPeers(&w, "added6.f", added, addedCount, 6u, 1);
    putPeers(&w, "dropped", dropped, droppedCount, 4u, 0);
    putPeers(&w, "dropped6", dropped, droppedCount, 6u, 0);
    putText(&w, "e");
    return finish(&w, length);
}

/* Call back for each compact address in 'list' (with flags from 'flags'). */
static void reportPeers(const uint8_t *list, uint64_t listLength, const uint8_t *flags,
                        uint64_t flagsLength, uint8_t family, int added,
                        extensionPeerCallback callback, void *privateArg)
{
    extensionPeer peer;
    size_t        width = (family == 4u) ? 6u : 18u;
    uint64_t      count = listLength / width;
    uint64_t      i;

    if (list == NULL) {
        return;
    }
    for (i = 0; i < count && i < EXTENSION_MAX_PEX_PEERS; i++) {
        const uint8_t *entry = list + i * width;

        memset(&peer, 0, sizeof(peer));
        peer.family = family;
        memcpy(peer.address, entry, width - 2u);
        peer.port = (uint16_t)((entry[width - 2u] << 8) | entry[width - 1u]);
        peer.flags = (flags != NULL && i < flagsLength) ? flags[i] : 0u;
        if (peer.port != 0) {
            callback(privateArg, &peer, added);
        }
    }
}

int32_t extensionDecodePex(const uint8_t *data, size_t length, extensionPeerCallback callback,
                           void *privateArg)
{
    parserCursor   cursor;
    const uint8_t *key;
    const uint8_t *value;
    const uint8_t *lists[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    uint64_t       lengths[6] = { 0, 0, 0, 0, 0, 0 };
    static const char *names[6] = { "added", "added.f", "added6", "added6.f", "dropped",
                                    "dropped6" };
    uint64_t       keyLength;
    uint64_t       valueLength;
    uint8_t        more;
    int            i;

    if (data == NULL || callback == NULL) {
        return EXTENSION_EINVAL;
    }
    memset(&cursor, 0, sizeof(cursor));
    cursor.buffer = data;
    cursor.length = length;
    if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
        return EXTENSION_EPROTO;
    }
    for (;;) {
        int known = -1;

        if (parserContainerNext(&cursor, &more) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
        if (!more) {
            break;
        }
        if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
        for (i = 0; i < 6; i++) {
            if (keyIs(key, keyLength, names[i])) {
                known = i;
            }
        }
        if (known >= 0 && parserCursorPeek(&cursor) >= '0' && parserCursorPeek(&cursor) <= '9') {
            if (parserReadString(&cursor, &value, &valueLength) != PARSER_OK) {
                return EXTENSION_EPROTO;
            }
            lists[known] = value;
            lengths[known] = valueLength;
        } else if (parserSkipValue(&cursor) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
    }
    reportPeers(lists[0], lengths[0], lists[1], lengths[1], 4u, 1, callback, privateArg);
    reportPeers(lists[2], lengths[2], lists[3], lengths[3], 6u, 1, callback, privateArg);
    reportPeers(lists[4], lengths[4], NULL, 0, 4u, 0, callback, privateArg);
    reportPeers(lists[5], lengths[5], NULL, 0, 6u, 0, callback, privateArg);
    return EXTENSION_OK;
}

/* ------------------------------------------------------------------------ */
/* Metadata exchange (BEP 9)                                                */
/* ------------------------------------------------------------------------ */

int32_t extensionEncodeMetadata(uint32_t type, uint32_t piece, uint32_t totalSize,
                                uint8_t *out, size_t size, size_t *length)
{
    writer w = { out, size, 0, 0 };

    if (out == NULL || length == NULL || type > EXTENSION_METADATA_REJECT) {
        return EXTENSION_EINVAL;
    }
    /* Keys in sorted order: msg_type, piece, total_size. */
    putText(&w, "d");
    putKey(&w, "msg_type");
    putInteger(&w, type);
    putKey(&w, "piece");
    putInteger(&w, piece);
    if (type == EXTENSION_METADATA_DATA) {
        putKey(&w, "total_size");
        putInteger(&w, totalSize);
    }
    putText(&w, "e");
    return finish(&w, length);
}

int32_t extensionDecodeMetadata(const uint8_t *data, size_t length, uint32_t *type,
                                uint32_t *piece, uint32_t *totalSize, size_t *dataOffset)
{
    parserCursor   cursor;
    const uint8_t *key;
    uint64_t       keyLength;
    int64_t        value;
    uint8_t        more;
    int            haveType = 0;
    int            havePiece = 0;

    if (data == NULL || type == NULL || piece == NULL || totalSize == NULL ||
        dataOffset == NULL) {
        return EXTENSION_EINVAL;
    }
    *type = 0;
    *piece = 0;
    *totalSize = 0;
    memset(&cursor, 0, sizeof(cursor));
    cursor.buffer = data;
    cursor.length = length;
    if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
        return EXTENSION_EPROTO;
    }
    for (;;) {
        if (parserContainerNext(&cursor, &more) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
        if (!more) {
            break;
        }
        if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
        if (parserCursorPeek(&cursor) == 'i' &&
            (keyIs(key, keyLength, "msg_type") || keyIs(key, keyLength, "piece") ||
             keyIs(key, keyLength, "total_size"))) {
            if (parserReadInteger(&cursor, &value) != PARSER_OK || value < 0 ||
                value > (int64_t)UINT32_MAX) {
                return EXTENSION_EPROTO;
            }
            if (keyIs(key, keyLength, "msg_type")) {
                *type = (uint32_t)value;
                haveType = 1;
            } else if (keyIs(key, keyLength, "piece")) {
                *piece = (uint32_t)value;
                havePiece = 1;
            } else {
                *totalSize = (uint32_t)value;
            }
        } else if (parserSkipValue(&cursor) != PARSER_OK) {
            return EXTENSION_EPROTO;
        }
    }
    if (!haveType || !havePiece) {
        return EXTENSION_EPROTO;
    }
    *dataOffset = (size_t)cursor.position;          /* the piece's bytes follow */
    return EXTENSION_OK;
}
