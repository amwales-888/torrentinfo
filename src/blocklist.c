/*
 * blocklist.c - IP address blocklists. See blocklist.h.
 *
 * Parsing collects every range into growing arrays, which are then sorted
 * (qsort) and merged, overlapping or adjacent ranges becoming one. Lookups
 * binary-search for the last range starting at or below the address.
 */
#include <stdlib.h>
#include <string.h>

#include "blocklist.h"
#include "platform.h"

typedef struct range4 {
    uint32_t first;
    uint32_t last;
} range4;

typedef struct range6 {
    uint8_t first[16];
    uint8_t last[16];
} range6;

struct blocklist {
    range4  *v4;
    uint32_t count4;
    range6  *v6;
    uint32_t count6;
};

typedef struct builder {
    parserContext *context;
    range4        *v4;
    uint32_t       count4;
    uint32_t       capacity4;
    range6        *v6;
    uint32_t       count6;
    uint32_t       capacity6;
    int            failed;
} builder;

/* ------------------------------------------------------------------------ */
/* Reading addresses                                                        */
/* ------------------------------------------------------------------------ */

static int isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Trim [*start, *end). */
static void trim(const char **start, const char **end)
{
    while (*start < *end && isSpace(**start)) {
        (*start)++;
    }
    while (*end > *start && isSpace((*end)[-1])) {
        (*end)--;
    }
}

/* Dotted IPv4, leading zeros allowed (read as decimal). */
static int parse4(const char *start, const char *end, uint32_t *out)
{
    uint32_t value = 0;
    uint32_t parts = 0;

    while (parts < 4) {
        uint32_t part = 0;
        int      digits = 0;

        while (start < end && *start >= '0' && *start <= '9' && digits < 3) {
            part = part * 10u + (uint32_t)(*start - '0');
            start++;
            digits++;
        }
        if (digits == 0 || part > 255u) {
            return 0;
        }
        value = (value << 8) | part;
        parts++;
        if (parts < 4) {
            if (start >= end || *start != '.') {
                return 0;
            }
            start++;
        }
    }
    if (start != end) {
        return 0;
    }
    *out = value;
    return 1;
}

static int parse6(const char *start, const char *end, uint8_t out[16])
{
    char text[64];

    if ((size_t)(end - start) >= sizeof(text)) {
        return 0;
    }
    memcpy(text, start, (size_t)(end - start));
    text[end - start] = '\0';
    return uv_inet_pton(AF_INET6, text, out) == 0;
}

/* An IPv6 address that is really IPv4 (::ffff:a.b.c.d). */
static int mapped4(const uint8_t address[16], uint32_t *out)
{
    static const uint8_t prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

    if (memcmp(address, prefix, sizeof(prefix)) != 0) {
        return 0;
    }
    *out = ((uint32_t)address[12] << 24) | ((uint32_t)address[13] << 16) |
           ((uint32_t)address[14] << 8) | address[15];
    return 1;
}

static void add4(builder *b, uint32_t first, uint32_t last)
{
    if (b->failed) {
        return;
    }
    if (first > last) {
        uint32_t swap = first;

        first = last;
        last = swap;
    }
    if (b->count4 == b->capacity4) {
        uint32_t capacity = (b->capacity4 == 0) ? 1024u : b->capacity4 * 2u;
        range4  *grown = b->context->allocator.realloc(b->v4, sizeof(range4) * capacity);

        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->v4 = grown;
        b->capacity4 = capacity;
    }
    b->v4[b->count4].first = first;
    b->v4[b->count4].last = last;
    b->count4++;
}

