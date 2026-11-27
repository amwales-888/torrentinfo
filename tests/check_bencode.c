/*
 * check_bencode.c - the bencode writer: exact output, sticky errors, and a
 * fuzz round trip through parserCursor.
 */
#include <string.h>

#include "check.h"
#include "bencode.h"

static int sameBytes(const bencodeWriter *w, const char *expected)
{
    size_t length = strlen(expected);

    return w->error == BENCODE_OK && w->length == length && memcmp(w->data, expected, length) == 0;
}

static void exactOutput(void)
{
    uint8_t       buffer[256];
    bencodeWriter w;

    bencodeWriterInitFixed(&w, buffer, sizeof(buffer));
    bencodeDictBegin(&w);
    bencodeText(&w, "a");
    bencodeInteger(&w, -5);
    bencodeText(&w, "b");
    bencodeListBegin(&w);
    bencodeString(&w, "x\0z", 3);
    bencodeInteger(&w, 0);
    bencodeEnd(&w);
    bencodeText(&w, "c");
    bencodeUnsigned(&w, UINT64_MAX);
    bencodeText(&w, "d");
    bencodeInteger(&w, INT64_MIN);
    bencodeText(&w, "e");
    bencodeText(&w, "");
    bencodeText(&w, "f");
    bencodeStringHeader(&w, 4);
    bencodeRaw(&w, "ab", 2);
    bencodeRaw(&w, "cd", 2);
    bencodeEnd(&w);
    CHECK(w.error == BENCODE_OK);
    CHECK(w.length == strlen("d1:ai-5e1:bl3:x") + 2 + strlen("i0ee1:ci18446744073709551615e"
                             "1:di-9223372036854775808e1:e0:1:f4:abcde"));
    CHECK(memcmp(w.data, "d1:ai-5e1:bl3:x\0zi0ee1:ci18446744073709551615e"
                         "1:di-9223372036854775808e1:e0:1:f4:abcde", (size_t)w.length) == 0);
}

static void fixedBufferFull(void)
{
    uint8_t       buffer[8];
    bencodeWriter w;

    memset(buffer, '#', sizeof(buffer));
    bencodeWriterInitFixed(&w, buffer, 6);
    bencodeText(&w, "abc");                         /* 3:abc: 5 bytes */
    CHECK(sameBytes(&w, "3:abc"));
    bencodeText(&w, "de");                          /* would need 4 more */
    CHECK(w.error == BENCODE_ERROR_NO_SPACE);
    bencodeEnd(&w);                                 /* sticky: ignored */
    CHECK(w.error == BENCODE_ERROR_NO_SPACE);
    CHECK(buffer[6] == '#' && buffer[7] == '#');    /* nothing past the end */

    bencodeWriterInitFixed(&w, NULL, 100);          /* no buffer: nothing fits */
    bencodeEnd(&w);
    CHECK(w.error == BENCODE_ERROR_NO_SPACE);
}

static void growable(void)
{
    bencodeWriter w;
    uint8_t      *data;
    uint64_t      length;
    uint32_t      i;
    parserCursor  cursor;
    uint8_t       more;
    uint32_t      seen = 0;

    bencodeWriterInit(&w, &checkAllocator);
    bencodeListBegin(&w);
    for (i = 0; i < 20000; i++) {
        bencodeUnsigned(&w, i);
    }
    bencodeEnd(&w);
    CHECK(bencodeWriterTake(&w, &data, &length) == BENCODE_OK);
    CHECK(data != NULL && w.data == NULL);

    memset(&cursor, 0, sizeof(cursor));
    cursor.buffer = data;
    cursor.length = length;
    CHECK(parserContainerBegin(&cursor, 'l') == PARSER_OK);
    while (parserContainerNext(&cursor, &more) == PARSER_OK && more) {
        int64_t value;

        if (parserReadInteger(&cursor, &value) != PARSER_OK || value != (int64_t)seen) {
            break;
        }
        seen++;
    }
    CHECK(seen == 20000);
    CHECK(cursor.position == length);
    checkAllocator.free(data);

    /* Out of memory part way: the error sticks and nothing leaks. */
    bencodeWriterInit(&w, &checkAllocator);
    checkFailAllocationsAfter(3);
    for (i = 0; i < 20000; i++) {
        bencodeText(&w, "some text to fill the buffer");
    }
    checkFailAllocationsAfter(-1);
    CHECK(w.error == PARSER_ERROR_NO_MEMORY);
    CHECK(bencodeWriterTake(&w, &data, &length) == PARSER_ERROR_NO_MEMORY);
    CHECK(data == NULL && length == 0);
}

