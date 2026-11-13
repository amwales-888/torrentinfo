/*
 * main.c - Example driver for the torrent parser and encoder.
 *
 * Inspect:  ./torrentinfo file.torrent
 * Create:   ./torrentinfo -o out.torrent [options] file1 [file2 ...]
 *
 * Build:    cc -std=c99 -Wall -Wextra -O2 -o torrentinfo main.c parser.c encoder.c
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "encoder.h"
#include "parser.h"

static const uint64_t maxTorrentSize = 64u * 1024u * 1024u;
static const uint32_t piecePreviewCount = 3u;

/* ------------------------------------------------------------------------ */
/* Allocator hooks: wrap the C library and count live allocations so we can */
/* check that the parser released everything.                               */
/* ------------------------------------------------------------------------ */

static uint64_t liveAllocations = 0;

static void *trackingRealloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size);

    if (result != NULL && ptr == NULL) {
        liveAllocations++;
    }
    return result;
}

static void trackingFree(void *ptr)
{
    if (ptr != NULL) {
        liveAllocations--;
        free(ptr);
    }
}

/* ------------------------------------------------------------------------ */
/* File loading                                                             */
/* ------------------------------------------------------------------------ */

static int readFile(const parserAllocator *allocator, const char *path,
                    uint8_t **bufferOut, uint64_t *lengthOut)
{
    struct stat info;
    uint8_t    *buffer;
    uint64_t    size;
    uint64_t    total = 0;
    ssize_t     count;
    int         fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &info) != 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (!S_ISREG(info.st_mode)) {
        fprintf(stderr, "%s: not a regular file\n", path);
        close(fd);
        return -1;
    }
    if (info.st_size <= 0 || (uint64_t)info.st_size > maxTorrentSize) {
        fprintf(stderr, "%s: unexpected file size (%lld bytes)\n",
                path, (long long)info.st_size);
        close(fd);
        return -1;
    }
    size = (uint64_t)info.st_size;

    buffer = allocator->realloc(NULL, (size_t)size);
    if (buffer == NULL) {
        fprintf(stderr, "%s: out of memory\n", path);
        close(fd);
        return -1;
    }

    while (total < size) {
        count = read(fd, buffer + total, (size_t)(size - total));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            allocator->free(buffer);
            close(fd);
            return -1;
        }
        if (count == 0) {
            break;
        }
        total += (uint64_t)count;
    }
    close(fd);

    if (total != size) {
        fprintf(stderr, "%s: short read\n", path);
        allocator->free(buffer);
        return -1;
    }

    *bufferOut = buffer;
    *lengthOut = size;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Printing helpers                                                         */
/* ------------------------------------------------------------------------ */

/* Torrent strings are untrusted: escape control characters so they cannot */
/* inject terminal escape sequences. UTF-8 bytes pass through unchanged.   */
static void printEscaped(const char *text)
{
    const unsigned char *p;

    for (p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20u || *p == 0x7Fu) {
            printf("\\x%02x", *p);
        } else {
            putchar(*p);
        }
    }
}

static void printField(const char *label, const char *value)
{
    printf("  %-15s ", label);
    if (value != NULL) {
        printEscaped(value);
    } else {
        printf("-");
    }
    putchar('\n');
}

static void printHex(const uint8_t *data, uint32_t length)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        printf("%02x", data[i]);
    }
}

static void formatSize(uint64_t bytes, char *out, size_t outSize)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double   value = (double)bytes;
    uint32_t unit = 0;

    if (bytes < 1024u) {
        snprintf(out, outSize, "%" PRIu64 " B", bytes);
        return;
    }
    while (value >= 1024.0 && unit < (sizeof(units) / sizeof(units[0])) - 1u) {
        value /= 1024.0;
        unit++;
    }
    snprintf(out, outSize, "%.2f %s (%" PRIu64 " bytes)", value, units[unit], bytes);
}

static void printFilePath(const parserTorrent *torrent, const parserFile *file)
{
    const parserPathComponent *component;

    if (torrent->isMultiFile) {
        printEscaped(torrent->name);
        putchar('/');
    }
    TAILQ_FOREACH(component, &file->path, entries) {
        if (component != TAILQ_FIRST(&file->path)) {
            putchar('/');
        }
        printEscaped(component->name);
    }
}

