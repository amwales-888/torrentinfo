/*
 * main.c - Example driver for the torrent parser and encoder.
 *
 * Inspect:  ./torrentinfo file.torrent
 * Create:   ./torrentinfo -o out.torrent [options] file1 [file2 ...]
 * Announce: ./torrentinfo -t [-T seconds] [-C ca.pem] file.torrent
 * Peers:    ./torrentinfo -P [-T seconds] [-C ca.pem] file.torrent
 * Download: ./torrentinfo -D [-S] [-L port] [-O dir] [-i piece] [-T seconds] [-C ca.pem]
 *           [-I interface] file.torrent
 *
 * Options come before the file names.
 *
 * Build:    Linux:   make   (see Makefile)
 *           Windows: open torrentinfo.sln in Visual Studio (see BUILDING.md)
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "announce.h"
#include "download.h"
#include "encoder.h"
#include "net.h"
#include "parser.h"
#include "peer.h"
#include "piece.h"
#include "platform.h"
#include "storage.h"
#include "tracker.h"    /* includes <uv.h>, and on Windows <windows.h> */

#ifdef _WIN32
#include <shellapi.h>   /* CommandLineToArgvW(); not in WIN32_LEAN_AND_MEAN */
#else
#include <signal.h>     /* SIGPIPE */
#endif

static const uint64_t maxTorrentSize = 64u * 1024u * 1024u;
static const uint32_t piecePreviewCount = 3u;
static const uint32_t peerPreviewCount = 10u;
static const uint32_t defaultAnnounceTimeout = 15u;
static int            announceTag;  /* privateArg for tracker requests */
static const uint32_t maxPeerAttempts = 200u;

/* What to do after announcing. */
#define MODE_ANNOUNCE   0u
#define MODE_PEERS      1u
#define MODE_DOWNLOAD   2u

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
    platformFile file;
    uint8_t     *buffer;
    uint64_t     size;
    uint64_t     total = 0;
    uint32_t     count;
    uint8_t      isRegular;
    int32_t      result;

    result = platformFileOpenRead(path, &file);
    if (result != 0) {
        fprintf(stderr, "%s: %s\n", path, uv_strerror(result));
        return -1;
    }
    result = platformFileInfo(&file, &size, &isRegular);
    if (result != 0) {
        fprintf(stderr, "%s: %s\n", path, uv_strerror(result));
        platformFileClose(&file);
        return -1;
    }
    if (!isRegular) {
        fprintf(stderr, "%s: not a regular file\n", path);
        platformFileClose(&file);
        return -1;
    }
    if (size == 0 || size > maxTorrentSize) {
        fprintf(stderr, "%s: unexpected file size (%" PRIu64 " bytes)\n", path, size);
        platformFileClose(&file);
        return -1;
    }

    buffer = allocator->realloc(NULL, (size_t)size);
    if (buffer == NULL) {
        fprintf(stderr, "%s: out of memory\n", path);
        platformFileClose(&file);
        return -1;
    }

    while (total < size) {
        result = platformFileRead(&file, buffer + total, (uint32_t)(size - total), &count);
        if (result != 0) {
            fprintf(stderr, "%s: %s\n", path, uv_strerror(result));
            allocator->free(buffer);
            platformFileClose(&file);
            return -1;
        }
        if (count == 0) {
            break;
        }
        total += count;
    }
    platformFileClose(&file);

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

