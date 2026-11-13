/*
 * resume.c - Fast resume files. See resume.h.
 */
#include <stdio.h>
#include <string.h>
#include <uv.h>

#include "message.h"    /* messageBitfieldBytes() */
#include "piece.h"      /* PIECE_BLOCK_LENGTH */
#include "platform.h"
#include "resume.h"

#define RESUME_MAGIC            "TIRS"
#define RESUME_HEADER_LENGTH    (4u + 4u + PARSER_SHA1_LENGTH + 4u + 4u)

/* ------------------------------------------------------------------------ */
/* Encoding helpers                                                         */
/* ------------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)(value >> 32));
    put32(p + 4, (uint32_t)value);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get64(const uint8_t *p)
{
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

static uint32_t blockCountOf(uint32_t length)
{
    return (length + PIECE_BLOCK_LENGTH - 1u) / PIECE_BLOCK_LENGTH;
}

static uint32_t blockLengthOf(uint32_t pieceLength, uint32_t block)
{
    uint32_t begin = block * PIECE_BLOCK_LENGTH;

    return (pieceLength - begin < PIECE_BLOCK_LENGTH) ? pieceLength - begin : PIECE_BLOCK_LENGTH;
}

static void *allocateZeroed(parserContext *context, size_t size)
{
    void *memory = context->allocator.realloc(NULL, (size > 0) ? size : 1u);

    if (memory != NULL) {
        memset(memory, 0, (size > 0) ? size : 1u);
    }
    return memory;
}

static void release(parserContext *context, void *memory)
{
    if (memory != NULL) {
        context->allocator.free(memory);
    }
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                  */
/* ------------------------------------------------------------------------ */

/* A cursor over the file's bytes that refuses to read past the end. */
typedef struct reader {
    const uint8_t *at;
    const uint8_t *end;
    int            bad;
} reader;

static const uint8_t *take(reader *r, uint64_t length)
{
    const uint8_t *start = r->at;

    if (r->bad || (uint64_t)(r->end - r->at) < length) {
        r->bad = 1;
        return NULL;
    }
    r->at += length;
    return start;
}

static int32_t parse(parserContext *context, const uint8_t *text, uint64_t length,
                     const uint8_t infoHash[PARSER_SHA1_LENGTH], uint32_t pieceCount,
                     uint32_t fileCount, resumeData *data)
{
    reader         r;
    const uint8_t *p;
    uint8_t        digest[PARSER_SHA1_LENGTH];
    uint32_t       bitfieldBytes = messageBitfieldBytes(pieceCount);
    uint32_t       i;

    if (length < RESUME_HEADER_LENGTH + PARSER_SHA1_LENGTH) {
        return RESUME_EINVAL;
    }
    parserSha1(text, (size_t)(length - PARSER_SHA1_LENGTH), digest);
    if (memcmp(digest, text + length - PARSER_SHA1_LENGTH, PARSER_SHA1_LENGTH) != 0) {
        return RESUME_EINVAL;                       /* damaged */
    }
    r.at = text;
    r.end = text + length - PARSER_SHA1_LENGTH;
    r.bad = 0;
    p = take(&r, RESUME_HEADER_LENGTH);
    if (p == NULL || memcmp(p, RESUME_MAGIC, 4u) != 0 || get32(p + 4) != RESUME_VERSION ||
        memcmp(p + 8, infoHash, PARSER_SHA1_LENGTH) != 0 ||
        get32(p + 8 + PARSER_SHA1_LENGTH) != pieceCount ||
        get32(p + 12 + PARSER_SHA1_LENGTH) != fileCount) {
        return RESUME_EINVAL;
    }
    memcpy(data->infoHash, infoHash, PARSER_SHA1_LENGTH);
    data->pieceCount = pieceCount;
    data->fileCount = fileCount;
    data->have = allocateZeroed(context, bitfieldBytes);
    data->fileSizes = allocateZeroed(context, sizeof(uint64_t) * fileCount);
    data->fileTimes = allocateZeroed(context, sizeof(int64_t) * fileCount);
    if (data->have == NULL || data->fileSizes == NULL || data->fileTimes == NULL) {
        return RESUME_ENOMEM;
    }
    for (i = 0; i < fileCount; i++) {
        p = take(&r, 16u);
        if (p == NULL) {
            return RESUME_EINVAL;
        }
        data->fileSizes[i] = get64(p);
        data->fileTimes[i] = (int64_t)get64(p + 8);
    }
    p = take(&r, bitfieldBytes);
    if (p == NULL) {
        return RESUME_EINVAL;
    }
    memcpy(data->have, p, bitfieldBytes);

    p = take(&r, 4u);
    if (p == NULL) {
        return RESUME_EINVAL;
    }
    data->partialCount = get32(p);
    if (data->partialCount > pieceCount) {
        return RESUME_EINVAL;
    }
    data->partials = allocateZeroed(context, sizeof(resumePartial) * data->partialCount);
    if (data->partials == NULL) {
        return RESUME_ENOMEM;
    }
    for (i = 0; i < data->partialCount; i++) {
        resumePartial *partial = &data->partials[i];
        uint32_t       block;

        p = take(&r, 12u);
        if (p == NULL) {
            return RESUME_EINVAL;
        }
        partial->index = get32(p);
        partial->length = get32(p + 4);
        partial->blockCount = get32(p + 8);
        if (partial->index >= pieceCount || partial->length == 0 ||
            partial->blockCount != blockCountOf(partial->length)) {
            return RESUME_EINVAL;
        }
        p = take(&r, (partial->blockCount + 7u) / 8u);
        if (p == NULL) {
            return RESUME_EINVAL;
        }
        partial->blocks = allocateZeroed(context, (partial->blockCount + 7u) / 8u);
        partial->data = allocateZeroed(context, partial->length);
        if (partial->blocks == NULL || partial->data == NULL) {
            return RESUME_ENOMEM;
        }
        memcpy(partial->blocks, p, (partial->blockCount + 7u) / 8u);
        for (block = 0; block < partial->blockCount; block++) {
            if (messageBitfieldGet(partial->blocks, block)) {
                uint32_t blockLength = blockLengthOf(partial->length, block);

                p = take(&r, blockLength);
                if (p == NULL) {
                    return RESUME_EINVAL;
                }
                memcpy(partial->data + (uint64_t)block * PIECE_BLOCK_LENGTH, p, blockLength);
            }
        }
    }
    return (r.at == r.end) ? RESUME_OK : RESUME_EINVAL;
}