static void printTorrent(const char *path, const parserTorrent *torrent)
{
    const parserTier *tier;
    const parserUrl  *url;
    const parserFile *file;
    char              sizeText[64];
    char              dateText[64];
    struct tm         calendar;
    time_t            timestamp;
    uint32_t          index;

    printf("Torrent: %s\n\n", path);

    printf("General\n");
    printField("Name:", torrent->name);
    printf("  %-15s ", "Info hash:");
    printHex(torrent->infoHash, PARSER_SHA1_LENGTH);
    putchar('\n');
    printField("Mode:", torrent->isMultiFile ? "multi-file" : "single-file");
    formatSize(torrent->totalLength, sizeText, sizeof(sizeText));
    printField("Total size:", sizeText);
    formatSize(torrent->pieceLength, sizeText, sizeof(sizeText));
    printField("Piece length:", sizeText);
    printf("  %-15s %" PRIu32 "\n", "Pieces:", torrent->pieceCount);
    printField("Private:", torrent->isPrivate ? "yes" : "no");
    printField("Created by:", torrent->createdBy);
    if (torrent->hasCreationDate) {
        timestamp = (time_t)torrent->creationDate;
        if (gmtime_r(&timestamp, &calendar) != NULL &&
            strftime(dateText, sizeof(dateText), "%Y-%m-%d %H:%M:%S UTC", &calendar) > 0) {
            printf("  %-15s %s (%" PRId64 ")\n", "Creation date:", dateText,
                   torrent->creationDate);
        } else {
            printf("  %-15s %" PRId64 "\n", "Creation date:", torrent->creationDate);
        }
    } else {
        printField("Creation date:", NULL);
    }
    printField("Comment:", torrent->comment);
    printField("Encoding:", torrent->encoding);

    printf("\nTrackers\n");
    printField("Announce:", torrent->announce);
    if (torrent->tierCount == 0) {
        printf("  (no announce-list)\n");
    }
    index = 0;
    TAILQ_FOREACH(tier, &torrent->announceList, entries) {
        printf("  Tier %" PRIu32 ":\n", ++index);
        TAILQ_FOREACH(url, &tier->urls, entries) {
            printf("    ");
            printEscaped(url->url);
            putchar('\n');
        }
    }

    if (torrent->webSeedCount > 0) {
        printf("\nWeb seeds (%" PRIu32 ")\n", torrent->webSeedCount);
        TAILQ_FOREACH(url, &torrent->webSeeds, entries) {
            printf("  ");
            printEscaped(url->url);
            putchar('\n');
        }
    }

    printf("\nFiles (%" PRIu32 ")\n", torrent->fileCount);
    printf("  %16s  %16s  %s\n", "Offset", "Size", "Path");
    TAILQ_FOREACH(file, &torrent->files, entries) {
        printf("  %16" PRIu64 "  %16" PRIu64 "  ", file->offset, file->length);
        printFilePath(torrent, file);
        if (file->isPadding) {
            printf("  [padding]");
        }
        putchar('\n');
    }

    printf("\nPiece hashes\n");
    for (index = 0; index < torrent->pieceCount; index++) {
        if (index == piecePreviewCount && torrent->pieceCount > piecePreviewCount * 2u) {
            printf("  %10s\n", "...");
            index = torrent->pieceCount - piecePreviewCount;
        }
        printf("  %10" PRIu32 "  ", index);
        printHex(torrent->pieces + (uint64_t)index * PARSER_SHA1_LENGTH, PARSER_SHA1_LENGTH);
        putchar('\n');
    }
}

static void printEncoderError(const char *path, int32_t result, int32_t systemError)
{
    if (result == ENCODER_ERROR_IO && systemError != 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(systemError));
    } else {
        fprintf(stderr, "%s: %s\n", path, encoderErrorString(result));
    }
}

/* ------------------------------------------------------------------------ */
/* Inspect mode: ./torrentinfo file.torrent                                 */
/* ------------------------------------------------------------------------ */

