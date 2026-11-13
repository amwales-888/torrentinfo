/*
 * main.c - Example driver for the torrent parser and encoder.
 *
 * Inspect:  ./torrentinfo file.torrent
 * Create:   ./torrentinfo -o out.torrent [options] file1 [file2 ...]
 * Announce: ./torrentinfo -t [-T seconds] file.torrent
 *
 * Build:    cc -std=c99 -Wall -Wextra -O2 -o torrentinfo main.c parser.c encoder.c \
 *               tracker.c $(pkg-config --cflags --libs libuv)
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
#include "tracker.h"

static const uint64_t maxTorrentSize = 64u * 1024u * 1024u;
static const uint32_t piecePreviewCount = 3u;
static const uint32_t peerPreviewCount = 10u;
static const uint32_t defaultAnnounceTimeout = 15u;
static int            announceTag;  /* privateArg for tracker requests */

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
/* Announce mode: ./torrentinfo -t file.torrent                             */
/* ------------------------------------------------------------------------ */

static void formatPeer(const trackerPeer *peer, char *out, size_t size)
{
    char address[64] = "?";

    if (peer->family == TRACKER_FAMILY_IPV4) {
        uv_inet_ntop(AF_INET, peer->address, address, sizeof(address));
        snprintf(out, size, "%s:%u", address, (unsigned)peer->port);
    } else {
        uv_inet_ntop(AF_INET6, peer->address, address, sizeof(address));
        snprintf(out, size, "[%s]:%u", address, (unsigned)peer->port);
    }
}

static void printCount(const char *label, int64_t value)
{
    if (value >= 0) {
        printf("  %-15s %" PRId64 "\n", label, value);
    } else {
        printField(label, NULL);
    }
}

static void printTrackerResult(const trackerResult *result, int32_t status)
{
    char     peerText[80];
    uint32_t i;

    printf("\n%s\n", result->url);
    printField("Status:", trackerStatusString(status));
    if (result->errorMessage != NULL) {
        printField("Error:", result->errorMessage);
    }
    if (result->failureReason != NULL) {
        printField("Failure:", result->failureReason);
    }
    if (result->warningMessage != NULL) {
        printField("Warning:", result->warningMessage);
    }
    if (status != TRACKER_OK) {
        return;
    }
    printf("  %-15s %" PRIu32 " s", "Interval:", result->interval);
    if (result->minInterval != 0) {
        printf(" (minimum %" PRIu32 " s)", result->minInterval);
    }
    putchar('\n');
    printCount("Seeders:", result->seeders);
    printCount("Leechers:", result->leechers);
    printf("  %-15s %" PRIu32 "\n", "Peers:", result->peerCount);
    for (i = 0; i < result->peerCount && i < peerPreviewCount; i++) {
        formatPeer(&result->peers[i], peerText, sizeof(peerText));
        printf("    %s\n", peerText);
    }
    if (result->peerCount > peerPreviewCount) {
        printf("    ... and %" PRIu32 " more\n", result->peerCount - peerPreviewCount);
    }
}

/* Runs from the libuv loop as each tracker answers (or fails). */
static void onAnnounceDone(void *privateArg, uint32_t uniqueId)
{
    trackerResult result;
    int32_t       status = trackerInfo(privateArg, uniqueId, &result);

    printTrackerResult(&result, status);
    fflush(stdout);
}

/* Start an announce to every distinct tracker URL in the torrent. */
static uint32_t startAnnounces(const parserTorrent *torrent, const char **seen,
                               uint32_t timeoutSeconds)
{
    const parserTier *tier;
    const parserUrl  *url;
    const char       *candidate;
    uint32_t          seenCount = 0;
    uint32_t          started = 0;
    uint32_t          i;
    uint32_t          id;
    int32_t           result;

    tier = TAILQ_FIRST(&torrent->announceList);
    url = NULL;
    candidate = torrent->announce;
    for (;;) {
        if (candidate != NULL) {
            for (i = 0; i < seenCount && strcmp(seen[i], candidate) != 0; i++) {
            }
            if (i == seenCount) {
                seen[seenCount++] = candidate;
                result = trackerConnect(torrent->infoHash, candidate, &id, timeoutSeconds,
                                        onAnnounceDone, &announceTag);
                if (result == TRACKER_OK) {
                    started++;
                } else {
                    printf("\n%s\n", candidate);
                    printField("Status:", trackerStatusString(result));
                    printField("Error:", "not started");
                }
            }
        }
        /* Next URL: walk each tier's URLs, then move to the next tier. */
        url = (url != NULL) ? TAILQ_NEXT(url, entries) : NULL;
        while (url == NULL && tier != NULL) {
            url = TAILQ_FIRST(&tier->urls);
            tier = TAILQ_NEXT(tier, entries);
        }
        if (url == NULL) {
            break;
        }
        candidate = url->url;
    }
    return started;
}