/* Thread-safe UTC conversion: gmtime_r() on POSIX, gmtime_s() on Windows. */
static int toUtc(time_t timestamp, struct tm *calendar)
{
#ifdef _WIN32
    return gmtime_s(calendar, &timestamp) == 0;
#else
    return gmtime_r(&timestamp, calendar) != NULL;
#endif
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
        if (toUtc(timestamp, &calendar) &&
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
        fprintf(stderr, "%s: %s\n", path, uv_strerror(systemError));
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
    platformFile file;
    uint64_t     total = 0;
    uint32_t     count;
    int32_t      result;

    result = platformFileOpenWrite(path, &file);
    if (result != 0) {
        fprintf(stderr, "%s: %s\n", path, uv_strerror(result));
        return -1;
    }
    while (total < length) {
        result = platformFileWrite(&file, data + total, (uint32_t)(length - total), &count);
        if (result != 0) {
            fprintf(stderr, "%s: %s\n", path, uv_strerror(result));
            platformFileClose(&file);
            return -1;
        }
        total += count;
    }
    result = platformFileClose(&file);
    if (result != 0) {
        fprintf(stderr, "%s: %s\n", path, uv_strerror(result));
        return -1;
    }
    return 0;
}

/* Default name for a multi-file torrent: "dir/av.torrent" -> "av" */
static void nameFromOutput(const char *outputPath, char *name, size_t size)
{
    static const char suffix[] = ".torrent";
    const size_t      suffixLength = sizeof(suffix) - 1u;
    const char       *base = outputPath;
    const char       *p;
    size_t            length;

    /* The file name part: after the last '/' (and on Windows '\\' or "C:"). */
    for (p = outputPath; *p != '\0'; p++) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\' || (*p == ':' && p == outputPath + 1)) {
#else
        if (*p == '/') {
#endif
            base = p + 1;
        }
    }
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

/* Runs on the loop thread when the background hashing finishes. */
static void onCreated(void *privateArg, int32_t status)
{
    *(int32_t *)privateArg = status;
}

static int createTorrent(parserContext *context, const char *outputPath,
                         encoderOptions *options, char **paths, int pathCount)
{
    encoderState  encoder;
    uv_loop_t     loop;
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

    if (uv_loop_init(&loop) != 0) {
        fprintf(stderr, "uv_loop_init failed\n");
        return -1;
    }
    result = encoderBegin(&encoder, context, &loop, options);
    if (result != ENCODER_OK) {
        fprintf(stderr, "encoderBegin: %s\n", encoderErrorString(result));
        uv_loop_close(&loop);
        return -1;
    }

    for (i = 0; i < pathCount; i++) {
        result = encoderAddFile(&encoder, paths[i]);
        if (result != ENCODER_OK) {
            printEncoderError(paths[i], result, encoder.systemError);
            encoderAbort(&encoder);
            uv_loop_close(&loop);
            return -1;
        }
    }

    fprintf(stderr, "Hashing %" PRIu32 " file(s)...\n", encoder.fileCount);

    /* The reading and hashing happen on libuv's thread pool; the loop runs
     * until the callback has reported the result. */
    result = encoderEnd(&encoder, &torrent, onCreated, &result);
    if (result == ENCODER_OK) {
        uv_run(&loop, UV_RUN_DEFAULT);
    }
    uv_loop_close(&loop);
    if (result != ENCODER_OK) {
        if ((result == ENCODER_ERROR_IO || result == ENCODER_ERROR_FILE_CHANGED ||
             result == ENCODER_ERROR_NOT_REGULAR_FILE || result == ENCODER_ERROR_TOO_LARGE) &&
            encoder.errorFileIndex < (uint32_t)pathCount) {
            printEncoderError(paths[encoder.errorFileIndex], result, encoder.systemError);
        } else {
            printEncoderError(outputPath, result, encoder.systemError);
        }
        return -1;
    }
    formatSize(torrent.totalLength, sizeText, sizeof(sizeText));
    fprintf(stderr, "Hashed %s in %" PRIu32 " pieces.\n", sizeText, torrent.pieceCount);

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

/* ------------------------------------------------------------------------ */
/* Peer mode: ./torrentinfo -P file.torrent                                 */
/* ------------------------------------------------------------------------ */

struct peerRun;

/* One peer: handshake, then wait for its pieces and an unchoke. Also the
 * privateArg of its connection. */
typedef struct peerAttempt {
    struct peerRun *run;
    trackerPeer     peer;
    peerConnection *connection;
    int32_t         status;         /* handshake outcome                    */
    uint8_t         settled;        /* reported and closed                  */
    uint8_t         sawPieces;      /* bitfield or have received            */
    uint8_t         sawBitfield;
    uint8_t         unchoked;
    uint64_t        unchokedAt;     /* uv_now() when first unchoked          */
    uint8_t         dropped;        /* connection ended after the handshake */
    char            client[64];
    uint32_t        extensions;
    uint32_t        haveCount;
} peerAttempt;

/* The whole -P run. */
typedef struct peerRun {
    uv_timer_t      deadline;       /* ticks every 250 ms                    */
    uint64_t        deadlineAt;     /* uv_now() at which to give up          */
    uv_loop_t      *loop;
    peerAttempt    *attempts;
    uint32_t        count;
    uint32_t        pieceCount;
    uint32_t        started;
    uint32_t        settled;
} peerRun;

/*
 * Name the client from its peer id. Most clients use the Azureus style
 * "-XXvvvv-" (two-letter client code, four version characters); BitTorrent
 * mainline uses "Mx-y-z--". Anything else is shown as its printable prefix.
 */
static void describeClient(const uint8_t id[PEER_ID_LENGTH], char *out, size_t size)
{
    static const struct {
        const char *code;
        const char *name;
    } clients[] = {
        { "qB", "qBittorrent" }, { "TR", "Transmission" }, { "UT", "\xC2\xB5Torrent" },
        { "UM", "\xC2\xB5Torrent Mac" }, { "UW", "\xC2\xB5Torrent Web" },
        { "BT", "BitTorrent" }, { "BI", "BiglyBT" }, { "AZ", "Vuze" }, { "DE", "Deluge" },
        { "LT", "libtorrent (Rasterbar)" }, { "lt", "libTorrent (rTorrent)" },
        { "KT", "KTorrent" }, { "FD", "Free Download Manager" }, { "WW", "WebTorrent" },
        { "WD", "WebTorrent Desktop" }, { "PI", "PicoTorrent" }, { "BC", "BitComet" },
        { "XL", "Xunlei" }, { "SD", "Thunder" }, { "TI", "torrentinfo" }
    };
    char     text[16];
    size_t   used = 0;
    uint32_t i;

    if (id[0] == '-' && id[7] == '-') {
        const char *name = NULL;

        for (i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
            if (memcmp(id + 1, clients[i].code, 2) == 0) {
                name = clients[i].name;
            }
        }
        if (name != NULL) {
            snprintf(out, size, "%s %c.%c.%c.%c", name, id[3], id[4], id[5], id[6]);
        } else {                                    /* keep the unknown code visible */
            snprintf(out, size, "unknown client \"%c%c\" %c.%c.%c.%c",
                     id[1], id[2], id[3], id[4], id[5], id[6]);
        }
        for (i = 0; out[i] != '\0'; i++) {         /* never print control bytes */
            if ((unsigned char)out[i] < 0x20u) {
                out[i] = '?';
            }
        }
        return;
    }
    /* Shadow style: one client letter, up to five version characters, then
     * '-' padding, e.g. "T03I--" = BitTornado 0.3.18. Each character is one
     * number: 0-9, A-Z = 10-35, a-z = 36-61, '.' = 62. */
    {
        static const struct {
            char        letter;
            const char *name;
        } shadow[] = {
            { 'A', "ABC" }, { 'O', "Osprey Permaseed" }, { 'Q', "BTQueue" }, { 'R', "Tribler" },
            { 'S', "Shadow's client" }, { 'T', "BitTornado" }, { 'U', "UPnP NAT BitTorrent" }
        };
        char     version[32];
        size_t   length = 0;
        uint32_t digits = 0;
        uint32_t value;

        for (i = 0; i < sizeof(shadow) / sizeof(shadow[0]) && shadow[i].letter != (char)id[0]; i++) {
        }
        if (i < sizeof(shadow) / sizeof(shadow[0])) {
            const char *name = shadow[i].name;

            for (i = 1; i <= 5u && id[i] != '-'; i++) {
                if (id[i] >= '0' && id[i] <= '9') {
                    value = (uint32_t)(id[i] - '0');
                } else if (id[i] >= 'A' && id[i] <= 'Z') {
                    value = 10u + (uint32_t)(id[i] - 'A');
                } else if (id[i] >= 'a' && id[i] <= 'z') {
                    value = 36u + (uint32_t)(id[i] - 'a');
                } else if (id[i] == '.') {
                    value = 62u;
                } else {
                    break;                          /* not this style */
                }
                length += (size_t)snprintf(version + length, sizeof(version) - length,
                                           "%s%" PRIu32, (digits > 0) ? "." : "", value);
                digits++;
            }
            if (digits > 0 && i <= 6u && id[i] == '-') {
                snprintf(out, size, "%s %s", name, version);
                return;
            }
        }
    }
    if (id[0] == 'M' && id[1] >= '0' && id[1] <= '9') {
        for (i = 1; i < PEER_ID_LENGTH && used + 1u < sizeof(text) &&
                    (id[i] == '-' || (id[i] >= '0' && id[i] <= '9')); i++) {
            text[used++] = (char)((id[i] == '-') ? '.' : id[i]);
        }
        while (used > 0 && text[used - 1u] == '.') {
            used--;
        }
        text[used] = '\0';
        snprintf(out, size, "BitTorrent mainline %s", text);
        return;
    }
    for (i = 0; i < 8u && id[i] >= 0x20u && id[i] < 0x7Fu; i++) {
        text[used++] = (char)id[i];
    }
    text[used] = '\0';
    snprintf(out, size, (used > 0) ? "unknown client \"%s\"" : "unknown client%s", text);
}

static void toSocketAddress(const trackerPeer *peer, struct sockaddr_storage *out)
{
    uint8_t *port;

    memset(out, 0, sizeof(*out));
    if (peer->family == TRACKER_FAMILY_IPV4) {
        struct sockaddr_in *address = (struct sockaddr_in *)out;

        address->sin_family = AF_INET;
        memcpy(&address->sin_addr, peer->address, 4);
        port = (uint8_t *)&address->sin_port;
    } else {
        struct sockaddr_in6 *address = (struct sockaddr_in6 *)out;

        address->sin6_family = AF_INET6;
        memcpy(&address->sin6_addr, peer->address, 16);
        port = (uint8_t *)&address->sin6_port;
    }
    port[0] = (uint8_t)(peer->port >> 8);           /* network byte order */
    port[1] = (uint8_t)(peer->port & 0xFFu);
}

/* Print one line for a peer, close it, and stop the run once all are done. */
static void settle(peerAttempt *attempt, const char *note)
{
    peerRemoteState state;
    peerRun        *run = attempt->run;
    char            where[80];

    if (attempt->settled) {
        return;
    }
    attempt->settled = 1;
    formatPeer(&attempt->peer, where, sizeof(where));
    if (attempt->status != PEER_OK) {
        printf("  %-46s %s\n", where, note);
    } else {
        if (attempt->connection != NULL && peerGetState(attempt->connection, &state) == PEER_OK) {
            attempt->haveCount = state.haveCount;
        }
        printf("  %-46s %s", where, attempt->client);
        if (attempt->extensions != 0) {
            printf("  [%s%s%s ]",
                   (attempt->extensions & PEER_EXTENSION_PROTOCOL) ? " extensions" : "",
                   (attempt->extensions & PEER_EXTENSION_FAST) ? " fast" : "",
                   (attempt->extensions & PEER_EXTENSION_DHT) ? " dht" : "");
        }
        printf("\n  %-46s ", "");
        if (attempt->sawPieces) {
            printf("has %" PRIu32 "/%" PRIu32 " pieces (%.0f%%)", attempt->haveCount,
                   run->pieceCount,
                   run->pieceCount ? 100.0 * attempt->haveCount / run->pieceCount : 100.0);
        } else {
            printf("sent no bitfield");
        }
        printf(", %s%s%s\n", attempt->unchoked ? "unchoked us" : "choking us",
               (note != NULL) ? "; " : "", (note != NULL) ? note : "");
        if (attempt->sawPieces && !attempt->sawBitfield) {
            printf("  %-46s no bitfield, only haves: possibly a super-seeding seed\n", "");
        }
    }
    fflush(stdout);

    peerClose(attempt->connection);
    attempt->connection = NULL;
    if (++run->settled == run->started) {
        uv_timer_stop(&run->deadline);              /* everyone done: finish early */
    }
}

static void onHandshake(void *privateArg, peerConnection *connection,
                        const peerHandshakeResult *result)
{
    peerAttempt *attempt = privateArg;
    messageFrame interested;

    attempt->status = result->status;
    if (result->status != PEER_OK) {
        settle(attempt, result->errorMessage);
        return;
    }
    describeClient(result->peerId, attempt->client, sizeof(attempt->client));
    attempt->extensions = result->extensions;

    /* Ask for data, so that the peer has a reason to unchoke us. */
    memset(&interested, 0, sizeof(interested));
    interested.id = MESSAGE_INTERESTED;
    if (peerSend(connection, &interested) != PEER_OK) {
        settle(attempt, "could not send interested");
    }
}

static void onMessage(void *privateArg, peerConnection *connection, const messageFrame *message)
{
    peerAttempt *attempt = privateArg;

    (void)connection;
    if (message->id == MESSAGE_BITFIELD) {
        attempt->sawPieces = 1;
        attempt->sawBitfield = 1;
    } else if (message->id == MESSAGE_HAVE) {
        attempt->sawPieces = 1;
    } else if (message->id == MESSAGE_UNCHOKE) {
        attempt->unchoked = 1;
        if (attempt->unchokedAt == 0) {
            attempt->unchokedAt = uv_now(attempt->run->loop);
        }
    } else if (message->id == MESSAGE_CHOKE) {
        attempt->unchoked = 0;
    }
    /* With a bitfield we know everything at once. With only haves (lazy
     * bitfields, super-seeding) more may follow: onDeadline waits a little. */
    if (attempt->sawBitfield && attempt->unchoked) {
        settle(attempt, NULL);
    }
}

static void onPeerClosed(void *privateArg, peerConnection *connection, int32_t status,
                         const char *errorMessage)
{
    peerAttempt *attempt = privateArg;

    (void)connection;
    (void)status;
    attempt->dropped = 1;
    settle(attempt, errorMessage);
}

/*
 * Every 250 ms: report peers that unchoked us but sent only haves once they
 * have had 3 seconds to send more, and when time is up, report whatever
 * each remaining peer has told us.
 */
static void onDeadline(uv_timer_t *timer)
{
    peerRun     *run = timer->data;
    uint64_t     now = uv_now(run->loop);
    peerAttempt *attempt;
    uint32_t     i;

    for (i = 0; i < run->count; i++) {
        attempt = &run->attempts[i];
        if (attempt->connection == NULL || attempt->settled) {
            continue;
        }
        if (now >= run->deadlineAt) {
            settle(attempt, (attempt->status != PEER_OK) ? "no handshake before the deadline"
                            : attempt->unchoked          ? NULL
                                                         : "no unchoke before the deadline");
        } else if (attempt->unchoked && attempt->sawPieces && !attempt->sawBitfield &&
                   now - attempt->unchokedAt >= 3000u) {
            settle(attempt, NULL);
        }
    }
}

/* Add a peer unless it is already listed. Returns 0 if out of memory. */
static int addPeer(const parserAllocator *allocator, peerAttempt **attempts,
                   uint32_t *count, uint32_t *capacity, const trackerPeer *peer)
{
    peerAttempt *grown;
    uint32_t     i;

    for (i = 0; i < *count; i++) {
        if (memcmp(&(*attempts)[i].peer, peer, sizeof(*peer)) == 0) {
            return 1;
        }
    }
    if (*count == *capacity) {
        *capacity = (*capacity != 0) ? *capacity * 2u : 64u;
        grown = allocator->realloc(*attempts, sizeof(**attempts) * *capacity);
        if (grown == NULL) {
            return 0;
        }
        *attempts = grown;
    }
    memset(&(*attempts)[*count], 0, sizeof(**attempts));
    (*attempts)[*count].peer = *peer;
    (*count)++;
    return 1;
}

/* Handshake with every peer at once, wait for pieces and unchokes, then summarise. */
static uint32_t handshakePeers(uv_loop_t *loop, peerAttempt *attempts, uint32_t count,
                               const uint8_t infoHash[PARSER_SHA1_LENGTH],
                               const uint8_t peerId[PEER_ID_LENGTH], uint32_t pieceCount,
                               uint32_t timeoutSeconds)
{
    static const int32_t failures[] = {
        PEER_ETIMEDOUT, PEER_ERROR, PEER_ECLOSED, PEER_EPROTO, PEER_EHASH, PEER_ESELF, PEER_ENOMEM
    };
    peerCallbacks           callbacks = { onHandshake, onMessage, onPeerClosed, NULL };
    peerConnectOptions      options;
    struct sockaddr_storage address;
    peerRun                 run;
    uint32_t                handshakes = 0;
    uint32_t                seeds = 0;
    uint32_t                unchoked = 0;
    uint32_t                dropped = 0;
    uint32_t                tally;
    uint32_t                i;
    uint32_t                j;
    int32_t                 status;

    if (count > maxPeerAttempts) {
        printf("\n(Trying the first %" PRIu32 " of %" PRIu32 " peers.)\n", maxPeerAttempts, count);
        count = maxPeerAttempts;
    }
    printf("\nPeers (%" PRIu32 ", handshake timeout %" PRIu32 " s, then up to %" PRIu32
           " s for a bitfield and an unchoke)\n", count, timeoutSeconds, timeoutSeconds);

    memset(&run, 0, sizeof(run));
    run.loop = loop;
    run.attempts = attempts;
    run.count = count;
    run.pieceCount = pieceCount;
    memset(&options, 0, sizeof(options));
    memcpy(options.infoHash, infoHash, PARSER_SHA1_LENGTH);
    memcpy(options.peerId, peerId, PEER_ID_LENGTH);
    options.pieceCount = pieceCount;
    options.timeoutSeconds = timeoutSeconds;

    for (i = 0; i < count; i++) {
        attempts[i].run = &run;
        toSocketAddress(&attempts[i].peer, &address);
        status = peerConnect((const struct sockaddr *)&address, &options, &callbacks,
                             &attempts[i], &attempts[i].connection);
        if (status == PEER_OK) {
            attempts[i].status = PEER_ETIMEDOUT;    /* until onHandshake says otherwise */
            run.started++;
        } else {
            char where[80];

            attempts[i].status = status;
            attempts[i].settled = 1;
            formatPeer(&attempts[i].peer, where, sizeof(where));
            printf("  %-46s %s (not started)\n", where, peerStatusString(status));
        }
    }
    if (run.started > 0) {
        uv_timer_init(loop, &run.deadline);
        run.deadline.data = &run;
        run.deadlineAt = uv_now(loop) + (uint64_t)timeoutSeconds * 2000u;
        uv_timer_start(&run.deadline, onDeadline, 250u, 250u);
        uv_run(loop, UV_RUN_DEFAULT);               /* until every peer is settled */
        uv_close((uv_handle_t *)&run.deadline, NULL);
        uv_run(loop, UV_RUN_DEFAULT);
    }

    printf("\nPeer summary\n");
    for (i = 0; i < count; i++) {
        if (attempts[i].status == PEER_OK) {
            handshakes++;
            seeds += (attempts[i].sawPieces && attempts[i].haveCount == pieceCount) ? 1u : 0u;
            unchoked += attempts[i].unchoked;
            dropped += attempts[i].dropped;
        }
    }
    printf("  %-28s %5" PRIu32 "\n", "Handshake OK", handshakes);
    printf("  %-28s %5" PRIu32 "\n", "  with every piece", seeds);
    printf("  %-28s %5" PRIu32 "\n", "  unchoked us", unchoked);
    printf("  %-28s %5" PRIu32 "\n", "  dropped afterwards", dropped);
    for (j = 0; j < sizeof(failures) / sizeof(failures[0]); j++) {
        tally = 0;
        for (i = 0; i < count; i++) {
            tally += (attempts[i].status == failures[j]) ? 1u : 0u;
        }
        if (tally > 0) {
            printf("  %-28s %5" PRIu32 "\n", peerStatusString(failures[j]), tally);
        }
    }
    return handshakes;
}

/* ------------------------------------------------------------------------ */
/* Download mode: ./torrentinfo -D [-S] [-L port] [-O dir] [-i piece] ...   */
/*                                                                          */
/* The download module does the work: it opens the files (hashing existing  */
/* data, so an interrupted download resumes), connects to the peers the     */
/* trackers returned and fetches pieces from all of them at once. This      */
/* prints its events and a progress line every two seconds.                 */
/* ------------------------------------------------------------------------ */

typedef struct downloadRun {
    uv_loop_t      *loop;
    download       *transfer;
    announce       *trackers;           /* NULL if the torrent lists none   */
    uv_timer_t      timer;              /* progress line                    */
    uv_signal_t     interrupt;          /* Ctrl-C                           */
#ifndef _WIN32
    uv_signal_t     terminate;          /* SIGTERM                          */
#endif
    uint8_t         trackerUp;          /* trackerInit() succeeded          */
    parserContext  *context;
    const parserTorrent *torrent;
    uint8_t         stopping;           /* telling the trackers we leave    */
    uint8_t         complete;           /* everything wanted is on disk     */
    uint8_t         seed;               /* -S: keep running once complete   */
    uint8_t         failed;
    uint64_t        startedAt;          /* uv_hrtime() when the files were ready */
    uint64_t        lastReport;         /* uv_now() of the last progress line */
    uint64_t        reportReceived;     /* bytes received at that line       */
    uint64_t        reportSent;         /* bytes uploaded at that line       */
} downloadRun;

static void formatBytes(uint64_t bytes, char *out, size_t size)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double             value = (double)bytes;
    uint32_t           unit = 0;

    while (value >= 1024.0 && unit < 4u) {
        value /= 1024.0;
        unit++;
    }
    snprintf(out, size, (unit == 0) ? "%.0f %s" : "%.1f %s", value, units[unit]);
}

static void onProgressTick(uv_timer_t *timer)
{
    downloadRun     *run = timer->data;
    downloadProgress progress;
    uint64_t         now = uv_now(timer->loop);
    uint64_t         elapsed = now - run->lastReport;
    char             down[32];
    char             up[32];

    if (run->transfer == NULL || elapsed == 0) {
        return;
    }
    downloadGetProgress(run->transfer, &progress);
    formatBytes((progress.bytesReceived - run->reportReceived) * 1000u / elapsed, down,
                sizeof(down));
    formatBytes((progress.bytesUploaded - run->reportSent) * 1000u / elapsed, up, sizeof(up));
    if (progress.seeding) {
        printf("  seeding  up %9s/s  peers %" PRIu32 " connected (%" PRIu32 " incoming),"
               " %" PRIu32 " interested, uploading to %" PRIu32 "\n",
               up, progress.peersConnected, progress.peersIncoming, progress.peersInterested,
               progress.peersUploading);
    } else {
        printf("  %5.1f%%  %" PRIu32 "/%" PRIu32 "  down %9s/s  up %9s/s  peers %" PRIu32
               " (%" PRIu32 " in), %" PRIu32 " unchoking us, %" PRIu32 " sending, uploading to %"
               PRIu32 "%s\n",
               (progress.wanted > 0) ? 100.0 * progress.done / progress.wanted : 100.0,
               progress.done, progress.wanted, down, up, progress.peersConnected,
               progress.peersIncoming, progress.peersUnchoking, progress.peersDownloading,
               progress.peersUploading, progress.endgame ? "  [endgame]" : "");
    }
    fflush(stdout);
    run->lastReport = now;
    run->reportReceived = progress.bytesReceived;
    run->reportSent = progress.bytesUploaded;
}

/* --- Trackers ----------------------------------------------------------- */

static void getAnnounceStats(void *privateArg, announceStats *stats)
{
    downloadRun     *run = privateArg;
    downloadProgress progress;

    downloadGetProgress(run->transfer, &progress);
    stats->uploaded = progress.bytesUploaded;
    stats->downloaded = progress.bytesReceived;
    stats->left = progress.left;
}

static void onAnnouncePeers(void *privateArg, const trackerPeer *peers, uint32_t count)
{
    downloadRun            *run = privateArg;
    struct sockaddr_storage address;
    uint32_t                i;

    for (i = 0; i < count && run->transfer != NULL; i++) {
        toSocketAddress(&peers[i], &address);
        downloadAddPeer(run->transfer, (const struct sockaddr *)&address);
    }
}

static void onAnnounceResult(void *privateArg, const char *url, uint32_t event, int32_t status,
                             const trackerResult *result)
{
    static const char *events[] = { "", " (started)", " (completed)", " (stopped)" };

    (void)privateArg;
    printf("  tracker %s%s: ", url, events[event <= TRACKER_EVENT_STOPPED ? event : 0]);
    if (result == NULL) {
        printf("could not announce: %s\n", trackerStatusString(status));
    } else if (status != TRACKER_OK) {
        printf("%s\n", (result->failureReason != NULL) ? result->failureReason
                       : (result->errorMessage != NULL) ? result->errorMessage
                                                         : trackerStatusString(status));
    } else {
        printf("%" PRIu32 " peer%s", result->peerCount, (result->peerCount == 1u) ? "" : "s");
        if (result->seeders >= 0 && result->leechers >= 0) {
            printf(" (%" PRId64 " seeders, %" PRId64 " leechers)", result->seeders,
                   result->leechers);
        }
        printf(", next in %" PRIu32 " s\n", result->interval);
    }
    fflush(stdout);
}

/* --- Shutting down ------------------------------------------------------ */

static void closeRun(downloadRun *run)
{
    if (run->trackers != NULL) {
        announceEnd(run->trackers);
        run->trackers = NULL;
    }
    if (run->transfer != NULL) {
        downloadEnd(run->transfer);
        run->transfer = NULL;
    }
    uv_timer_stop(&run->timer);
    uv_signal_stop(&run->interrupt);
#ifndef _WIN32
    uv_signal_stop(&run->terminate);
#endif
}

static void onTrackersStopped(void *privateArg)
{
    closeRun(privateArg);
}

/* Leave the swarm: tell the trackers (a few seconds at most), then stop. */
static void stopRun(downloadRun *run)
{
    if (run->stopping) {
        return;
    }
    run->stopping = 1;
    if (run->trackers != NULL) {
        printf("Telling the trackers we are leaving...\n");
        fflush(stdout);
        announceStop(run->trackers, 5u, onTrackersStopped, run);
    } else {
        closeRun(run);
    }
}

static int downloadIsSeeding(const download *transfer)
{
    downloadProgress progress;

    downloadGetProgress(transfer, &progress);
    return progress.seeding;
}

static void onInterrupt(uv_signal_t *signal, int number)
{
    downloadRun *run = signal->data;

    (void)number;
    if (run->stopping) {
        printf("\nStopping now.\n");
        closeRun(run);                              /* second Ctrl-C: do not wait */
        return;
    }
    if (run->transfer != NULL && !run->complete && !downloadIsSeeding(run->transfer)) {
        downloadProgress progress;
        char             sent[32];

        downloadGetProgress(run->transfer, &progress);
        formatBytes(progress.bytesUploaded, sent, sizeof(sent));
        printf("\nInterrupted: %" PRIu32 " of %" PRIu32 " pieces on disk, %s uploaded;"
               " run again to resume.\n", progress.done, progress.wanted, sent);
    } else if (run->transfer != NULL) {
        downloadProgress progress;
        char             sent[32];

        downloadGetProgress(run->transfer, &progress);
        formatBytes(progress.bytesUploaded, sent, sizeof(sent));
        printf("\nStopped seeding: %s uploaded in total.\n", sent);
    } else {
        printf("\nInterrupted.\n");
    }
    stopRun(run);
}

/* --- Download events ---------------------------------------------------- */

static void onDownloadReady(void *privateArg, download *transfer, int32_t status,
                            const char *message)
{
    downloadRun     *run = privateArg;
    downloadProgress progress;

    (void)message;
    if (status != DOWNLOAD_OK) {
        return;                                     /* onDownloadFinished reports it */
    }
    downloadGetProgress(transfer, &progress);
    printf("Saving to:  %s\n", downloadRoot(transfer));
    if (progress.resumed > 0) {
        printf("Resuming:   %" PRIu32 " of %" PRIu32 " pieces already on disk and verified\n",
               progress.resumed, progress.pieceCount);
    }
    if (progress.done < progress.wanted) {
        printf("\nDownloading %" PRIu32 " piece(s); Ctrl-C to stop\n",
               progress.wanted - progress.done);
    } else if (run->seed) {
        run->complete = 1;                          /* complete already: seeding only */
        printf("\nSeeding; Ctrl-C to stop\n");
    }
    fflush(stdout);
    run->startedAt = uv_hrtime();
    run->lastReport = uv_now(run->loop);
    uv_timer_start(&run->timer, onProgressTick, 2000u, 2000u);

    /* Announce now that "left" is known. */
    if (run->trackerUp && run->trackers == NULL) {
        static const announceEvents callbacks = { getAnnounceStats, onAnnouncePeers,
                                                  onAnnounceResult };

        status = announceBegin(run->loop, run->context, run->torrent, &callbacks, run,
                               &run->trackers);
        if (status == ANNOUNCE_ENOTRACKERS) {
            printf("The torrent lists no trackers.\n");
        } else if (status != ANNOUNCE_OK) {
            printf("Could not start announcing.\n");
        }
    }
}

static void onDownloadEvent(void *privateArg, download *transfer, const char *message)
{
    (void)privateArg;
    (void)transfer;
    printf("  %s\n", message);
    fflush(stdout);
}

static void reportDownload(downloadRun *run, download *transfer)
{
    downloadProgress progress;
    double           seconds = (double)(uv_hrtime() - run->startedAt) / 1e9;
    char             size[32];
    char             wasted[32];
    char             sent[32];

    downloadGetProgress(transfer, &progress);
    formatBytes(progress.bytesReceived, size, sizeof(size));
    formatBytes(progress.bytesWasted, wasted, sizeof(wasted));
    formatBytes(progress.bytesUploaded, sent, sizeof(sent));
    printf("\nDownload complete: %s\n  %" PRIu32 " piece(s), %s received in %.1f s"
           " (%.1f KiB/s)", downloadRoot(transfer), progress.done - progress.resumed, size,
           seconds, (seconds > 0.0) ? (double)progress.bytesReceived / 1024.0 / seconds : 0.0);
    if (progress.bytesWasted > 0 || progress.hashFailures > 0) {
        printf("; %s wasted, %" PRIu32 " piece(s) failed verification", wasted,
               progress.hashFailures);
    }
    printf("; %s uploaded\n", sent);
    fflush(stdout);
}

static void onDownloadComplete(void *privateArg, download *transfer)
{
    downloadRun *run = privateArg;

    run->complete = 1;
    reportDownload(run, transfer);
    if (run->trackers != NULL) {
        announceCompleted(run->trackers);
    }
}

static void onDownloadFinished(void *privateArg, download *transfer, int32_t status,
                               const char *message)
{
    downloadRun     *run = privateArg;
    downloadProgress progress;

    downloadGetProgress(transfer, &progress);
    if (status == DOWNLOAD_OK && !run->complete) {
        printf("\nNothing to download: %s\n", message);
        run->complete = 1;
    } else if (status != DOWNLOAD_OK) {
        printf("\nDownload failed: %s (%" PRIu32 " of %" PRIu32 " pieces on disk)\n", message,
               progress.done, progress.wanted);
        run->failed = 1;
    }
    fflush(stdout);
    stopRun(run);
}

/*
 * Download the torrent (or piece 'wanted') into 'directory' from the peers
 * the trackers give, re-announcing as they ask, accepting connections on
 * 'listenPort' (0: none), and, with 'seed', serving peers once complete
 * until Ctrl-C.
 */
static int downloadTorrent(uv_loop_t *loop, parserContext *context, const parserTorrent *torrent,
                           const trackerConfig *config, uint32_t timeoutSeconds, int64_t wanted,
                           const char *directory, uint16_t listenPort, uint8_t seed)
{
    downloadEvents  events = { onDownloadReady, onDownloadEvent, onDownloadFinished,
                               onDownloadComplete };
    downloadOptions options;
    trackerConfig   trackerSettings = *config;
    downloadRun     run;
    int32_t         status;

    if (wanted >= (int64_t)torrent->pieceCount) {
        printf("\nPiece %" PRId64 " does not exist: the torrent has %" PRIu32 " pieces.\n",
               wanted, torrent->pieceCount);
        return -1;
    }
    memset(&run, 0, sizeof(run));
    run.loop = loop;
    run.context = context;
    run.torrent = torrent;
    run.seed = seed;
    memset(&options, 0, sizeof(options));
    memcpy(options.peerId, config->peerId, PEER_ID_LENGTH);
    options.directory = directory;
    options.handshakeTimeoutSeconds = timeoutSeconds;
    options.onlyPiece = wanted;
    options.listenPort = listenPort;
    options.seed = seed;

    uv_timer_init(loop, &run.timer);
    run.timer.data = &run;
    uv_signal_init(loop, &run.interrupt);
    run.interrupt.data = &run;
#ifndef _WIN32
    uv_signal_init(loop, &run.terminate);
    run.terminate.data = &run;
#endif

    if (peerInit(loop, context, config->binding) != PEER_OK) {
        printf("Could not start the peer module.\n");
        run.failed = 1;
        goto done;
    }
    printf("\nChecking files...\n");
    fflush(stdout);
    status = downloadBegin(loop, context, torrent, &options, &events, &run, &run.transfer);
    if (status != DOWNLOAD_OK) {
        printf("Could not start the download: %s\n", downloadStatusString(status));
        run.failed = 1;
        peerShutdown();
        goto done;
    }
    if (listenPort != 0) {
        if (downloadListenPort(run.transfer) != 0) {
            printf("Listening:  port %u\n", (unsigned)downloadListenPort(run.transfer));
            trackerSettings.port = downloadListenPort(run.transfer);
        } else {
            printf("Listening:  could not open ports %u-%u; outgoing connections only\n",
                   (unsigned)listenPort, (unsigned)(listenPort + DOWNLOAD_LISTEN_ATTEMPTS - 1u));
        }
    }
    status = trackerInit(loop, context, &trackerSettings);
    if (status == TRACKER_OK) {
        run.trackerUp = 1;                          /* announcing starts once ready */
    } else {
        printf("trackerInit: %s\n", trackerStatusString(status));
    }
    uv_signal_start(&run.interrupt, onInterrupt, SIGINT);
#ifndef _WIN32
    uv_signal_start(&run.terminate, onInterrupt, SIGTERM);
#endif

    uv_run(loop, UV_RUN_DEFAULT);                   /* until closeRun() */
    peerShutdown();
    if (run.trackerUp) {
        trackerShutdown();
    }

done:
    uv_close((uv_handle_t *)&run.timer, NULL);
    uv_close((uv_handle_t *)&run.interrupt, NULL);
#ifndef _WIN32
    uv_close((uv_handle_t *)&run.terminate, NULL);
#endif
    uv_run(loop, UV_RUN_DEFAULT);                   /* closing handles */
    return (run.complete && !run.failed) ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Verify mode: ./torrentinfo -V [-O dir] file.torrent                      */
/*                                                                          */
/* Hash the data already on disk against the torrent, without changing      */
/* anything, and report each file: complete, partial or missing, how many   */
/* of its bytes are verified, and which pieces are missing.                 */
/* ------------------------------------------------------------------------ */

static const uint32_t verifyRangeLimit = 40u;   /* missing-piece ranges listed per line */

typedef struct verifyRun {
    int32_t status;
    char    message[STORAGE_ERROR_MESSAGE_LENGTH];
    uint8_t done;
} verifyRun;

static void onVerifyOpen(void *privateArg, storage *store, int32_t status, const char *errorMessage)
{
    verifyRun *run = privateArg;

    (void)store;
    run->status = status;
    run->done = 1;
    if (errorMessage != NULL) {
        snprintf(run->message, sizeof(run->message), "%s", errorMessage);
    }
}

static double percentOf(uint64_t part, uint64_t whole)
{
    return (whole == 0) ? 100.0 : 100.0 * (double)part / (double)whole;
}

/* Bytes of piece 'index' that fall inside [start, end). */
static uint64_t pieceOverlap(const parserTorrent *torrent, uint32_t index, uint64_t start, uint64_t end)
{
    uint64_t pieceStart = (uint64_t)index * torrent->pieceLength;
    uint64_t pieceEnd = pieceStart + torrent->pieceLength;

    if (pieceEnd > torrent->totalLength) {
        pieceEnd = torrent->totalLength;
    }
    if (pieceStart < start) {
        pieceStart = start;
    }
    if (pieceEnd > end) {
        pieceEnd = end;
    }
    return (pieceEnd > pieceStart) ? pieceEnd - pieceStart : 0;
}

/*
 * Print the pieces in [first, last] that are not in 'have' as ranges
 * ("3, 7-12, 40"), at most 'verifyRangeLimit' of them. Returns the number
 * of missing pieces.
 */
static uint32_t printMissing(const uint8_t *have, uint32_t first, uint32_t last)
{
    uint32_t missing = 0;
    uint32_t ranges = 0;
    uint32_t hidden = 0;
    uint32_t index = first;
    uint32_t start;

    while (index <= last) {
        if (messageBitfieldGet(have, index)) {
            if (index == last) {
                break;
            }
            index++;
            continue;
        }
        start = index;
        while (index < last && !messageBitfieldGet(have, index + 1u)) {
            index++;
        }
        missing += index - start + 1u;
        if (ranges < verifyRangeLimit) {
            printf("%s%" PRIu32, (ranges > 0) ? ", " : "", start);
            if (index > start) {
                printf("-%" PRIu32, index);
            }
        } else {
            hidden++;
        }
        ranges++;
        if (index == last) {
            break;
        }
        index++;
    }
    if (hidden > 0) {
        printf(", ... (%" PRIu32 " more range%s)", hidden, (hidden == 1u) ? "" : "s");
    }
    return missing;
}

static int verifyTorrent(parserContext *context, const parserAllocator *allocator,
                         const char *path, const char *directory)
{
    const parserFile *file;
    parserTorrent     torrent;
    storageFileState  state;
    verifyRun         run;
    uv_loop_t         loop;
    storage          *store = NULL;
    const uint8_t    *have;
    uint8_t          *buffer = NULL;
    uint64_t          length = 0;
    uint64_t          bytesDone = 0;
    uint64_t          dataLength = 0;   /* excluding padding */
    uint32_t          haveCount = 0;
    uint32_t          fileIndex = 0;
    uint32_t          shown = 0;
    uint32_t          padding = 0;
    uint32_t          filesComplete = 0;
    uint32_t          i;
    char              size[64];
    char              total[64];
    int               result = -1;
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

    printf("Verifying:  ");
    printEscaped(torrent.name);
    printf("\nInfo hash:  ");
    printHex(torrent.infoHash, PARSER_SHA1_LENGTH);
    formatSize(torrent.pieceLength, size, sizeof(size));
    printf("\nPieces:     %" PRIu32 " of %s\n", torrent.pieceCount, size);
    fflush(stdout);

    memset(&run, 0, sizeof(run));
    uv_loop_init(&loop);
    status = storageOpen(&loop, context, &torrent, directory, STORAGE_READ_ONLY, onVerifyOpen,
                         &run, &store);
    if (status != STORAGE_OK) {
        printf("Could not check the files: %s\n", storageStatusString(status));
        goto done;
    }
    printf("Location:   %s\n\nHashing the data on disk...\n", storageRoot(store));
    fflush(stdout);
    uv_run(&loop, UV_RUN_DEFAULT);
    if (run.status != STORAGE_OK) {
        printf("Could not check the files: %s\n",
               (run.message[0] != '\0') ? run.message : storageStatusString(run.status));
        goto done;
    }
    have = storageHave(store, &haveCount);

    /* Each file: the pieces it spans, and how many of its bytes they verify. */
    printf("\nFiles\n");
    TAILQ_FOREACH(file, &torrent.files, entries) {
        uint64_t    end = file->offset + file->length;
        uint64_t    verified = 0;
        uint32_t    first = (uint32_t)(file->offset / torrent.pieceLength);
        uint32_t    last = (file->length > 0) ? (uint32_t)((end - 1u) / torrent.pieceLength) : first;
        uint32_t    piecesHad = 0;
        const char *label;

        if (storageGetFile(store, fileIndex++, &state) != STORAGE_OK) {
            continue;
        }
        if (state.isPadding) {
            padding++;
            continue;
        }
        dataLength += file->length;
        for (i = first; file->length > 0 && i <= last; i++) {
            if (messageBitfieldGet(have, i)) {
                verified += pieceOverlap(&torrent, i, file->offset, end);
                piecesHad++;
            }
        }
        bytesDone += verified;

        if (state.notRegular) {
            label = "NOT A FILE";
        } else if (verified == file->length) {
            label = "complete";
            filesComplete++;
        } else if (!state.existed) {
            label = "MISSING";
        } else {
            label = "partial";
        }
        formatBytes(verified, size, sizeof(size));
        formatBytes(file->length, total, sizeof(total));
        if (state.notRegular) {
            printf("  %-10s    --   ", label);
        } else {
            printf("  %-10s %5.1f%%  ", label, percentOf(verified, file->length));
        }
        printFilePath(&torrent, file);
        printf("\n             %s of %s verified", size, total);
        if (file->length > 0 && first == last) {
            printf("; piece %" PRIu32 ": %s", first, (piecesHad == 1u) ? "good" : "missing");
        } else if (file->length > 0) {
            printf("; pieces %" PRIu32 "-%" PRIu32 ": %" PRIu32 " of %" PRIu32 " good",
                   first, last, piecesHad, last - first + 1u);
        }
        printf("\n");
        if (state.existed && !state.notRegular && state.sizeOnDisk != file->length) {
            formatSize(state.sizeOnDisk, size, sizeof(size));
            printf("             size on disk is %s, %s than the torrent says\n", size,
                   (state.sizeOnDisk < file->length) ? "smaller" : "LARGER");
        }
        if (verified < file->length) {
            printf("             missing pieces: ");
            printMissing(have, first, last);
            printf("\n");
        }
        shown++;
    }
    if (padding > 0) {
        printf("  (%" PRIu32 " padding file%s not shown: never stored on disk)\n", padding,
               (padding == 1u) ? "" : "s");
    }

    formatSize(bytesDone, size, sizeof(size));
    formatSize(dataLength, total, sizeof(total));
    printf("\nSummary\n");
    printf("  Status:          %s\n", (haveCount == torrent.pieceCount) ? "COMPLETE" : "INCOMPLETE");
    printf("  Files:           %" PRIu32 " of %" PRIu32 " complete\n", filesComplete, shown);
    printf("  Pieces:          %" PRIu32 " of %" PRIu32 " verified (%.1f%%)\n", haveCount,
           torrent.pieceCount, percentOf(haveCount, torrent.pieceCount));
    printf("  Bytes:           %s of %s verified (%.1f%%)\n", size, total,
           percentOf(bytesDone, dataLength));
    if (haveCount < torrent.pieceCount) {
        printf("  Missing pieces:  %" PRIu32 ": ", torrent.pieceCount - haveCount);
        printMissing(have, 0, torrent.pieceCount - 1u);
        printf("\n");
    }
    result = (haveCount == torrent.pieceCount) ? 0 : 2;

done:
    storageClose(store);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    parserFree(context, &torrent);
    return result;
}

static int announceTorrent(parserContext *context, const parserAllocator *allocator,
                           const char *path, uint32_t timeoutSeconds, const char *caFile,
                           uint32_t mode, int64_t wanted, const char *outputDirectory,
                           const netBinding *binding, uint16_t listenPort, uint8_t seed)
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const parserTier *tier;
    parserTorrent     torrent;
    trackerConfig     config;
    trackerResult     result;
    uv_loop_t         loop;
    const char      **seen;
    peerAttempt      *attempts = NULL;
    uint8_t           infoHash[PARSER_SHA1_LENGTH];
    uint32_t          pieceCount;
    uint32_t          attemptCount = 0;
    uint32_t          attemptCapacity = 0;
    uint32_t          handshakes = 0;
    int               downloaded = 0;
    uint32_t         *ids = NULL;
    uint8_t          *buffer = NULL;
    uint8_t           randomBytes[12];
    uint64_t          length = 0;
    uint32_t          urlCount = 1;
    uint32_t          count = 0;
    uint32_t          succeeded = 0;
    uint32_t          i;
    uint32_t          j;
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
    config.port = (listenPort != 0) ? listenPort : 6881;
    config.caFile = caFile;
    config.binding = binding;

    uv_loop_init(&loop);
    if (mode == MODE_DOWNLOAD) {
        downloaded = (downloadTorrent(&loop, context, &torrent, &config, timeoutSeconds, wanted,
                                      outputDirectory, listenPort, seed) == 0);
        uv_loop_close(&loop);
        parserFree(context, &torrent);
        return downloaded ? 0 : -1;
    }
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
    memcpy(infoHash, torrent.infoHash, PARSER_SHA1_LENGTH);
    pieceCount = torrent.pieceCount;

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
            /* For -P, gather each tracker's peers before closing its id. */
            for (j = 0; mode != MODE_ANNOUNCE && status == TRACKER_OK && j < result.peerCount; j++) {
                if (!addPeer(allocator, &attempts, &attemptCount, &attemptCapacity,
                             &result.peers[j])) {
                    fprintf(stderr, "out of memory\n");
                    break;
                }
            }
            trackerClose(ids[i]);
        }
        allocator->free(ids);
    }

    trackerShutdown();

    if (mode != MODE_ANNOUNCE) {
        if (attemptCount == 0) {
            printf("\nNo peers to connect to.\n");
        } else if (peerInit(&loop, context, binding) == PEER_OK) {
            if (mode == MODE_PEERS) {
                handshakes = handshakePeers(&loop, attempts, attemptCount, infoHash,
                                            config.peerId, pieceCount, timeoutSeconds);
            }
            peerShutdown();
        }
        allocator->free(attempts);
    }

    uv_run(&loop, UV_RUN_DEFAULT);      /* let any closing handles finish */
    uv_loop_close(&loop);
    parserFree(context, &torrent);
    if (mode == MODE_PEERS) {
        return (handshakes > 0) ? 0 : -1;
    }
    return (succeeded > 0) ? 0 : -1;
}

