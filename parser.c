/*
 * parser.c - BitTorrent metainfo (.torrent) parser.
 *
 * A single-pass recursive-descent bencode reader. Rather than building a
 * generic tree, it walks the input once and copies only the fields that
 * a client needs into parserTorrent. Unknown keys are skipped.
 *
 * Everything is bounds-checked against the buffer length. Nesting depth is
 * limited to PARSER_MAX_DEPTH, so hostile input cannot overflow the stack.
 */
#include <string.h>

#include "parser.h"

/* Evaluate an expression; return its result early if it is not PARSER_OK. */
#define PARSER_TRY(expression)                                  \
    do {                                                        \
        int32_t parserTryResult = (expression);                 \
        if (parserTryResult != PARSER_OK) {                     \
            return parserTryResult;                             \
        }                                                       \
    } while (0)

/* Bits used to detect duplicate keys: root dictionary */
#define PARSER_KEY_ANNOUNCE         (1u << 0)
#define PARSER_KEY_ANNOUNCE_LIST    (1u << 1)
#define PARSER_KEY_COMMENT          (1u << 2)
#define PARSER_KEY_CREATED_BY       (1u << 3)
#define PARSER_KEY_CREATION_DATE    (1u << 4)
#define PARSER_KEY_ENCODING         (1u << 5)
#define PARSER_KEY_INFO             (1u << 6)
#define PARSER_KEY_URL_LIST         (1u << 7)

/* Bits used to detect duplicate keys: info and file dictionaries */
#define PARSER_KEY_NAME             (1u << 0)
#define PARSER_KEY_PIECE_LENGTH     (1u << 1)
#define PARSER_KEY_PIECES           (1u << 2)
#define PARSER_KEY_PRIVATE          (1u << 3)
#define PARSER_KEY_LENGTH           (1u << 4)
#define PARSER_KEY_FILES            (1u << 5)
#define PARSER_KEY_PATH             (1u << 6)
#define PARSER_KEY_ATTR             (1u << 7)

typedef struct parserCursor {
    const uint8_t *buffer;
    uint64_t       length;
    uint64_t       position;
    uint32_t       depth;
} parserCursor;

/* ======================================================================== */
/* SHA-1 (FIPS 180-1), used for the info hash                               */
/* ======================================================================== */

static uint32_t sha1Rotate(uint32_t value, uint32_t bits)
{
    return (value << bits) | (value >> (32u - bits));
}