static void add6(builder *b, const uint8_t first[16], const uint8_t last[16])
{
    uint32_t a;
    uint32_t z;

    if (b->failed) {
        return;
    }
    if (mapped4(first, &a) && mapped4(last, &z)) {
        add4(b, a, z);                              /* IPv4 in IPv6 clothing */
        return;
    }
    if (b->count6 == b->capacity6) {
        uint32_t capacity = (b->capacity6 == 0) ? 64u : b->capacity6 * 2u;
        range6  *grown = b->context->allocator.realloc(b->v6, sizeof(range6) * capacity);

        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->v6 = grown;
        b->capacity6 = capacity;
    }
    if (memcmp(first, last, 16) <= 0) {
        memcpy(b->v6[b->count6].first, first, 16);
        memcpy(b->v6[b->count6].last, last, 16);
    } else {
        memcpy(b->v6[b->count6].first, last, 16);
        memcpy(b->v6[b->count6].last, first, 16);
    }
    b->count6++;
}

/* One address, a CIDR block, or "first - last". 1 if understood. */
static int parseEntry(builder *b, const char *start, const char *end)
{
    const char *dash;
    const char *slash;
    uint32_t    a;
    uint32_t    z;
    uint8_t     a6[16];
    uint8_t     z6[16];

    trim(&start, &end);
    if (start == end) {
        return 0;
    }
    dash = memchr(start, '-', (size_t)(end - start));
    if (dash != NULL) {
        const char *firstEnd = dash;
        const char *lastStart = dash + 1;

        trim(&start, &firstEnd);
        trim(&lastStart, &end);
        if (parse4(start, firstEnd, &a) && parse4(lastStart, end, &z)) {
            add4(b, a, z);
            return 1;
        }
        if (parse6(start, firstEnd, a6) && parse6(lastStart, end, z6)) {
            add6(b, a6, z6);
            return 1;
        }
        return 0;
    }
    slash = memchr(start, '/', (size_t)(end - start));
    if (slash != NULL) {
        const char *bitsText = slash + 1;
        long        bits = 0;
        int         digits = 0;

        while (bitsText < end && *bitsText >= '0' && *bitsText <= '9' && digits < 3) {
            bits = bits * 10 + (*bitsText - '0');
            bitsText++;
            digits++;
        }
        if (digits == 0 || bitsText != end) {
            return 0;
        }
        if (parse4(start, slash, &a) && bits <= 32) {
            uint32_t mask = (bits == 0) ? 0u : 0xffffffffu << (32 - bits);

            add4(b, a & mask, (a & mask) | ~mask);
            return 1;
        }
        if (parse6(start, slash, a6) && bits <= 128) {
            int i;

            memcpy(z6, a6, 16);
            for (i = 0; i < 128; i++) {
                if (i >= bits) {
                    a6[i / 8] &= (uint8_t)~(0x80u >> (i % 8));
                    z6[i / 8] |= (uint8_t)(0x80u >> (i % 8));
                }
            }
            add6(b, a6, z6);
            return 1;
        }
        return 0;
    }
    if (parse4(start, end, &a)) {
        add4(b, a, a);
        return 1;
    }
    if (parse6(start, end, a6)) {
        add6(b, a6, a6);
        return 1;
    }
    return 0;
}