/* ------------------------------------------------------------------------ */

/*
 * Minimal getopt(): Windows has none. 'spec' lists option letters, with ':'
 * after those that take a value ("-o file" or "-ofile"). Flags may be
 * grouped ("-pt"). Option parsing stops at the first non-option or "--".
 * Returns the option letter, '?' for an unknown option or missing value,
 * or -1 when there are no more options.
 */
static int         optionIndex = 1;     /* next argv element to examine */
static const char *optionValue;         /* value of the last option     */

static int nextOption(int argc, char **argv, const char *spec)
{
    static const char *cursor = NULL;   /* position within grouped flags */
    const char        *found;
    int                letter;

    if (cursor == NULL || *cursor == '\0') {
        if (optionIndex >= argc || argv[optionIndex][0] != '-' ||
            argv[optionIndex][1] == '\0') {
            return -1;
        }
        if (strcmp(argv[optionIndex], "--") == 0) {
            optionIndex++;
            return -1;
        }
        cursor = argv[optionIndex++] + 1;
    }

    letter = (unsigned char)*cursor++;
    found = (letter != ':') ? strchr(spec, letter) : NULL;
    if (found == NULL) {
        fprintf(stderr, "unknown option -%c\n", letter);
        return '?';
    }
    if (found[1] == ':') {
        if (*cursor != '\0') {                  /* "-ofile" */
            optionValue = cursor;
        } else if (optionIndex < argc) {        /* "-o file" */
            optionValue = argv[optionIndex++];
        } else {
            fprintf(stderr, "option -%c needs a value\n", letter);
            return '?';
        }
        cursor = NULL;
    }
    return letter;
}