static void sha1Block(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, temp;
    uint32_t i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        temp = sha1Rotate(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1Rotate(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void parserSha1(const uint8_t *data, uint64_t length, uint8_t digest[PARSER_SHA1_LENGTH])
{
    uint32_t state[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    uint8_t  block[64];
    uint64_t remaining = length;
    uint64_t bits = length * 8u;
    uint32_t i;

    while (remaining >= 64u) {
        sha1Block(state, data);
        data += 64;
        remaining -= 64u;
    }

    memset(block, 0, sizeof(block));
    memcpy(block, data, (size_t)remaining);
    block[remaining] = 0x80u;
    if (remaining >= 56u) {
        sha1Block(state, block);
        memset(block, 0, sizeof(block));
    }
    for (i = 0; i < 8; i++) {
        block[63 - i] = (uint8_t)(bits >> (i * 8u));
    }
    sha1Block(state, block);

    for (i = 0; i < 5; i++) {
        digest[i * 4]     = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(state[i]);
    }
}

/* ======================================================================== */
/* Memory helpers                                                           */
/* ======================================================================== */

static void *allocate(parserContext *context, uint64_t size)
{
    void *memory;

    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    memory = context->allocator.realloc(NULL, (size_t)size);
    if (memory != NULL) {
        memset(memory, 0, (size_t)size);
    }
    return memory;
}

static void release(parserContext *context, void *memory)
{
    if (memory != NULL) {
        context->allocator.free(memory);
    }
}

/* Copy 'length' bytes into a new NUL-terminated string. */
static int32_t copyString(parserContext *context, const uint8_t *data,
                          uint64_t length, char **out)
{
    char *copy;

    if (length == UINT64_MAX) {
        return PARSER_ERROR_OVERFLOW;
    }
    copy = allocate(context, length + 1u);
    if (copy == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }
    if (length > 0) {
        memcpy(copy, data, (size_t)length);
    }
    *out = copy;
    return PARSER_OK;
}

static void freeUrlList(parserContext *context, struct parserUrlList *list)
{
    parserUrl *url;

    while ((url = TAILQ_FIRST(list)) != NULL) {
        TAILQ_REMOVE(list, url, entries);
        release(context, url->url);
        release(context, url);
    }
}

static void freePathList(parserContext *context, struct parserPathList *list)
{
    parserPathComponent *component;

    while ((component = TAILQ_FIRST(list)) != NULL) {
        TAILQ_REMOVE(list, component, entries);
        release(context, component->name);
        release(context, component);
    }
}

static void resetTorrent(parserTorrent *torrent)
{
    memset(torrent, 0, sizeof(*torrent));
    TAILQ_INIT(&torrent->announceList);
    TAILQ_INIT(&torrent->webSeeds);
    TAILQ_INIT(&torrent->files);
}

/* ======================================================================== */
/* Bencode primitives                                                       */
/* ======================================================================== */

static int32_t cursorPeek(const parserCursor *cursor)
{
    if (cursor->position >= cursor->length) {
        return -1;
    }
    return (int32_t)cursor->buffer[cursor->position];
}

static uint8_t isDigit(int32_t c)
{
    return (uint8_t)(c >= '0' && c <= '9');
}

static int32_t expectByte(parserCursor *cursor, uint8_t value)
{
    int32_t c = cursorPeek(cursor);

    if (c < 0) {
        return PARSER_ERROR_TRUNCATED;
    }
    if (c != (int32_t)value) {
        return PARSER_ERROR_SYNTAX;
    }
    cursor->position++;
    return PARSER_OK;
}

/* i<digits>e  - no leading zeros, no "-0" */
static int32_t readInteger(parserCursor *cursor, int64_t *value)
{
    uint64_t magnitude = 0;
    uint64_t digits = 0;
    uint64_t limit;
    uint8_t  negative = 0;
    int32_t  c;

    PARSER_TRY(expectByte(cursor, 'i'));

    if (cursorPeek(cursor) == '-') {
        negative = 1;
        cursor->position++;
    }
    limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;

    while (isDigit(c = cursorPeek(cursor))) {
        uint64_t digit = (uint64_t)(c - '0');

        if (digits > 0 && magnitude == 0) {
            return PARSER_ERROR_SYNTAX;             /* leading zero */
        }
        if (magnitude > (limit - digit) / 10u) {
            return PARSER_ERROR_OVERFLOW;
        }
        magnitude = magnitude * 10u + digit;
        digits++;
        cursor->position++;
    }

    if (digits == 0) {
        return (c < 0) ? PARSER_ERROR_TRUNCATED : PARSER_ERROR_SYNTAX;
    }
    if (negative && magnitude == 0) {
        return PARSER_ERROR_SYNTAX;                 /* "-0" */
    }
    PARSER_TRY(expectByte(cursor, 'e'));

    if (!negative) {
        *value = (int64_t)magnitude;
    } else if (magnitude == (uint64_t)INT64_MAX + 1u) {
        *value = INT64_MIN;
    } else {
        *value = -(int64_t)magnitude;
    }
    return PARSER_OK;
}

/* <length>:<bytes>  - returns a pointer into the buffer, nothing copied */
static int32_t readString(parserCursor *cursor, const uint8_t **data, uint64_t *length)
{
    uint64_t magnitude = 0;
    uint64_t digits = 0;
    int32_t  c;

    while (isDigit(c = cursorPeek(cursor))) {
        uint64_t digit = (uint64_t)(c - '0');

        if (digits > 0 && magnitude == 0) {
            return PARSER_ERROR_SYNTAX;             /* leading zero */
        }
        if (magnitude > (UINT64_MAX - digit) / 10u) {
            return PARSER_ERROR_OVERFLOW;
        }
        magnitude = magnitude * 10u + digit;
        digits++;
        cursor->position++;
    }

    if (digits == 0) {
        return (c < 0) ? PARSER_ERROR_TRUNCATED : PARSER_ERROR_SYNTAX;
    }
    PARSER_TRY(expectByte(cursor, ':'));

    if (magnitude > cursor->length - cursor->position) {
        return PARSER_ERROR_TRUNCATED;
    }
    *data = cursor->buffer + cursor->position;
    *length = magnitude;
    cursor->position += magnitude;
    return PARSER_OK;
}

/* Consume 'l' or 'd' and enter a container. */
static int32_t containerBegin(parserCursor *cursor, uint8_t type)
{
    PARSER_TRY(expectByte(cursor, type));
    if (++cursor->depth > PARSER_MAX_DEPTH) {
        return PARSER_ERROR_TOO_DEEP;
    }
    return PARSER_OK;
}

/* Sets *more to 0 and consumes 'e' at the end of a container, else sets 1. */
static int32_t containerNext(parserCursor *cursor, uint8_t *more)
{
    int32_t c = cursorPeek(cursor);

    if (c < 0) {
        return PARSER_ERROR_TRUNCATED;
    }
    if (c == 'e') {
        cursor->position++;
        cursor->depth--;
        *more = 0;
    } else {
        *more = 1;
    }
    return PARSER_OK;
}

/* Skip over any single bencoded value. */
static int32_t skipValue(parserCursor *cursor)
{
    const uint8_t *data;
    uint64_t       length;
    int64_t        integer;
    uint8_t        more;
    int32_t        c = cursorPeek(cursor);

    if (c < 0) {
        return PARSER_ERROR_TRUNCATED;
    }
    if (c == 'i') {
        return readInteger(cursor, &integer);
    }
    if (isDigit(c)) {
        return readString(cursor, &data, &length);
    }
    if (c == 'l' || c == 'd') {
        PARSER_TRY(containerBegin(cursor, (uint8_t)c));
        for (;;) {
            PARSER_TRY(containerNext(cursor, &more));
            if (!more) {
                break;
            }
            if (c == 'd') {
                PARSER_TRY(readString(cursor, &data, &length));
            }
            PARSER_TRY(skipValue(cursor));
        }
        return PARSER_OK;
    }
    return PARSER_ERROR_SYNTAX;
}

/* ======================================================================== */
/* Field helpers                                                            */
/* ======================================================================== */

static uint8_t keyEquals(const uint8_t *key, uint64_t keyLength, const char *literal)
{
    size_t literalLength = strlen(literal);

    return (uint8_t)(keyLength == (uint64_t)literalLength &&
                     memcmp(key, literal, literalLength) == 0);
}

static int32_t markSeen(uint32_t *seen, uint32_t bit)
{
    if (*seen & bit) {
        return PARSER_ERROR_DUPLICATE_KEY;
    }
    *seen |= bit;
    return PARSER_OK;
}

static int32_t readOwnedString(parserContext *context, parserCursor *cursor, char **out)
{
    const uint8_t *data;
    uint64_t       length;

    PARSER_TRY(readString(cursor, &data, &length));
    return copyString(context, data, length, out);
}

/*
 * A path component must be usable as a single file name, and must not let
 * a hostile torrent escape the download directory.
 */
static uint8_t isSafeComponent(const uint8_t *data, uint64_t length)
{
    if (length == 0) {
        return 0;
    }
    if ((length == 1 && data[0] == '.') ||
        (length == 2 && data[0] == '.' && data[1] == '.')) {
        return 0;
    }
    if (memchr(data, '/', (size_t)length) != NULL ||
        memchr(data, '\0', (size_t)length) != NULL) {
        return 0;
    }
    return 1;
}

static int32_t appendPathComponent(parserContext *context, parserFile *file,
                                   const uint8_t *data, uint64_t length)
{
    parserPathComponent *component;
    int32_t              result;

    if (!isSafeComponent(data, length)) {
        return PARSER_ERROR_INVALID_FIELD;
    }
    component = allocate(context, sizeof(*component));
    if (component == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }
    result = copyString(context, data, length, &component->name);
    if (result != PARSER_OK) {
        release(context, component);
        return result;
    }
    TAILQ_INSERT_TAIL(&file->path, component, entries);
    file->pathCount++;
    return PARSER_OK;
}

/* Empty URLs are silently ignored (some tools emit url-list ""). */
static int32_t appendUrl(parserContext *context, struct parserUrlList *list,
                         uint32_t *count, const uint8_t *data, uint64_t length)
{
    parserUrl *url;
    int32_t    result;

    if (length == 0) {
        return PARSER_OK;
    }
    url = allocate(context, sizeof(*url));
    if (url == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }
    result = copyString(context, data, length, &url->url);
    if (result != PARSER_OK) {
        release(context, url);
        return result;
    }
    TAILQ_INSERT_TAIL(list, url, entries);
    (*count)++;
    return PARSER_OK;
}

static parserFile *appendFile(parserContext *context, parserTorrent *torrent)
{
    parserFile *file = allocate(context, sizeof(*file));

    if (file != NULL) {
        TAILQ_INIT(&file->path);
        TAILQ_INSERT_TAIL(&torrent->files, file, entries);
        torrent->fileCount++;
    }
    return file;
}

/* ======================================================================== */
/* Torrent structure parsing                                                */
/* ======================================================================== */

/* A bencoded list of strings. */
static int32_t parseUrlList(parserContext *context, parserCursor *cursor,
                            struct parserUrlList *list, uint32_t *count)
{
    const uint8_t *data;
    uint64_t       length;
    uint8_t        more;

    PARSER_TRY(containerBegin(cursor, 'l'));
    for (;;) {
        PARSER_TRY(containerNext(cursor, &more));
        if (!more) {
            break;
        }
        PARSER_TRY(readString(cursor, &data, &length));
        PARSER_TRY(appendUrl(context, list, count, data, length));
    }
    return PARSER_OK;
}

/* announce-list: a list of tiers, each a list of URL strings (BEP 12). */
static int32_t parseAnnounceList(parserContext *context, parserCursor *cursor,
                                 parserTorrent *torrent)
{
    parserTier *tier;
    uint8_t     more;

    PARSER_TRY(containerBegin(cursor, 'l'));
    for (;;) {
        PARSER_TRY(containerNext(cursor, &more));
        if (!more) {
            break;
        }

        tier = allocate(context, sizeof(*tier));
        if (tier == NULL) {
            return PARSER_ERROR_NO_MEMORY;
        }
        TAILQ_INIT(&tier->urls);
        /* Link first so that parserFree() cleans up if parsing fails. */
        TAILQ_INSERT_TAIL(&torrent->announceList, tier, entries);
        torrent->tierCount++;

        PARSER_TRY(parseUrlList(context, cursor, &tier->urls, &tier->urlCount));

        if (tier->urlCount == 0) {
            TAILQ_REMOVE(&torrent->announceList, tier, entries);
            torrent->tierCount--;
            release(context, tier);
        }
    }
    return PARSER_OK;
}

/* url-list: either a single string or a list of strings (BEP 19). */
static int32_t parseWebSeeds(parserContext *context, parserCursor *cursor,
                             parserTorrent *torrent)
{
    const uint8_t *data;
    uint64_t       length;

    if (isDigit(cursorPeek(cursor))) {
        PARSER_TRY(readString(cursor, &data, &length));
        return appendUrl(context, &torrent->webSeeds, &torrent->webSeedCount,
                         data, length);
    }
    return parseUrlList(context, cursor, &torrent->webSeeds, &torrent->webSeedCount);
}

static int32_t parsePath(parserContext *context, parserCursor *cursor, parserFile *file)
{
    const uint8_t *data;
    uint64_t       length;
    uint8_t        more;

    PARSER_TRY(containerBegin(cursor, 'l'));
    for (;;) {
        PARSER_TRY(containerNext(cursor, &more));
        if (!more) {
            break;
        }
        PARSER_TRY(readString(cursor, &data, &length));
        PARSER_TRY(appendPathComponent(context, file, data, length));
    }
    return PARSER_OK;
}

/* One entry of info.files: { length, path, [attr] } */
static int32_t parseFileEntry(parserContext *context, parserCursor *cursor,
                              parserFile *file)
{
    const uint8_t *key;
    const uint8_t *data;
    uint64_t       keyLength;
    uint64_t       length;
    int64_t        value;
    uint32_t       seen = 0;
    uint8_t        more;

    PARSER_TRY(containerBegin(cursor, 'd'));
    for (;;) {
        PARSER_TRY(containerNext(cursor, &more));
        if (!more) {
            break;
        }
        PARSER_TRY(readString(cursor, &key, &keyLength));

        if (keyEquals(key, keyLength, "length")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_LENGTH));
            PARSER_TRY(readInteger(cursor, &value));
            if (value < 0) {
                return PARSER_ERROR_INVALID_FIELD;
            }
            file->length = (uint64_t)value;
        } else if (keyEquals(key, keyLength, "path")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_PATH));
            PARSER_TRY(parsePath(context, cursor, file));
        } else if (keyEquals(key, keyLength, "attr")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_ATTR));
            PARSER_TRY(readString(cursor, &data, &length));
            if (length > 0 && memchr(data, 'p', (size_t)length) != NULL) {
                file->isPadding = 1;
            }
        } else {
            PARSER_TRY(skipValue(cursor));
        }
    }

    if (!(seen & PARSER_KEY_LENGTH) || !(seen & PARSER_KEY_PATH)) {
        return PARSER_ERROR_MISSING_FIELD;
    }
    if (file->pathCount == 0) {
        return PARSER_ERROR_INVALID_FIELD;
    }
    return PARSER_OK;
}