static int inspectTorrent(parserContext *context, const parserAllocator *allocator,
                          const char *path)
{
    parserTorrent torrent;
    uint8_t      *buffer = NULL;
    uint64_t      length = 0;
    int32_t       result;

    if (readFile(allocator, path, &buffer, &length) != 0) {
        return -1;
    }
    result = parserParse(context, buffer, length, &torrent);
    allocator->free(buffer);        /* everything needed has been copied */

    if (result != PARSER_OK) {
        fprintf(stderr, "%s: parse failed: %s (at byte offset %" PRIu64 ")\n",
                path, parserErrorString(result), context->errorOffset);
        return -1;
    }
    printTorrent(path, &torrent);
    parserFree(context, &torrent);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Create mode: ./torrentinfo -o out.torrent file1 file2 ...                */
/* ------------------------------------------------------------------------ */

static int writeFile(const char *path, const uint8_t *data, uint64_t length)
{
    uint64_t total = 0;
    ssize_t  count;
    int      fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    while (total < length) {
        count = write(fd, data + total, (size_t)(length - total));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }
        total += (uint64_t)count;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* Default name for a multi-file torrent: "dir/av.torrent" -> "av" */
static void nameFromOutput(const char *outputPath, char *name, size_t size)
{
    static const char suffix[] = ".torrent";
    const size_t      suffixLength = sizeof(suffix) - 1u;
    const char       *base = strrchr(outputPath, '/');
    size_t            length;

    base = (base != NULL) ? base + 1 : outputPath;
    length = strlen(base);
    if (length > suffixLength && strcmp(base + length - suffixLength, suffix) == 0) {
        length -= suffixLength;
    }
    if (length >= size) {
        length = size - 1u;
    }
    memcpy(name, base, length);
    name[length] = '\0';

    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        snprintf(name, size, "torrent");
    }
}

/* Accepts bytes, or a number ending in K or M. */
static int parsePieceLength(const char *text, uint64_t *out)
{
    unsigned long long value;
    uint64_t           multiplier = 1;
    char              *end;

    if (text[0] < '0' || text[0] > '9') {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0) {
        return -1;
    }
    if (*end == 'k' || *end == 'K') {
        multiplier = 1024u;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1024u * 1024u;
        end++;
    }
    if (*end != '\0' || (uint64_t)value > UINT64_MAX / multiplier) {
        return -1;
    }
    value *= multiplier;
    if (value < ENCODER_MIN_PIECE_LENGTH || value > ENCODER_MAX_PIECE_LENGTH ||
        (value & (value - 1u)) != 0) {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

static int createTorrent(parserContext *context, const char *outputPath,
                         encoderOptions *options, char **paths, int pathCount)
{
    encoderState  encoder;
    parserTorrent torrent;
    parserTorrent check;
    uint8_t       infoHash[PARSER_SHA1_LENGTH];
    uint8_t      *buffer = NULL;
    uint64_t      length = 0;
    char          name[256];
    char          sizeText[64];
    int32_t       result;
    int           status = 0;
    int           i;

    if (pathCount > 1 && options->name == NULL) {
        nameFromOutput(outputPath, name, sizeof(name));
        options->name = name;
    }

    result = encoderBegin(&encoder, context, options);
    if (result != ENCODER_OK) {
        fprintf(stderr, "encoderBegin: %s\n", encoderErrorString(result));
        return -1;
    }

    for (i = 0; i < pathCount; i++) {
        result = encoderAddFile(&encoder, paths[i]);
        if (result != ENCODER_OK) {
            printEncoderError(paths[i], result, encoder.systemError);
            encoderAbort(&encoder);
            return -1;
        }
    }

    formatSize(encoder.totalLength, sizeText, sizeof(sizeText));
    fprintf(stderr, "Hashing %" PRIu32 " file(s), %s...\n", encoder.fileCount, sizeText);

    result = encoderEnd(&encoder, &torrent);
    if (result != ENCODER_OK) {
        if ((result == ENCODER_ERROR_IO || result == ENCODER_ERROR_FILE_CHANGED) &&
            encoder.errorFileIndex < (uint32_t)pathCount) {
            printEncoderError(paths[encoder.errorFileIndex], result, encoder.systemError);
        } else {
            printEncoderError(outputPath, result, encoder.systemError);
        }
        return -1;
    }

    memcpy(infoHash, torrent.infoHash, PARSER_SHA1_LENGTH);
    result = encoderEncode(context, &torrent, &buffer, &length);
    parserFree(context, &torrent);
    if (result != ENCODER_OK) {
        printEncoderError(outputPath, result, 0);
        return -1;
    }

    if (writeFile(outputPath, buffer, length) != 0) {
        status = -1;
    } else {
        /* Parse what was written as a round-trip check, and show it. */
        result = parserParse(context, buffer, length, &check);
        if (result != PARSER_OK) {
            fprintf(stderr, "%s: generated torrent does not parse: %s\n",
                    outputPath, parserErrorString(result));
            status = -1;
        } else {
            if (memcmp(check.infoHash, infoHash, PARSER_SHA1_LENGTH) != 0) {
                fprintf(stderr, "%s: info hash changed when re-parsed\n", outputPath);
                status = -1;
            }
            printTorrent(outputPath, &check);
            parserFree(context, &check);
        }
    }

    encoderFreeBuffer(context, buffer);
    return status;
}

/* ------------------------------------------------------------------------ */

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <file.torrent>\n"
            "       %s -o <output.torrent> [options] <file>...\n"
            "\n"
            "  -o file   create a torrent from the given files and write it to 'file'\n"
            "  -a url    tracker announce URL (repeat for more; one tier each)\n"
            "  -w url    web seed URL (repeatable)\n"
            "  -c text   comment\n"
            "  -n name   torrent name (default: the file name, or for several\n"
            "            files the output name without .torrent)\n"
            "  -l size   piece length, e.g. 262144, 256K or 1M (default: automatic)\n"
            "  -p        mark the torrent private\n",
            program, program);
}

int main(int argc, char **argv)
{
    parserAllocator allocator = { .realloc = trackingRealloc, .free = trackingFree };
    parserContext   context;
    encoderOptions  options;
    const char    **trackers = NULL;
    const char    **webSeeds = NULL;
    const char     *outputPath = NULL;
    uint8_t         createOnly = 0;     /* an option that only makes sense with -o */
    int32_t         result;
    int             option;
    int             status = -1;

    memset(&options, 0, sizeof(options));

    result = parserInit(&context, &allocator);
    if (result != PARSER_OK) {
        fprintf(stderr, "parserInit: %s\n", parserErrorString(result));
        return EXIT_FAILURE;
    }

    /* One slot per argument is always enough for the -a and -w values. */
    trackers = allocator.realloc(NULL, sizeof(*trackers) * (size_t)argc);
    webSeeds = allocator.realloc(NULL, sizeof(*webSeeds) * (size_t)argc);
    if (trackers == NULL || webSeeds == NULL) {
        fprintf(stderr, "out of memory\n");
        goto cleanup;
    }
    options.trackers = (const char *const *)trackers;
    options.webSeeds = (const char *const *)webSeeds;

    while ((option = getopt(argc, argv, "o:a:w:c:n:l:ph")) != -1) {
        switch (option) {
        case 'o':
            outputPath = optarg;
            break;
        case 'a':
            trackers[options.trackerCount++] = optarg;
            createOnly = 1;
            break;
        case 'w':
            webSeeds[options.webSeedCount++] = optarg;
            createOnly = 1;
            break;
        case 'c':
            options.comment = optarg;
            createOnly = 1;
            break;
        case 'n':
            options.name = optarg;
            createOnly = 1;
            break;
        case 'l':
            if (parsePieceLength(optarg, &options.pieceLength) != 0) {
                fprintf(stderr, "invalid piece length '%s': use a power of two "
                        "from 16K to 64M\n", optarg);
                goto cleanup;
            }
            createOnly = 1;
            break;
        case 'p':
            options.isPrivate = 1;
            createOnly = 1;
            break;
        case 'h':
            usage(argv[0]);
            status = 0;
            goto cleanup;
        default:
            usage(argv[0]);
            goto cleanup;
        }
    }

    if (outputPath != NULL) {
        if (optind >= argc) {
            usage(argv[0]);
            goto cleanup;
        }
        options.createdBy = "torrentinfo";
        options.creationDate = (int64_t)time(NULL);
        options.hasCreationDate = 1;
        status = createTorrent(&context, outputPath, &options, argv + optind, argc - optind);
    } else {
        if (createOnly || argc - optind != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        status = inspectTorrent(&context, &allocator, argv[optind]);
    }

cleanup:
    allocator.free((void *)trackers);
    allocator.free((void *)webSeeds);

    if (liveAllocations != 0) {
        fprintf(stderr, "warning: %" PRIu64 " allocation(s) not freed\n", liveAllocations);
        status = -1;
    }
    return (status == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