#ifdef _WIN32
/*
 * Windows passes main() its arguments in the ANSI code page, which cannot
 * represent every file name. Rebuild argv from the UTF-16 command line as
 * UTF-8, which is what the library expects for paths and torrent names.
 */
static void freeArguments(const parserAllocator *allocator, char **arguments)
{
    int i;

    if (arguments != NULL) {
        for (i = 0; arguments[i] != NULL; i++) {
            allocator->free(arguments[i]);
        }
        allocator->free((void *)arguments);
    }
}

static char **utf8Arguments(const parserAllocator *allocator, int *count)
{
    wchar_t **wide;
    char    **arguments;
    int       total;
    int       bytes;
    int       i;

    wide = CommandLineToArgvW(GetCommandLineW(), &total);
    if (wide == NULL) {
        return NULL;
    }
    arguments = allocator->realloc(NULL, sizeof(*arguments) * ((size_t)total + 1u));
    if (arguments == NULL) {
        LocalFree(wide);
        return NULL;
    }
    memset(arguments, 0, sizeof(*arguments) * ((size_t)total + 1u));

    for (i = 0; i < total; i++) {
        bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        arguments[i] = (bytes > 0) ? allocator->realloc(NULL, (size_t)bytes) : NULL;
        if (arguments[i] == NULL ||
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arguments[i], bytes, NULL, NULL) == 0) {
            freeArguments(allocator, arguments);
            LocalFree(wide);
            return NULL;
        }
    }
    LocalFree(wide);
    *count = total;
    return arguments;
}
#endif

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <file.torrent>\n"
            "       %s -o <output.torrent> [options] <file>...\n"
            "       %s -t [-T seconds] [-C ca.pem] [-I interface] <file.torrent>\n"
            "       %s -P [-T seconds] [-C ca.pem] [-I interface] <file.torrent>\n"
            "       %s -D [-S] [-L port] [-O dir] [-i piece] [-T seconds] [-C ca.pem]\n"
            "            [-I interface] <file.torrent>\n"
            "       %s -V [-O dir] <file.torrent>\n"
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
            "  -P        announce, then contact each peer found (up to 200): handshake,\n"
            "            send interested, report its pieces and whether it unchokes us\n"
            "  -D        download the torrent from the peers the trackers give (up to 50\n"
            "            at once), uploading to peers too; trackers are re-announced to\n"
            "            as they ask; Ctrl-C stops and tells them; re-run to resume\n"
            "  -S        with -D: keep running once complete, seeding until Ctrl-C\n"
            "  -L port   with -D: accept peer connections on this port, or the next\n"
            "            free one of the 9 after it (default 6881; 0: do not listen)\n"
            "  -V        verify downloaded data against the torrent (no network, changes\n"
            "            nothing): each file's state, bytes verified, missing pieces;\n"
            "            exit status 0 if complete, 2 if not, 1 on error\n"
            "  -O dir    with -D or -V: where the files are (default: current directory)\n"
            "  -i piece  with -D: download only this piece\n"
            "  -T secs   announce and handshake timeout (default 15)\n"
            "  -C file   trust the CA certificates in this PEM file for HTTPS\n"
            "            trackers (default: the system's certificate store)\n"
            "  -I iface  with -t, -P or -D: send all tracker and peer traffic through\n"
            "            this network interface, given by name (tun0, \"Ethernet 2\")\n"
            "            or by one of its IP addresses; DNS lookups are not covered\n",
            program, program, program, program, program, program);
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
    uint8_t         announceMode = 0;
    uint8_t         verify = 0;         /* -V */
    uint8_t         seed = 0;           /* -S */
    uint32_t        listenPort = 6881;  /* -L */
    uint8_t         downloadOnly = 0;   /* -S or -L given */
    const char     *interfaceSpec = NULL;   /* -I */
    netBinding      binding;
    uint8_t         directoryGiven = 0; /* -O */
    uint32_t        mode = MODE_ANNOUNCE;
    int64_t         wanted = -1;            /* -i piece index */
    const char     *outputDirectory = "";   /* -O directory   */
    uint8_t         announceOnly = 0;   /* -T or -C: only make sense with -t */
    const char     *caFile = NULL;
    uint32_t        timeoutSeconds = defaultAnnounceTimeout;
    unsigned long   number;
    char           *end;
    int32_t         result;
    int             option;
    int             status = -1;