/* 1 entry read, 2 an ipfilter.dat line that allows (skipped), 0 not understood. */
static int parseLine(builder *b, const char *start, const char *end)
{
    const char *comma;
    const char *colon;

    /* ipfilter.dat: "range , level , description" */
    comma = memchr(start, ',', (size_t)(end - start));
    if (comma != NULL) {
        const char *levelStart = comma + 1;
        const char *levelEnd = memchr(levelStart, ',', (size_t)(end - levelStart));
        long        level = 0;

        if (levelEnd == NULL) {
            levelEnd = end;
        }
        trim(&levelStart, &levelEnd);
        while (levelStart < levelEnd && *levelStart >= '0' && *levelStart <= '9') {
            level = level * 10 + (*levelStart++ - '0');
            if (level > 1000) {
                break;
            }
        }
        if (levelStart == levelEnd && level >= 128) {
            return 2;
        }
        return parseEntry(b, start, comma);
    }
    if (parseEntry(b, start, end)) {
        return 1;
    }
    /* P2P: "description:range" (the description may contain colons). */
    colon = end;
    while (colon > start && colon[-1] != ':') {
        colon--;
    }
    if (colon > start && parseEntry(b, colon, end)) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Sorting and merging                                                      */
/* ------------------------------------------------------------------------ */

static int compare4(const void *a, const void *b)
{
    const range4 *x = a;
    const range4 *y = b;

    return (x->first < y->first) ? -1 : (x->first > y->first) ? 1 : 0;
}

static int compare6(const void *a, const void *b)
{
    return memcmp(((const range6 *)a)->first, ((const range6 *)b)->first, 16);
}

/* Is 'b' the address right after 'a' (no gap between ranges)? */
static int follows6(const uint8_t a[16], const uint8_t b[16])
{
    uint8_t next[16];
    int     i;

    memcpy(next, a, 16);
    for (i = 15; i >= 0; i--) {
        if (++next[i] != 0) {
            break;
        }
        if (i == 0) {
            return 0;                               /* a was the last address */
        }
    }
    return memcmp(next, b, 16) == 0;
}

static uint32_t merge4(range4 *ranges, uint32_t count)
{
    uint32_t out = 0;
    uint32_t i;

    if (count == 0) {
        return 0;
    }
    qsort(ranges, count, sizeof(*ranges), compare4);
    for (i = 1; i < count; i++) {
        range4 *current = &ranges[out];

        if (ranges[i].first <= current->last ||
            (current->last != 0xffffffffu && ranges[i].first == current->last + 1u)) {
            if (ranges[i].last > current->last) {
                current->last = ranges[i].last;
            }
        } else {
            ranges[++out] = ranges[i];
        }
    }
    return out + 1u;
}

static uint32_t merge6(range6 *ranges, uint32_t count)
{
    uint32_t out = 0;
    uint32_t i;

    if (count == 0) {
        return 0;
    }
    qsort(ranges, count, sizeof(*ranges), compare6);
    for (i = 1; i < count; i++) {
        range6 *current = &ranges[out];

        if (memcmp(ranges[i].first, current->last, 16) <= 0 ||
            follows6(current->last, ranges[i].first)) {
            if (memcmp(ranges[i].last, current->last, 16) > 0) {
                memcpy(current->last, ranges[i].last, 16);
            }
        } else {
            ranges[++out] = ranges[i];
        }
    }
    return out + 1u;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t blocklistParse(parserContext *context, const char *text, size_t length,
                       blocklist **list, blocklistStats *stats)
{
    blocklistStats counts;
    blocklist     *result;
    builder        b;
    const char    *at = text;
    const char    *end = text + length;
    uint32_t       i;

    if (list != NULL) {
        *list = NULL;
    }
    if (context == NULL || (text == NULL && length > 0) || list == NULL) {
        return BLOCKLIST_EINVAL;
    }
    memset(&counts, 0, sizeof(counts));
    memset(&b, 0, sizeof(b));
    b.context = context;
    while (at < end && !b.failed) {
        const char *lineEnd = memchr(at, '\n', (size_t)(end - at));
        const char *start = at;
        const char *stop;
        int         result4;

        if (lineEnd == NULL) {
            lineEnd = end;
        }
        stop = lineEnd;
        at = (lineEnd < end) ? lineEnd + 1 : end;
        counts.lines++;
        trim(&start, &stop);
        if (start == stop || *start == '#' || *start == ';' ||
            (stop - start >= 2 && start[0] == '/' && start[1] == '/')) {
            continue;
        }
        result4 = parseLine(&b, start, stop);
        if (result4 == 1) {
            counts.entries++;
        } else if (result4 == 2) {
            counts.allowed++;
        } else {
            counts.rejected++;
            if (counts.firstRejected == 0) {
                counts.firstRejected = counts.lines;
            }
        }
    }
    result = b.failed ? NULL : context->allocator.realloc(NULL, sizeof(*result));
    if (result == NULL) {
        if (b.v4 != NULL) {
            context->allocator.free(b.v4);
        }
        if (b.v6 != NULL) {
            context->allocator.free(b.v6);
        }
        return BLOCKLIST_ENOMEM;
    }
    result->v4 = b.v4;
    result->count4 = merge4(b.v4, b.count4);
    result->v6 = b.v6;
    result->count6 = merge6(b.v6, b.count6);
    counts.ranges4 = result->count4;
    counts.ranges6 = result->count6;
    for (i = 0; i < result->count4; i++) {
        counts.addresses4 += (uint64_t)(result->v4[i].last - result->v4[i].first) + 1u;
    }
    if (stats != NULL) {
        *stats = counts;
    }
    *list = result;
    return BLOCKLIST_OK;
}

int32_t blocklistLoad(parserContext *context, const char *path, blocklist **list,
                      blocklistStats *stats, int32_t *systemError)
{
    platformFile file;
    char        *text;
    uint64_t     size;
    uint8_t      isRegular;
    uint32_t     got;
    uint64_t     done = 0;
    int32_t      result;

    if (list != NULL) {
        *list = NULL;
    }
    if (systemError != NULL) {
        *systemError = 0;
    }
    if (context == NULL || path == NULL || list == NULL) {
        return BLOCKLIST_EINVAL;
    }
    result = platformFileStat(path, &size, &isRegular);
    if (result == 0 && (!isRegular || size > BLOCKLIST_MAX_FILE)) {
        result = UV_EINVAL;
    }
    if (result == 0) {
        result = platformFileOpenRead(path, &file);
    }
    if (result != 0) {
        if (systemError != NULL) {
            *systemError = result;
        }
        return BLOCKLIST_EIO;
    }
    text = context->allocator.realloc(NULL, (size_t)size + 1u);
    if (text == NULL) {
        platformFileClose(&file);
        return BLOCKLIST_ENOMEM;
    }
    while (done < size) {
        uint64_t left = size - done;

        result = platformFileRead(&file, text + done,
                                  (uint32_t)((left > 0x40000000u) ? 0x40000000u : left), &got);
        if (result != 0 || got == 0) {
            break;
        }
        done += got;
    }
    platformFileClose(&file);
    if (done != size) {
        context->allocator.free(text);
        if (systemError != NULL) {
            *systemError = (result != 0) ? result : UV_EIO;
        }
        return BLOCKLIST_EIO;
    }
    result = blocklistParse(context, text, (size_t)size, list, stats);
    context->allocator.free(text);
    return result;
}

static int contains4(const blocklist *list, uint32_t address)
{
    uint32_t low = 0;
    uint32_t high = list->count4;

    while (low < high) {                            /* last range starting <= address */
        uint32_t middle = low + (high - low) / 2u;

        if (list->v4[middle].first <= address) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return low > 0 && address <= list->v4[low - 1u].last;
}

static int contains6(const blocklist *list, const uint8_t address[16])
{
    uint32_t low = 0;
    uint32_t high = list->count6;

    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;

        if (memcmp(list->v6[middle].first, address, 16) <= 0) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return low > 0 && memcmp(address, list->v6[low - 1u].last, 16) <= 0;
}

int blocklistContains(const blocklist *list, const struct sockaddr *address)
{
    if (list == NULL || address == NULL) {
        return 0;
    }
    if (address->sa_family == AF_INET) {
        const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in *)address)->sin_addr;

        return contains4(list, ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                               ((uint32_t)b[2] << 8) | b[3]);
    }
    if (address->sa_family == AF_INET6) {
        const uint8_t *bytes = (const uint8_t *)&((const struct sockaddr_in6 *)address)->sin6_addr;
        uint32_t       v4;

        return mapped4(bytes, &v4) ? contains4(list, v4) : contains6(list, bytes);
    }
    return 0;
}

void blocklistFree(parserContext *context, blocklist *list)
{
    if (context == NULL || list == NULL) {
        return;
    }
    if (list->v4 != NULL) {
        context->allocator.free(list->v4);
    }
    if (list->v6 != NULL) {
        context->allocator.free(list->v6);
    }
    context->allocator.free(list);
}