/* ------------------------------------------------------------------------ */
/* Fuzz: random values written, then walked with parserCursor, comparing    */
/* each value with a replay of the same random sequence.                    */
/* ------------------------------------------------------------------------ */

typedef struct replay {
    uint32_t seed;
} replay;

static uint32_t nextOf(replay *r)
{
    r->seed ^= r->seed << 13;
    r->seed ^= r->seed >> 17;
    r->seed ^= r->seed << 5;
    return r->seed;
}

static void randomValue(bencodeWriter *w, replay *r, uint32_t depth)
{
    uint32_t kind = nextOf(r) % ((depth < 6) ? 4u : 2u);
    uint32_t i;

    if (kind == 0) {
        int64_t value = (int64_t)(((uint64_t)nextOf(r) << 32) | nextOf(r));

        bencodeInteger(w, value);
    } else if (kind == 1) {
        uint8_t  bytes[40];
        uint32_t length = nextOf(r) % sizeof(bytes);

        for (i = 0; i < length; i++) {
            bytes[i] = (uint8_t)nextOf(r);
        }
        bencodeString(w, bytes, length);
    } else {
        uint32_t count = nextOf(r) % 6u;

        if (kind == 2) {
            bencodeListBegin(w);
        } else {
            bencodeDictBegin(w);
        }
        for (i = 0; i < count; i++) {
            if (kind == 3) {
                char key[8];

                key[0] = (char)('a' + i);       /* sorted, unique */
                key[1] = '\0';
                bencodeText(w, key);
            }
            randomValue(w, r, depth + 1);
        }
        bencodeEnd(w);
    }
}

static int readValue(parserCursor *c, replay *r, uint32_t depth)
{
    uint32_t kind = nextOf(r) % ((depth < 6) ? 4u : 2u);
    uint32_t i;
    uint8_t  more;

    if (kind == 0) {
        int64_t expected = (int64_t)(((uint64_t)nextOf(r) << 32) | nextOf(r));
        int64_t value;

        return parserReadInteger(c, &value) == PARSER_OK && value == expected;
    } else if (kind == 1) {
        uint8_t        bytes[40];
        uint32_t       length = nextOf(r) % sizeof(bytes);
        const uint8_t *data;
        uint64_t       dataLength;

        for (i = 0; i < length; i++) {
            bytes[i] = (uint8_t)nextOf(r);
        }
        return parserReadString(c, &data, &dataLength) == PARSER_OK && dataLength == length &&
               (length == 0 || memcmp(data, bytes, length) == 0);
    } else {
        uint32_t count = nextOf(r) % 6u;

        if (parserContainerBegin(c, (kind == 2) ? 'l' : 'd') != PARSER_OK) {
            return 0;
        }
        for (i = 0; i < count; i++) {
            if (parserContainerNext(c, &more) != PARSER_OK || !more) {
                return 0;
            }
            if (kind == 3) {
                const uint8_t *key;
                uint64_t       keyLength;

                if (parserReadString(c, &key, &keyLength) != PARSER_OK || keyLength != 1 ||
                    key[0] != (uint8_t)('a' + i)) {
                    return 0;
                }
            }
            if (!readValue(c, r, depth + 1)) {
                return 0;
            }
        }
        return parserContainerNext(c, &more) == PARSER_OK && !more;
    }
}

static void fuzzRoundTrip(void)
{
    uint32_t round;
    uint32_t good = 0;

    for (round = 0; round < 2000; round++) {
        bencodeWriter w;
        replay        writing = { checkRandom() | 1u };
        replay        reading = writing;
        parserCursor  cursor;

        bencodeWriterInit(&w, &checkAllocator);
        randomValue(&w, &writing, 0);
        if (w.error != BENCODE_OK) {
            bencodeWriterFree(&w);
            continue;
        }
        memset(&cursor, 0, sizeof(cursor));
        cursor.buffer = w.data;
        cursor.length = w.length;
        if (readValue(&cursor, &reading, 0) && cursor.position == w.length) {
            good++;
        }
        /* The generic skipper must agree on where the value ends. */
        memset(&cursor, 0, sizeof(cursor));
        cursor.buffer = w.data;
        cursor.length = w.length;
        if (parserSkipValue(&cursor) != PARSER_OK || cursor.position != w.length) {
            good = 0;
        }
        bencodeWriterFree(&w);
    }
    CHECK(good == 2000);
}

void checkBencode(void)
{
    exactOutput();
    fixedBufferFull();
    growable();
    fuzzRoundTrip();
}