#ifndef _WIN32
    /* A peer may close its connection while we are writing to it; that must
     * be an error from the write, not a signal that ends the program. */
    signal(SIGPIPE, SIG_IGN);
#endif

    memset(&options, 0, sizeof(options));

#ifdef _WIN32
    /* Torrent names and paths are UTF-8; show them correctly in the console,
     * and take the arguments as UTF-8 too. */
    SetConsoleOutputCP(CP_UTF8);
    argv = utf8Arguments(&allocator, &argc);
    if (argv == NULL) {
        fprintf(stderr, "could not read the command line\n");
        return EXIT_FAILURE;
    }
#endif

    result = parserInit(&context, &allocator);
    if (result != PARSER_OK) {
        fprintf(stderr, "parserInit: %s\n", parserErrorString(result));
        goto cleanup;
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

    while ((option = nextOption(argc, argv, "o:a:w:c:n:l:ptPDVSL:O:i:T:C:I:h")) != -1) {
        switch (option) {
        case 'o':
            outputPath = optionValue;
            break;
        case 'a':
            trackers[options.trackerCount++] = optionValue;
            createOnly = 1;
            break;
        case 'w':
            webSeeds[options.webSeedCount++] = optionValue;
            createOnly = 1;
            break;
        case 'c':
            options.comment = optionValue;
            createOnly = 1;
            break;
        case 'n':
            options.name = optionValue;
            createOnly = 1;
            break;
        case 'l':
            if (parsePieceLength(optionValue, &options.pieceLength) != 0) {
                fprintf(stderr, "invalid piece length '%s': use a power of two "
                        "from 16K to 64M\n", optionValue);
                goto cleanup;
            }
            createOnly = 1;
            break;
        case 'p':
            options.isPrivate = 1;
            createOnly = 1;
            break;
        case 't':
            announceMode = 1;
            break;
        case 'P':
            announceMode = 1;
            mode = MODE_PEERS;
            break;
        case 'D':
            announceMode = 1;
            mode = MODE_DOWNLOAD;
            break;
        case 'S':
            seed = 1;
            downloadOnly = 1;
            break;
        case 'L': {
            char         *portEnd;
            unsigned long value = strtoul(optionValue, &portEnd, 10);

            if (*portEnd != '\0' || portEnd == optionValue || value > 65535u) {
                fprintf(stderr, "-L: not a port number: %s\n", optionValue);
                goto cleanup;
            }
            listenPort = (uint32_t)value;
            downloadOnly = 1;
            break;
        }
        case 'V':
            verify = 1;
            break;
        case 'O':
            outputDirectory = optionValue;
            directoryGiven = 1;
            break;
        case 'i':
            number = strtoul(optionValue, &end, 10);
            if (*optionValue < '0' || *optionValue > '9' || *end != '\0' || number > UINT32_MAX) {
                fprintf(stderr, "invalid piece index '%s'\n", optionValue);
                goto cleanup;
            }
            wanted = (int64_t)number;
            announceOnly = 1;
            break;
        case 'T':
            number = strtoul(optionValue, &end, 10);
            if (*optionValue < '0' || *optionValue > '9' || *end != '\0' || number == 0 || number > 3600u) {
                fprintf(stderr, "invalid timeout '%s': use 1 to 3600 seconds\n", optionValue);
                goto cleanup;
            }
            timeoutSeconds = (uint32_t)number;
            announceOnly = 1;
            break;
        case 'C':
            caFile = optionValue;
            announceOnly = 1;
            break;
        case 'I':
            interfaceSpec = optionValue;
            announceOnly = 1;
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

    if ((announceOnly && !announceMode) || (directoryGiven && !verify && mode != MODE_DOWNLOAD) ||
        (downloadOnly && mode != MODE_DOWNLOAD)) {
        usage(argv[0]);
        goto cleanup;
    }

    if (verify) {
        if (announceMode || announceOnly || outputPath != NULL || createOnly || argc - optionIndex != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        status = verifyTorrent(&context, &allocator, argv[optionIndex], outputDirectory);
    } else if (announceMode) {
        if (outputPath != NULL || createOnly || argc - optionIndex != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        if ((wanted >= 0 || outputDirectory[0] != '\0' || downloadOnly) && mode != MODE_DOWNLOAD) {
            usage(argv[0]);
            goto cleanup;
        }
        memset(&binding, 0, sizeof(binding));
        if (interfaceSpec != NULL) {
            char message[NET_MESSAGE_LENGTH];
            char description[NET_DESCRIPTION_LENGTH];

            if (netBindingResolve(interfaceSpec, &binding, message, sizeof(message)) != NET_OK) {
                fprintf(stderr, "-I %s: %s\n", interfaceSpec, message);
                goto cleanup;
            }
            netBindingDescribe(&binding, description, sizeof(description));
            printf("Interface:  %s\n", description);
            if (message[0] != '\0') {
                printf("            %s\n", message);
            }
            printf("            (DNS lookups still use the system's resolver)\n");
        }
        status = announceTorrent(&context, &allocator, argv[optionIndex], timeoutSeconds, caFile,
                                 mode, wanted, outputDirectory, &binding, (uint16_t)listenPort,
                                 seed);
    } else if (outputPath != NULL) {
        if (optionIndex >= argc) {
            usage(argv[0]);
            goto cleanup;
        }
        options.createdBy = "torrentinfo";
        options.creationDate = (int64_t)time(NULL);
        options.hasCreationDate = 1;
        status = createTorrent(&context, outputPath, &options, argv + optionIndex, argc - optionIndex);
    } else {
        if (createOnly || argc - optionIndex != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        status = inspectTorrent(&context, &allocator, argv[optionIndex]);
    }

cleanup:
    allocator.free((void *)trackers);
    allocator.free((void *)webSeeds);
#ifdef _WIN32
    freeArguments(&allocator, argv);
#endif

    if (liveAllocations != 0) {
        fprintf(stderr, "warning: %" PRIu64 " allocation(s) not freed\n", liveAllocations);
        status = -1;
    }
    if (status == 2) {
        return 2;                                   /* -V: verified, but incomplete */
    }
    return (status == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