static int32_t parseFiles(parserContext *context, parserCursor *cursor,
                          parserTorrent *torrent)
{
    parserFile *file;
    uint8_t     more;

    PARSER_TRY(containerBegin(cursor, 'l'));
    for (;;) {
        PARSER_TRY(containerNext(cursor, &more));
        if (!more) {
            break;
        }
        file = appendFile(context, torrent);
        if (file == NULL) {
            return PARSER_ERROR_NO_MEMORY;
        }
        PARSER_TRY(parseFileEntry(context, cursor, file));
    }
    return PARSER_OK;
}

static int32_t parseInfo(parserContext *context, parserCursor *cursor,
                         parserTorrent *torrent)
{
    const uint8_t *key;
    const uint8_t *data;
    uint64_t       keyLength;
    uint64_t       length;
    uint64_t       total;
    uint64_t       expectedPieces;
    uint64_t       singleLength = 0;
    int64_t        value;
    uint32_t       seen = 0;
    uint8_t        more;
    parserFile    *file;

    PARSER_TRY(containerBegin(cursor, 'd'));
    for (;;) {
        PARSER_TRY(containerNext(cursor, &more));
        if (!more) {
            break;
        }
        PARSER_TRY(readString(cursor, &key, &keyLength));

        if (keyEquals(key, keyLength, "name")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_NAME));
            PARSER_TRY(readString(cursor, &data, &length));
            if (!isSafeComponent(data, length)) {
                return PARSER_ERROR_INVALID_FIELD;
            }
            PARSER_TRY(copyString(context, data, length, &torrent->name));
        } else if (keyEquals(key, keyLength, "piece length")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_PIECE_LENGTH));
            PARSER_TRY(readInteger(cursor, &value));
            if (value <= 0) {
                return PARSER_ERROR_INVALID_FIELD;
            }
            torrent->pieceLength = (uint64_t)value;
        } else if (keyEquals(key, keyLength, "pieces")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_PIECES));
            PARSER_TRY(readString(cursor, &data, &length));
            if (length % PARSER_SHA1_LENGTH != 0 ||
                length / PARSER_SHA1_LENGTH > UINT32_MAX) {
                return PARSER_ERROR_INVALID_FIELD;
            }
            if (length > 0) {
                torrent->pieces = allocate(context, length);
                if (torrent->pieces == NULL) {
                    return PARSER_ERROR_NO_MEMORY;
                }
                memcpy(torrent->pieces, data, (size_t)length);
            }
            torrent->pieceCount = (uint32_t)(length / PARSER_SHA1_LENGTH);
        } else if (keyEquals(key, keyLength, "private")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_PRIVATE));
            PARSER_TRY(readInteger(cursor, &value));
            torrent->isPrivate = (uint8_t)(value == 1);
        } else if (keyEquals(key, keyLength, "length")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_LENGTH));
            PARSER_TRY(readInteger(cursor, &value));
            if (value < 0) {
                return PARSER_ERROR_INVALID_FIELD;
            }
            singleLength = (uint64_t)value;
        } else if (keyEquals(key, keyLength, "files")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_FILES));
            PARSER_TRY(parseFiles(context, cursor, torrent));
        } else {
            PARSER_TRY(skipValue(cursor));
        }
    }

    /* Validation */
    if (!(seen & PARSER_KEY_NAME) || !(seen & PARSER_KEY_PIECE_LENGTH) ||
        !(seen & PARSER_KEY_PIECES)) {
        return PARSER_ERROR_MISSING_FIELD;
    }
    if ((seen & PARSER_KEY_LENGTH) && (seen & PARSER_KEY_FILES)) {
        return PARSER_ERROR_INVALID_FIELD;          /* must be one or the other */
    }
    if (!(seen & PARSER_KEY_LENGTH) && !(seen & PARSER_KEY_FILES)) {
        return PARSER_ERROR_MISSING_FIELD;
    }

    if (seen & PARSER_KEY_LENGTH) {
        /* Single-file: present as a one-entry file list named after the torrent. */
        file = appendFile(context, torrent);
        if (file == NULL) {
            return PARSER_ERROR_NO_MEMORY;
        }
        file->length = singleLength;
        PARSER_TRY(appendPathComponent(context, file, (const uint8_t *)torrent->name,
                                       strlen(torrent->name)));
        torrent->isMultiFile = 0;
    } else {
        if (torrent->fileCount == 0) {
            return PARSER_ERROR_INVALID_FIELD;
        }
        torrent->isMultiFile = 1;
    }

    total = 0;
    TAILQ_FOREACH(file, &torrent->files, entries) {
        if (file->length > UINT64_MAX - total) {
            return PARSER_ERROR_OVERFLOW;
        }
        file->offset = total;
        total += file->length;
    }
    torrent->totalLength = total;

    expectedPieces = total / torrent->pieceLength +
                     (uint64_t)(total % torrent->pieceLength != 0);
    if (expectedPieces != (uint64_t)torrent->pieceCount) {
        return PARSER_ERROR_INVALID_FIELD;
    }
    return PARSER_OK;
}