static int announceTorrent(parserContext *context, const parserAllocator *allocator,
                           const char *path, uint32_t timeoutSeconds)
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const parserTier *tier;
    parserTorrent     torrent;
    trackerConfig     config;
    trackerResult     result;
    uv_loop_t         loop;
    const char      **seen;
    uint32_t         *ids = NULL;
    uint8_t          *buffer = NULL;
    uint8_t           randomBytes[12];
    uint64_t          length = 0;
    uint32_t          urlCount = 1;
    uint32_t          count = 0;
    uint32_t          succeeded = 0;
    uint32_t          i;
    int32_t           status;

    if (readFile(allocator, path, &buffer, &length) != 0) {
        return -1;
    }
    status = parserParse(context, buffer, length, &torrent);
    allocator->free(buffer);
    if (status != PARSER_OK) {
        fprintf(stderr, "%s: parse failed: %s\n", path, parserErrorString(status));
        return -1;
    }

    /* Peer id in the common "-XX0000-" client style, then random characters. */
    memset(&config, 0, sizeof(config));
    memcpy(config.peerId, "-TI0001-", 8);
    if (uv_random(NULL, NULL, randomBytes, sizeof(randomBytes), 0, NULL) != 0) {
        memset(randomBytes, 0, sizeof(randomBytes));
    }
    for (i = 0; i < sizeof(randomBytes); i++) {
        config.peerId[8 + i] = (uint8_t)alphabet[randomBytes[i] % (sizeof(alphabet) - 1u)];
    }
    config.port = 6881;

    uv_loop_init(&loop);
    status = trackerInit(&loop, context, &config);
    if (status != TRACKER_OK) {
        fprintf(stderr, "trackerInit: %s\n", trackerStatusString(status));
        parserFree(context, &torrent);
        uv_loop_close(&loop);
        return -1;
    }

    printf("Announcing: ");
    printEscaped(torrent.name);
    printf("\nInfo hash:  ");
    printHex(torrent.infoHash, PARSER_SHA1_LENGTH);
    printf("\nTimeout:    %" PRIu32 " s\n", timeoutSeconds);

    TAILQ_FOREACH(tier, &torrent.announceList, entries) {
        urlCount += tier->urlCount;
    }
    seen = allocator->realloc(NULL, sizeof(*seen) * urlCount);
    if (seen == NULL) {
        fprintf(stderr, "out of memory\n");
    } else if (startAnnounces(&torrent, seen, timeoutSeconds) == 0) {
        printf("\nNo trackers could be contacted.\n");
    }
    allocator->free((void *)seen);
    parserFree(context, &torrent);      /* trackerConnect() copied what it needs */

    uv_run(&loop, UV_RUN_DEFAULT);      /* returns when every announce is done */

    /* Every request has finished: summarise them by iterating the ids. */
    trackerList(NULL, 0, &count);
    if (count > 0) {
        ids = allocator->realloc(NULL, sizeof(*ids) * count);
    }
    if (ids != NULL) {
        trackerList(ids, count, &count);
        printf("\nSummary\n");
        for (i = 0; i < count; i++) {
            status = trackerInfo(&announceTag, ids[i], &result);
            printf("  %-28s %6" PRIu32 " peers  %s\n", trackerStatusString(status),
                   result.peerCount, result.url);
            if (status == TRACKER_OK) {
                succeeded++;
            }
            trackerClose(ids[i]);
        }
        allocator->free(ids);
    }

    trackerShutdown();
    uv_run(&loop, UV_RUN_DEFAULT);      /* let any closing handles finish */
    uv_loop_close(&loop);
    return (succeeded > 0) ? 0 : -1;
}

/* ------------------------------------------------------------------------ */

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <file.torrent>\n"
            "       %s -o <output.torrent> [options] <file>...\n"
            "       %s -t [-T seconds] <file.torrent>\n"
            "\n"
            "  -o file   create a torrent from the given files and write it to 'file'\n"
            "  -a url    tracker announce URL (repeat for more; one tier each)\n"
            "  -w url    web seed URL (repeatable)\n"
            "  -c text   comment\n"
            "  -n name   torrent name (default: the file name, or for several\n"
            "            files the output name without .torrent)\n"
            "  -l size   piece length, e.g. 262144, 256K or 1M (default: automatic)\n"
            "  -p        mark the torrent private\n"
            "\n"
            "  -t        announce to every tracker in the torrent and list the peers\n"
            "  -T secs   announce timeout (default 15)\n",
            program, program, program);
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
    uint8_t         announce = 0;
    uint8_t         timeoutGiven = 0;  /* -T only makes sense with -t */
    uint32_t        timeoutSeconds = defaultAnnounceTimeout;
    unsigned long   number;
    char           *end;
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

    while ((option = getopt(argc, argv, "o:a:w:c:n:l:ptT:h")) != -1) {
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
        case 't':
            announce = 1;
            break;
        case 'T':
            number = strtoul(optarg, &end, 10);
            if (*optarg < '0' || *optarg > '9' || *end != '\0' || number == 0 || number > 3600u) {
                fprintf(stderr, "invalid timeout '%s': use 1 to 3600 seconds\n", optarg);
                goto cleanup;
            }
            timeoutSeconds = (uint32_t)number;
            timeoutGiven = 1;
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

    if (timeoutGiven && !announce) {
        usage(argv[0]);
        goto cleanup;
    }

    if (announce) {
        if (outputPath != NULL || createOnly || argc - optind != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        status = announceTorrent(&context, &allocator, argv[optind], timeoutSeconds);
    } else if (outputPath != NULL) {
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