int32_t resumeLoad(parserContext *context, const char *path,
                   const uint8_t infoHash[PARSER_SHA1_LENGTH], uint32_t pieceCount,
                   uint32_t fileCount, resumeData *data, int32_t *systemError)
{
    platformFile file;
    uint8_t     *text;
    uint64_t     size;
    uint64_t     done = 0;
    uint8_t      isRegular;
    uint32_t     got = 0;
    int32_t      result;

    if (systemError != NULL) {
        *systemError = 0;
    }
    if (data != NULL) {
        memset(data, 0, sizeof(*data));
    }
    if (context == NULL || path == NULL || infoHash == NULL || data == NULL) {
        return RESUME_EINVAL;
    }
    result = platformFileStat(path, &size, &isRegular);
    if (result == 0 && (!isRegular || size > RESUME_MAX_FILE)) {
        result = UV_EINVAL;
    }
    if (result == 0) {
        result = platformFileOpenRead(path, &file);
    }
    if (result != 0) {
        if (systemError != NULL) {
            *systemError = result;
        }
        return RESUME_EIO;
    }
    text = context->allocator.realloc(NULL, (size_t)((size > 0) ? size : 1u));
    if (text == NULL) {
        platformFileClose(&file);
        return RESUME_ENOMEM;
    }
    while (done < size) {
        result = platformFileRead(&file, text + done,
                                  (uint32_t)((size - done > 0x40000000u) ? 0x40000000u
                                                                         : size - done),
                                  &got);
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
        return RESUME_EIO;
    }
    result = parse(context, text, size, infoHash, pieceCount, fileCount, data);
    context->allocator.free(text);
    if (result != RESUME_OK) {
        resumeFree(context, data);
    }
    return result;
}

/* ------------------------------------------------------------------------ */
/* Writing                                                                  */
/* ------------------------------------------------------------------------ */