static int32_t parseRoot(parserContext *context, parserCursor *cursor,
                         parserTorrent *torrent)
{
    const uint8_t *key;
    const uint8_t *data;
    uint64_t       keyLength;
    uint64_t       length;
    uint64_t       infoStart;
    uint32_t       seen = 0;
    uint8_t        more;

    PARSER_TRY(containerBegin(cursor, 'd'));
    for (;;) {
        PARSER_TRY(containerNext(cursor, &more));
        if (!more) {
            break;
        }
        PARSER_TRY(readString(cursor, &key, &keyLength));

        if (keyEquals(key, keyLength, "announce")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_ANNOUNCE));
            PARSER_TRY(readString(cursor, &data, &length));
            if (length > 0) {
                PARSER_TRY(copyString(context, data, length, &torrent->announce));
            }
        } else if (keyEquals(key, keyLength, "announce-list")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_ANNOUNCE_LIST));
            PARSER_TRY(parseAnnounceList(context, cursor, torrent));
        } else if (keyEquals(key, keyLength, "comment")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_COMMENT));
            PARSER_TRY(readOwnedString(context, cursor, &torrent->comment));
        } else if (keyEquals(key, keyLength, "created by")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_CREATED_BY));
            PARSER_TRY(readOwnedString(context, cursor, &torrent->createdBy));
        } else if (keyEquals(key, keyLength, "creation date")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_CREATION_DATE));
            PARSER_TRY(readInteger(cursor, &torrent->creationDate));
            torrent->hasCreationDate = 1;
        } else if (keyEquals(key, keyLength, "encoding")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_ENCODING));
            PARSER_TRY(readOwnedString(context, cursor, &torrent->encoding));
        } else if (keyEquals(key, keyLength, "info")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_INFO));
            infoStart = cursor->position;
            PARSER_TRY(parseInfo(context, cursor, torrent));
            /* The info hash is the SHA-1 of the raw bytes exactly as they appear. */
            torrent->infoOffset = infoStart;
            torrent->infoLength = cursor->position - infoStart;
            parserSha1(cursor->buffer + infoStart, torrent->infoLength, torrent->infoHash);
        } else if (keyEquals(key, keyLength, "url-list")) {
            PARSER_TRY(markSeen(&seen, PARSER_KEY_URL_LIST));
            PARSER_TRY(parseWebSeeds(context, cursor, torrent));
        } else {
            PARSER_TRY(skipValue(cursor));
        }
    }

    if (!(seen & PARSER_KEY_INFO)) {
        return PARSER_ERROR_MISSING_FIELD;
    }
    return PARSER_OK;
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t parserInit(parserContext *context, const parserAllocator *allocator)
{
    if (context == NULL || allocator == NULL ||
        allocator->realloc == NULL || allocator->free == NULL) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    memset(context, 0, sizeof(*context));
    context->allocator = *allocator;
    context->magic = PARSER_CONTEXT_MAGIC;
    return PARSER_OK;
}

