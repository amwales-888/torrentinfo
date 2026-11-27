/*
 * datafile.c - files in the data directory, and whole-file reads and
 * writes. See datafile.h.
 */
#include <stdio.h>
#include <string.h>

#include "datafile.h"
#include "platform.h"

static int32_t fail(int32_t *systemError, int32_t error)
{
    if (systemError != NULL) {
        *systemError = error;
    }
    return DATAFILE_EIO;
}

int32_t datafilePath(const char *subdirectory, const char *name, char *path, size_t size)
{
    size_t length;
    int    written;

    if (name == NULL || path == NULL || size == 0) {
        return DATAFILE_EINVAL;
    }
    if (platformDataDirectory(subdirectory, path, size) != 0) {
        path[0] = '\0';
        return DATAFILE_ENODIR;
    }
    length = strlen(path);
    written = snprintf(path + length, size - length, "%c%s", PLATFORM_SEPARATOR, name);
    if (written < 0 || (size_t)written >= size - length) {
        return DATAFILE_ENAMETOOLONG;
    }
    return DATAFILE_OK;
}

static int32_t hashPath(const char *subdirectory, const uint8_t infoHash[PARSER_SHA1_LENGTH],
                        const char *suffix, char *path, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    char              name[PARSER_SHA1_LENGTH * 2u + 16u];
    uint32_t          i;

    if (infoHash == NULL) {
        return DATAFILE_EINVAL;
    }
    for (i = 0; i < PARSER_SHA1_LENGTH; i++) {
        name[2u * i] = hex[infoHash[i] >> 4];
        name[2u * i + 1u] = hex[infoHash[i] & 0x0fu];
    }
    snprintf(name + 2u * PARSER_SHA1_LENGTH, 16, "%s", suffix);
    return datafilePath(subdirectory, name, path, size);
}

int32_t datafileResumePath(const uint8_t infoHash[PARSER_SHA1_LENGTH], char *path, size_t size)
{
    return hashPath("resume", infoHash, ".resume", path, size);
}

int32_t datafileTorrentPath(const uint8_t infoHash[PARSER_SHA1_LENGTH], char *path, size_t size)
{
    return hashPath("torrents", infoHash, ".torrent", path, size);
}

int32_t datafileRead(const parserAllocator *allocator, const char *path, uint64_t maxSize,
                     uint8_t **data, uint64_t *length, int32_t *systemError)
{
    platformFile file;
    uint8_t     *buffer;
    uint64_t     size;
    uint64_t     total = 0;
    uint32_t     count;
    uint8_t      isRegular;
    int32_t      result;

    if (allocator == NULL || path == NULL || data == NULL || length == NULL) {
        return DATAFILE_EINVAL;
    }
    *data = NULL;
    *length = 0;
    /* Directories first: Windows will not open one, which would otherwise be
     * reported as "access denied". */
    if (platformFileStat(path, &size, &isRegular) == 0 && !isRegular) {
        return DATAFILE_ENOTFILE;
    }
    result = platformFileOpenRead(path, &file);
    if (result != 0) {
        return fail(systemError, result);
    }
    result = platformFileInfo(&file, &size, &isRegular);
    if (result != 0) {
        platformFileClose(&file);
        return fail(systemError, result);
    }
    if (!isRegular) {
        platformFileClose(&file);
        return DATAFILE_ENOTFILE;
    }
    if (size == 0 || size > maxSize || size > (uint64_t)(SIZE_MAX / 2u)) {
        platformFileClose(&file);
        return DATAFILE_ESIZE;
    }
    buffer = allocator->realloc(NULL, (size_t)size);
    if (buffer == NULL) {
        platformFileClose(&file);
        return DATAFILE_ENOMEM;
    }
    while (total < size) {
        uint64_t left = size - total;

        result = platformFileRead(&file, buffer + total,
                                  (left > 0x40000000u) ? 0x40000000u : (uint32_t)left, &count);
        if (result != 0) {
            allocator->free(buffer);
            platformFileClose(&file);
            return fail(systemError, result);
        }
        if (count == 0) {
            break;
        }
        total += count;
    }
    platformFileClose(&file);
    if (total != size) {
        allocator->free(buffer);
        return DATAFILE_ESIZE;                      /* it changed while we read it */
    }
    *data = buffer;
    *length = size;
    return DATAFILE_OK;
}

int32_t datafileWrite(const char *path, const uint8_t *data, uint64_t length,
                      int32_t *systemError)
{
    platformFile file;
    uint64_t     total = 0;
    uint32_t     count;
    int32_t      result;

    if (path == NULL || (data == NULL && length > 0)) {
        return DATAFILE_EINVAL;
    }
    result = platformFileOpenWrite(path, &file);
    if (result != 0) {
        return fail(systemError, result);
    }
    while (total < length) {
        uint64_t left = length - total;

        result = platformFileWrite(&file, data + total,
                                   (left > 0x40000000u) ? 0x40000000u : (uint32_t)left, &count);
        if (result != 0) {
            platformFileClose(&file);
            return fail(systemError, result);
        }
        if (count == 0) {
            platformFileClose(&file);
            return fail(systemError, UV_EIO);
        }
        total += count;
    }
    result = platformFileClose(&file);
    return (result != 0) ? fail(systemError, result) : DATAFILE_OK;
}

int32_t datafileWriteAtomic(const char *path, const uint8_t *data, uint64_t length,
                            int32_t *systemError)
{
    char    temporary[4096];
    int32_t result;
    int     written;

    if (path == NULL) {
        return DATAFILE_EINVAL;
    }
    written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(temporary)) {
        return DATAFILE_ENAMETOOLONG;
    }
    result = datafileWrite(temporary, data, length, systemError);
    if (result != DATAFILE_OK) {
        (void)platformFileRemove(temporary);
        return result;
    }
    result = platformFileReplace(temporary, path);
    if (result != 0) {
        (void)platformFileRemove(temporary);
        return fail(systemError, result);
    }
    return DATAFILE_OK;
}

const char *datafileErrorString(int32_t error)
{
    switch (error) {
    case DATAFILE_OK:           return "success";
    case DATAFILE_EINVAL:       return "invalid argument";
    case DATAFILE_ENOMEM:       return "out of memory";
    case DATAFILE_EIO:          return "input/output error";
    case DATAFILE_ENAMETOOLONG: return "path too long";
    case DATAFILE_ENOTFILE:     return "not a regular file";
    case DATAFILE_ESIZE:        return "unexpected file size";
    case DATAFILE_ENODIR:       return "no data directory";
    default:                    return "unknown data file error";
    }
}