int32_t resumeEncode(parserContext *context, const resumeData *data, uint8_t **textOut,
                     uint64_t *sizeOut)
{
    uint8_t *text;
    uint8_t *at;
    uint64_t size;
    uint32_t bitfieldBytes;
    uint32_t i;

    if (textOut != NULL) {
        *textOut = NULL;
    }
    if (context == NULL || data == NULL || data->have == NULL || textOut == NULL ||
        sizeOut == NULL) {
        return RESUME_EINVAL;
    }
    bitfieldBytes = messageBitfieldBytes(data->pieceCount);
    size = RESUME_HEADER_LENGTH + 16u * (uint64_t)data->fileCount + bitfieldBytes + 4u +
           PARSER_SHA1_LENGTH;
    for (i = 0; i < data->partialCount; i++) {
        const resumePartial *partial = &data->partials[i];
        uint32_t             block;

        size += 12u + (partial->blockCount + 7u) / 8u;
        for (block = 0; block < partial->blockCount; block++) {
            if (messageBitfieldGet(partial->blocks, block)) {
                size += blockLengthOf(partial->length, block);
            }
        }
    }
    if (size > RESUME_MAX_FILE) {
        return RESUME_EINVAL;
    }
    text = context->allocator.realloc(NULL, (size_t)size);
    if (text == NULL) {
        return RESUME_ENOMEM;
    }
    at = text;
    memcpy(at, RESUME_MAGIC, 4u);
    put32(at + 4, RESUME_VERSION);
    memcpy(at + 8, data->infoHash, PARSER_SHA1_LENGTH);
    put32(at + 8 + PARSER_SHA1_LENGTH, data->pieceCount);
    put32(at + 12 + PARSER_SHA1_LENGTH, data->fileCount);
    at += RESUME_HEADER_LENGTH;
    for (i = 0; i < data->fileCount; i++) {
        put64(at, (data->fileSizes != NULL) ? data->fileSizes[i] : 0u);
        put64(at + 8, (data->fileTimes != NULL) ? (uint64_t)data->fileTimes[i] : 0u);
        at += 16;
    }
    memcpy(at, data->have, bitfieldBytes);
    at += bitfieldBytes;
    put32(at, data->partialCount);
    at += 4;
    for (i = 0; i < data->partialCount; i++) {
        const resumePartial *partial = &data->partials[i];
        uint32_t             block;

        put32(at, partial->index);
        put32(at + 4, partial->length);
        put32(at + 8, partial->blockCount);
        memcpy(at + 12, partial->blocks, (partial->blockCount + 7u) / 8u);
        at += 12u + (partial->blockCount + 7u) / 8u;
        for (block = 0; block < partial->blockCount; block++) {
            if (messageBitfieldGet(partial->blocks, block)) {
                uint32_t blockLength = blockLengthOf(partial->length, block);

                memcpy(at, partial->data + (uint64_t)block * PIECE_BLOCK_LENGTH, blockLength);
                at += blockLength;
            }
        }
    }
    parserSha1(text, (size_t)(at - text), at);      /* resumeSeal() redoes it */
    *textOut = text;
    *sizeOut = size;
    return RESUME_OK;
}

void resumeSeal(uint8_t *text, uint64_t size, uint32_t fileCount, const uint64_t *sizes,
                const int64_t *times)
{
    uint8_t *at = text + RESUME_HEADER_LENGTH;
    uint32_t i;

    for (i = 0; i < fileCount; i++) {
        put64(at, sizes[i]);
        put64(at + 8, (uint64_t)times[i]);
        at += 16;
    }
    parserSha1(text, (size_t)(size - PARSER_SHA1_LENGTH), text + size - PARSER_SHA1_LENGTH);
}

int32_t resumeWrite(const char *path, const char *temporary, const uint8_t *text, uint64_t size,
                    int32_t *systemError)
{
    platformFile file;
    uint64_t     done = 0;
    uint32_t     written = 0;
    int32_t      result;

    if (systemError != NULL) {
        *systemError = 0;
    }
    if (path == NULL || temporary == NULL || text == NULL) {
        return RESUME_EINVAL;
    }
    result = platformFileOpenWrite(temporary, &file);
    if (result == 0) {
        while (done < size) {
            result = platformFileWrite(&file, text + done,
                                       (uint32_t)((size - done > 0x40000000u) ? 0x40000000u
                                                                              : size - done),
                                       &written);
            if (result != 0 || written == 0) {
                if (result == 0) {
                    result = UV_EIO;
                }
                break;
            }
            done += written;
        }
        if (platformFileClose(&file) != 0 && result == 0) {
            result = UV_EIO;
        }
        if (result == 0) {
            result = platformFileReplace(temporary, path);
        }
        if (result != 0) {
            platformFileRemove(temporary);
        }
    }
    if (result != 0) {
        if (systemError != NULL) {
            *systemError = result;
        }
        return RESUME_EIO;
    }
    return RESUME_OK;
}

int32_t resumeSave(parserContext *context, const char *path, const resumeData *data,
                   int32_t *systemError)
{
    uint8_t *text = NULL;
    uint64_t size = 0;
    size_t   pathLength;
    char    *temporary;
    int32_t  result;

    if (systemError != NULL) {
        *systemError = 0;
    }
    if (context == NULL || path == NULL) {
        return RESUME_EINVAL;
    }
    result = resumeEncode(context, data, &text, &size);
    if (result != RESUME_OK) {
        return result;
    }
    pathLength = strlen(path);
    temporary = context->allocator.realloc(NULL, pathLength + 5u);
    if (temporary == NULL) {
        context->allocator.free(text);
        return RESUME_ENOMEM;
    }
    memcpy(temporary, path, pathLength);
    memcpy(temporary + pathLength, ".tmp", 5u);
    result = resumeWrite(path, temporary, text, size, systemError);
    context->allocator.free(temporary);
    context->allocator.free(text);
    return result;
}

void resumeFree(parserContext *context, resumeData *data)
{
    uint32_t i;

    if (context == NULL || data == NULL) {
        return;
    }
    for (i = 0; data->partials != NULL && i < data->partialCount; i++) {
        release(context, data->partials[i].blocks);
        release(context, data->partials[i].data);
    }
    release(context, data->partials);
    release(context, data->have);
    release(context, data->fileSizes);
    release(context, data->fileTimes);
    memset(data, 0, sizeof(*data));
}
