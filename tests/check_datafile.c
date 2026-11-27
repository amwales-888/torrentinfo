/*
 * check_datafile.c - whole-file reads and writes, atomic replacement, size
 * limits, and the names of files in the data directory.
 */
#include <string.h>

#include "check.h"
#include "datafile.h"
#include "platform.h"

static int endsWith(const char *text, const char *suffix)
{
    size_t length = strlen(text);
    size_t suffixLength = strlen(suffix);

    return length >= suffixLength && strcmp(text + length - suffixLength, suffix) == 0;
}

void checkDataFile(void)
{
    static const uint8_t first[] = "first contents";
    static const uint8_t second[] = "the second, longer, contents";
    uint8_t              hash[PARSER_SHA1_LENGTH];
    uint8_t             *data = NULL;
    uint64_t             length = 0;
    uint64_t             size;
    uint8_t              isRegular;
    int32_t              error = 0;
    char                 path[512];
    char                 temporary[600];
    char                 expected[64];
    uint32_t             i;

    checkScratchPath("data.bin", path, sizeof(path));
    CHECK(datafileWrite(path, first, sizeof(first), &error) == DATAFILE_OK);
    CHECK(datafileRead(&checkAllocator, path, 1024, &data, &length, &error) == DATAFILE_OK);
    CHECK(length == sizeof(first) && memcmp(data, first, sizeof(first)) == 0);
    checkAllocator.free(data);

    /* Atomic replacement leaves no temporary file behind. */
    CHECK(datafileWriteAtomic(path, second, sizeof(second), &error) == DATAFILE_OK);
    CHECK(datafileRead(&checkAllocator, path, 1024, &data, &length, &error) == DATAFILE_OK);
    CHECK(length == sizeof(second) && memcmp(data, second, sizeof(second)) == 0);
    checkAllocator.free(data);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    CHECK(platformFileStat(temporary, &size, &isRegular) == UV_ENOENT);

    /* Size limits, empty files, directories and missing files. */
    CHECK(datafileRead(&checkAllocator, path, 4, &data, &length, &error) == DATAFILE_ESIZE);
    CHECK(data == NULL);
    CHECK(datafileWrite(path, NULL, 0, &error) == DATAFILE_OK);
    CHECK(datafileRead(&checkAllocator, path, 1024, &data, &length, &error) == DATAFILE_ESIZE);
    checkScratchPath("", temporary, sizeof(temporary));
    CHECK(datafileRead(&checkAllocator, temporary, 1024, &data, &length, &error) ==
          DATAFILE_ENOTFILE);
    checkScratchPath("missing.bin", path, sizeof(path));
    CHECK(datafileRead(&checkAllocator, path, 1024, &data, &length, &error) == DATAFILE_EIO);
    CHECK(error == UV_ENOENT);

    /* Out of memory. */
    checkScratchPath("data.bin", path, sizeof(path));
    CHECK(datafileWrite(path, first, sizeof(first), &error) == DATAFILE_OK);
    checkFailAllocationsAfter(0);
    CHECK(datafileRead(&checkAllocator, path, 1024, &data, &length, &error) == DATAFILE_ENOMEM);
    checkFailAllocationsAfter(-1);

    /* Names in the data directory. */
    for (i = 0; i < PARSER_SHA1_LENGTH; i++) {
        hash[i] = (uint8_t)(i * 13u);
    }
    snprintf(expected, sizeof(expected), "%cresume%c000d1a2734414e5b6875828f9ca9b6c3d0ddeaf7.resume",
             PLATFORM_SEPARATOR, PLATFORM_SEPARATOR);
    if (datafileResumePath(hash, path, sizeof(path)) == DATAFILE_OK) {
        CHECK(endsWith(path, expected));
    }
    CHECK(datafileTorrentPath(hash, path, 10) == DATAFILE_ENAMETOOLONG ||
          datafileTorrentPath(hash, path, 10) == DATAFILE_ENODIR);
    CHECK(datafileResumePath(NULL, path, sizeof(path)) == DATAFILE_EINVAL);
}