int32_t parserParse(parserContext *context, const uint8_t *buffer,
                    uint64_t length, parserTorrent *torrent)
{
    parserCursor cursor;
    int32_t      result;

    if (torrent != NULL) {
        resetTorrent(torrent);
    }
    if (context == NULL || buffer == NULL || torrent == NULL || length == 0) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    if (context->magic != PARSER_CONTEXT_MAGIC) {
        return PARSER_ERROR_NOT_INITIALISED;
    }

    context->errorOffset = 0;
    cursor.buffer = buffer;
    cursor.length = length;
    cursor.position = 0;
    cursor.depth = 0;

    result = parseRoot(context, &cursor, torrent);
    if (result != PARSER_OK) {
        context->errorOffset = cursor.position;
        parserFree(context, torrent);
    }
    return result;
}

void parserFree(parserContext *context, parserTorrent *torrent)
{
    parserTier *tier;
    parserFile *file;

    if (context == NULL || torrent == NULL || context->magic != PARSER_CONTEXT_MAGIC) {
        return;
    }

    release(context, torrent->announce);
    release(context, torrent->comment);
    release(context, torrent->createdBy);
    release(context, torrent->encoding);
    release(context, torrent->name);
    release(context, torrent->pieces);

    while ((tier = TAILQ_FIRST(&torrent->announceList)) != NULL) {
        TAILQ_REMOVE(&torrent->announceList, tier, entries);
        freeUrlList(context, &tier->urls);
        release(context, tier);
    }

    freeUrlList(context, &torrent->webSeeds);

    while ((file = TAILQ_FIRST(&torrent->files)) != NULL) {
        TAILQ_REMOVE(&torrent->files, file, entries);
        freePathList(context, &file->path);
        release(context, file);
    }

    resetTorrent(torrent);
}

const char *parserErrorString(int32_t error)
{
    switch (error) {
    case PARSER_OK:                     return "Success";
    case PARSER_ERROR_INVALID_ARGUMENT: return "Invalid argument";
    case PARSER_ERROR_NOT_INITIALISED:  return "Parser not initialised";
    case PARSER_ERROR_NO_MEMORY:        return "Out of memory";
    case PARSER_ERROR_TRUNCATED:        return "Unexpected end of data";
    case PARSER_ERROR_SYNTAX:           return "Malformed bencode";
    case PARSER_ERROR_OVERFLOW:         return "Integer overflow";
    case PARSER_ERROR_TOO_DEEP:         return "Nesting too deep";
    case PARSER_ERROR_DUPLICATE_KEY:    return "Duplicate dictionary key";
    case PARSER_ERROR_MISSING_FIELD:    return "Required field missing";
    case PARSER_ERROR_INVALID_FIELD:    return "Invalid field value";
    default:                            return "Unknown error";
    }
}
