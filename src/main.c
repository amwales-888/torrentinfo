/*
 * main.c - Example driver for the torrent parser and encoder.
 *
 * Inspect:  ./torrentinfo file.torrent
 * Create:   ./torrentinfo -o out.torrent [options] file1 [file2 ...]
 * Announce: ./torrentinfo -t [-T seconds] [-C ca.pem] file.torrent
 * Peers:    ./torrentinfo -P [-T seconds] [-C ca.pem] file.torrent
 * Download: ./torrentinfo -D [-S] [-L port] [-O dir] [-i piece] [-T seconds] [-C ca.pem]
 *           [-I interface] (file.torrent | magnet-link)...   (session.c does the work)
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

#include "blocklist.h"
#include "config.h"
#include "datafile.h"
#include "download.h"
#include "magnet.h"
#include "encoder.h"
#include "net.h"
#include "parser.h"
#include "peer.h"
#include "portmap.h"
#include "selection.h"
#include "session.h"
#include "settings.h"
#include "piece.h"
#include "platform.h"
#include "storage.h"
#include "torrent.h"
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

/* Read a .torrent (or another small file), saying what went wrong. */
static int readFile(const parserAllocator *allocator, const char *path,
                    uint8_t **bufferOut, uint64_t *lengthOut)
{
    int32_t systemError = 0;
    int32_t result = datafileRead(allocator, path, maxTorrentSize, bufferOut, lengthOut,
                                  &systemError);
    uint64_t size = 0;
    uint8_t  isRegular;

    if (result == DATAFILE_OK) {
        return 0;
    }
    if (result == DATAFILE_EIO) {
        fprintf(stderr, "%s: %s\n", path, uv_strerror(systemError));
    } else if (result == DATAFILE_ESIZE) {
        (void)platformFileStat(path, &size, &isRegular);
        fprintf(stderr, "%s: unexpected file size (%" PRIu64 " bytes)\n", path, size);
    } else {
        fprintf(stderr, "%s: %s\n", path, datafileErrorString(result));
    }
    return -1;
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

/* ------------------------------------------------------------------------ */
/* What the settings file and the command line decided, passed down to the  */
/* modes instead of being kept in globals.                                  */
/* ------------------------------------------------------------------------ */

typedef struct cliSettings {
    settingsValues values;          /* defaults <- settings file <- options        */
    blocklist     *blocklist;       /* peers never to talk to, or NULL             */
    selection      choice;          /* -f, -F                                      */
    uint8_t        recheckAll;      /* -R: ignore the resume file once             */
} cliSettings;

/* ------------------------------------------------------------------------ */
/* Choosing files (-f, -F)                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Say what -f and -F chose (status and result from selectionApply). -1 if
 * the choice is wrong or empty.
 */
static int printChoice(const selection *choice, int32_t status, const selectionResult *result)
{
    uint32_t i;
    char     size[32];
    char     total[32];

    for (i = 0; i < choice->patternCount; i++) {
        if (result->unmatched & (1u << i)) {
            printf("-F %s: matches no file\n", choice->patterns[i]);
        }
    }
    switch (status) {
    case SELECTION_OK:
        break;
    case SELECTION_ERANGE:
        if (result->files == 1u) {
            fprintf(stderr, "-f: no file %" PRIu64 ": the torrent has only file 1\n",
                    result->number);
        } else {
            fprintf(stderr, "-f: no file %" PRIu64 ": the torrent has files 1 to %" PRIu32 "\n",
                    result->number, result->files);
        }
        return -1;
    case SELECTION_ENONE:
        fprintf(stderr, "No files chosen: nothing to do.\n");
        return -1;
    default:
        fprintf(stderr, "-f: %s\n", selectionErrorString(status));
        return -1;
    }
    formatSize(result->chosenBytes, size, sizeof(size));
    formatSize(result->allBytes, total, sizeof(total));
    printf("Chosen:     %" PRIu32 " of %" PRIu32 " file%s, %s of %s\n", result->chosen,
           result->files, (result->files == 1u) ? "" : "s", size, total);
    return 0;
}

/* Fill wanted[] (one byte per file, padding included) from -f and -F, and
 * say what was chosen. -1 (after a message) if the choice is wrong or empty. */
static int chooseFiles(const selection *choice, const parserTorrent *torrent, uint8_t *wanted)
{
    selectionResult result;

    return printChoice(choice, selectionApply(choice, torrent, wanted, &result), &result);
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
    uint32_t          fileNumber = 0;

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
    printf("  %5s  %16s  %16s  %s\n", "#", "Offset", "Size", "Path");
    TAILQ_FOREACH(file, &torrent->files, entries) {
        if (file->isPadding) {
            printf("  %5s", "");
        } else {
            printf("  %5" PRIu32, ++fileNumber);
        }
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
    int32_t systemError = 0;
    int32_t result = datafileWrite(path, data, length, &systemError);

    if (result == DATAFILE_OK) {
        return 0;
    }
    fprintf(stderr, "%s: %s\n", path,
            (result == DATAFILE_EIO) ? uv_strerror(systemError) : datafileErrorString(result));
    return -1;
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
        trackerPeerAddress(&attempts[i].peer, &address);
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
/*                torrent-or-magnet...                                      */
/*                                                                          */
/* A session (session.c) runs every torrent given, sharing one listening    */
/* port, the DHT node and the speed limits; each torrent is a torrent task  */
/* (torrent.c). This prints what they report, and a progress line for each  */
/* every two seconds (every five while fetching metadata). With more than   */
/* one torrent, each line starts with the torrent's number: [1], [2], ...   */
/* ------------------------------------------------------------------------ */

#define REPORT_TICK_MS      250u        /* how often reports are due          */
#define REPORT_PROGRESS_MS  2000u
#define REPORT_FETCH_MS     5000u

struct downloadRun;

typedef struct cliTorrent {
    struct downloadRun *run;
    torrentTask    *task;
    const char     *source;             /* the .torrent's path, or the link   */
    char            tag[16];            /* "" or "[2] "                       */
    uint8_t         complete;           /* everything wanted is on disk       */
    uint8_t         failed;
    uint8_t         finished;           /* onFinished has run                 */
    uint8_t         ended;
    uint32_t        interval;           /* ms between reports; 0: none        */
    uint8_t         fetching;           /* metadata reports, not progress     */
    uint64_t        nextReport;         /* uv_now() of the next report        */
    uint64_t        startedAt;          /* uv_hrtime() when the files were ready */
    uint64_t        lastReport;         /* uv_now() of the last progress line */
    uint64_t        reportReceived;     /* bytes received at that line        */
    uint64_t        reportSent;         /* bytes uploaded at that line        */
    uint32_t        reportBlocked;      /* peers refused, as last reported    */
} cliTorrent;

typedef struct downloadRun {
    uv_loop_t      *loop;
    session        *owner;
    const cliSettings *cli;
    int64_t         onlyPiece;          /* -i, or -1                          */
    uint8_t         seed;               /* -S: keep running once complete     */
    cliTorrent     *torrents;
    uint32_t        count;
    uint32_t        ended;              /* torrents that have ended           */
    uint8_t         stopRequested;      /* Ctrl-C (or all done): sessionStop  */
    uv_timer_t      timer;              /* reports                            */
    uv_signal_t     interrupt;          /* Ctrl-C                             */
#ifndef _WIN32
    uv_signal_t     terminate;          /* SIGTERM                            */
#endif
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

/* Reports from now on: progress (every 2 s) or metadata (every 5 s). */
static void scheduleReports(cliTorrent *t, int fetching)
{
    uint64_t now = uv_now(t->run->loop);

    t->fetching = (uint8_t)fetching;
    t->interval = fetching ? REPORT_FETCH_MS : REPORT_PROGRESS_MS;
    t->nextReport = now + t->interval;
    t->lastReport = now;
}

static void reportProgress(cliTorrent *t, uint64_t now)
{
    torrentStatus    status;
    downloadProgress progress;
    uint64_t         elapsed = now - t->lastReport;
    char             down[32];
    char             up[32];

    torrentGetStatus(t->task, &status);
    if (!status.hasDownloadProgress || elapsed == 0) {
        return;
    }
    progress = status.download;
    if (progress.checking) {
        if (progress.checkTotal > 0) {
            printf("  %schecking files: %" PRIu32 " of %" PRIu32 " pieces (%.0f%%)\n", t->tag,
                   progress.checkedPieces, progress.checkTotal,
                   100.0 * progress.checkedPieces / progress.checkTotal);
        }
        t->lastReport = now;
        return;
    }
    formatBytes((progress.bytesReceived - t->reportReceived) * 1000u / elapsed, down,
                sizeof(down));
    formatBytes((progress.bytesUploaded - t->reportSent) * 1000u / elapsed, up, sizeof(up));
    if (progress.seeding) {
        printf("  %sseeding  up %9s/s  peers %" PRIu32 " connected (%" PRIu32 " incoming, %"
               PRIu32 " encrypted), %" PRIu32 " interested, uploading to %" PRIu32 "\n",
               t->tag, up, progress.peersConnected, progress.peersIncoming,
               progress.peersEncrypted, progress.peersInterested, progress.peersUploading);
    } else {
        printf("  %s%5.1f%%  %" PRIu32 "/%" PRIu32 "  down %9s/s  up %9s/s  peers %" PRIu32
               " (%" PRIu32 " in, %" PRIu32 " enc), %" PRIu32 " unchoking us, %" PRIu32
               " sending, uploading to %" PRIu32 "%s\n", t->tag,
               (progress.wanted > 0) ? 100.0 * progress.done / progress.wanted : 100.0,
               progress.done, progress.wanted, down, up, progress.peersConnected,
               progress.peersIncoming, progress.peersEncrypted, progress.peersUnchoking,
               progress.peersDownloading,
               progress.peersUploading, progress.endgame ? "  [endgame]" : "");
        if (progress.webSeeds > 0 || progress.bytesFromWeb > 0) {
            char fromWeb[32];

            formatBytes(progress.bytesFromWeb, fromWeb, sizeof(fromWeb));
            printf("          %sweb seeds: %" PRIu32 " in use, %" PRIu32 " fetching, %s received\n",
                   t->tag, progress.webSeeds, progress.webSeedsActive, fromWeb);
        }
    }
    if (progress.peersBlocked != t->reportBlocked) {
        printf("  %sblocklist: %" PRIu32 " peer address%s refused so far\n", t->tag,
               progress.peersBlocked, (progress.peersBlocked == 1u) ? "" : "es");
        t->reportBlocked = progress.peersBlocked;
    }
    t->lastReport = now;
    t->reportReceived = progress.bytesReceived;
    t->reportSent = progress.bytesUploaded;
}

static void reportFetch(cliTorrent *t)
{
    torrentStatus status;

    torrentGetStatus(t->task, &status);
    if (!status.hasMetadataProgress) {
        return;
    }
    printf("  %sfetching the metadata: %" PRIu32 " peer(s) connected, %" PRIu32 " offering it",
           t->tag, status.metadata.peersConnected, status.metadata.peersWithMetadata);
    if (status.dhtRunning) {
        printf(", %" PRIu32 " DHT nodes", status.dhtNodes);
    }
    if (status.metadata.pieces > 0) {
        printf(", %" PRIu32 " of %" PRIu32 " pieces", status.metadata.piecesDone,
               status.metadata.pieces);
    }
    if (status.metadata.peersBlocked > 0) {
        printf(", %" PRIu32 " blocked", status.metadata.peersBlocked);
    }
    printf("\n");
}

static void onReportTick(uv_timer_t *timer)
{
    downloadRun *run = timer->data;
    uint64_t     now = uv_now(timer->loop);
    uint32_t     i;

    for (i = 0; i < run->count; i++) {
        cliTorrent *t = &run->torrents[i];

        if (t->ended || t->interval == 0 || now < t->nextReport) {
            continue;
        }
        t->nextReport += t->interval;
        if (t->nextReport <= now) {
            t->nextReport = now + t->interval;      /* fell behind: no catching up */
        }
        if (t->fetching) {
            reportFetch(t);
        } else {
            reportProgress(t, now);
        }
    }
    fflush(stdout);
}

static void stopWatching(downloadRun *run)
{
    uv_timer_stop(&run->timer);
    uv_signal_stop(&run->interrupt);
#ifndef _WIN32
    uv_signal_stop(&run->terminate);
#endif
}

static void onInterrupt(uv_signal_t *signal, int number)
{
    downloadRun     *run = signal->data;
    downloadProgress progress;
    char             sent[32];
    uint32_t         i;

    (void)number;
    if (run->stopRequested ||
        (run->count == 1 && torrentState(run->torrents[0].task) == TORRENT_STATE_STOPPING)) {
        printf("\nStopping now.\n");
        fflush(stdout);
        run->stopRequested = 1;
        sessionStop(run->owner);                    /* second Ctrl-C: do not wait */
        return;
    }
    for (i = 0; i < run->count; i++) {
        cliTorrent *t = &run->torrents[i];
        download   *transfer = torrentDownload(t->task);

        if (t->ended || torrentState(t->task) == TORRENT_STATE_STOPPING) {
            continue;
        }
        if (transfer != NULL) {
            downloadGetProgress(transfer, &progress);
            formatBytes(progress.bytesUploaded, sent, sizeof(sent));
            if (!t->complete && !progress.seeding) {
                printf("\n%sInterrupted: %" PRIu32 " of %" PRIu32 " pieces on disk, %s uploaded;"
                       " run again to resume.\n", t->tag, progress.done, progress.wanted, sent);
            } else {
                printf("\n%sStopped seeding: %s uploaded in total.\n", t->tag, sent);
            }
        } else {
            printf("\n%sInterrupted.\n", t->tag);
        }
    }
    fflush(stdout);
    run->stopRequested = 1;
    sessionStop(run->owner);
}

/* --- What the session reports ------------------------------------------- */

static void onSessionNotice(void *privateArg, session *owner, const sessionNotice *notice)
{
    (void)privateArg;
    (void)owner;
    switch (notice->code) {
    case SESSION_NOTICE_LISTENING:
        printf("Listening:  port %u\n", (unsigned)notice->number);
        break;
    case SESSION_NOTICE_LISTEN_FAILED:
        printf("Listening:  could not open ports %u-%u; outgoing connections only\n",
               (unsigned)notice->number, (unsigned)notice->number2);
        break;
    case SESSION_NOTICE_DHT_FAILED:
        printf("DHT:        could not start; trackers only\n");
        break;
    case SESSION_NOTICE_TRACKERS_FAILED:
        printf("trackerInit: %s\n", trackerStatusString(notice->status));
        break;
    default:
        break;
    }
    fflush(stdout);
}

static void onSessionPortmap(void *privateArg, session *owner, const portmapResult *result)
{
    (void)privateArg;
    (void)owner;
    if (result->status == PORTMAP_OK) {
        printf("  router %s forwards port %u to us (%s)", result->router,
               (unsigned)result->externalPort, portmapMethodName(result->method));
        if (result->externalAddress[0] != '\0') {
            printf("; our public address is %s", result->externalAddress);
        }
        printf("\n");
    } else {
        printf("  port forwarding: %s; peers outside your network may not reach you\n",
               result->message);
    }
    fflush(stdout);
}

static void onSessionEnded(void *privateArg, session *owner)
{
    (void)owner;
    stopWatching(privateArg);
}

/* --- What each torrent task reports ------------------------------------- */

static void onTaskState(void *privateArg, torrentTask *task, uint32_t state)
{
    cliTorrent *t = privateArg;

    (void)task;
    if (state == TORRENT_STATE_METADATA) {
        printf("\n%sFetching the torrent's metadata from peers; Ctrl-C to stop\n", t->tag);
        scheduleReports(t, 1);
    } else if (state == TORRENT_STATE_CHECKING) {
        printf("\n%sChecking files...\n", t->tag);
        scheduleReports(t, 0);                      /* checking progress */
    }
    fflush(stdout);
}

static void onTaskNotice(void *privateArg, torrentTask *task, const torrentNotice *notice)
{
    cliTorrent *t = privateArg;

    switch (notice->code) {
    case TORRENT_NOTICE_CACHED:
        printf("%sUsing the torrent fetched before: %s\n", t->tag, notice->path);
        break;
    case TORRENT_NOTICE_METADATA:
        printf("%sGot the metadata (%" PRIu64 " bytes).\n", t->tag, notice->number);
        break;
    case TORRENT_NOTICE_SAVED:
        printf("%sSaved the torrent as %s\n", t->tag, notice->path);
        break;
    case TORRENT_NOTICE_DHT_FAILED:
        if (torrentState(task) == TORRENT_STATE_METADATA) {
            printf("%sDHT: could not start\n", t->tag);
        } else {
            printf("%sDHT:        could not start; trackers only\n", t->tag);
        }
        break;
    case TORRENT_NOTICE_NO_TRACKERS:
        printf("%sThe torrent lists no trackers.\n", t->tag);
        break;
    case TORRENT_NOTICE_ANNOUNCE_FAILED:
        printf("%sCould not start announcing.\n", t->tag);
        break;
    case TORRENT_NOTICE_LEAVING:
        printf("%sTelling the trackers we are leaving...\n", t->tag);
        break;
    default:
        break;
    }
    fflush(stdout);
}

static void onTaskEvent(void *privateArg, torrentTask *task, const eventInfo *event)
{
    cliTorrent *t = privateArg;

    (void)task;
    printf("  %s%s\n", t->tag, event->message);
    fflush(stdout);
}

static void onTaskTracker(void *privateArg, torrentTask *task, const char *url, uint32_t event,
                          int32_t status, const trackerResult *result)
{
    static const char *events[] = { "", " (started)", " (completed)", " (stopped)" };
    cliTorrent        *t = privateArg;

    (void)task;
    printf("  %stracker %s%s: ", t->tag, url, events[event <= TRACKER_EVENT_STOPPED ? event : 0]);
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

static void onTaskDht(void *privateArg, torrentTask *task, uint32_t peers, uint32_t nodes,
                      uint32_t tableNodes)
{
    cliTorrent *t = privateArg;

    (void)task;
    printf("  %sDHT: %" PRIu32 " peer%s found (asked %" PRIu32 " nodes; %" PRIu32
           " in the routing table)\n", t->tag, peers, (peers == 1u) ? "" : "s", nodes,
           tableNodes);
    fflush(stdout);
}

static void onTaskFilesChosen(void *privateArg, torrentTask *task, int32_t status,
                              const selectionResult *result)
{
    cliTorrent *t = privateArg;

    (void)task;
    (void)printChoice(&t->run->cli->choice, status, result);
}

static void onTaskReady(void *privateArg, torrentTask *task)
{
    cliTorrent      *t = privateArg;
    download        *transfer = torrentDownload(task);
    downloadProgress progress;

    downloadGetProgress(transfer, &progress);
    printf("%sSaving to:  %s\n", t->tag, downloadRoot(transfer));
    if (progress.resumed > 0) {
        printf("%sResuming:   %" PRIu32 " of %" PRIu32 "%s pieces already on disk and verified\n",
               t->tag, progress.resumed, progress.wanted,
               (progress.wanted < progress.pieceCount) ? " wanted" : "");
    }
    if (progress.resumeUsed) {
        printf("%s            fast resume: %" PRIu32 " piece%s taken from the resume file "
               "without checking", t->tag, progress.resumeTrusted,
               (progress.resumeTrusted == 1u) ? "" : "s");
        if (progress.resumeChanged > 0) {
            printf("; %" PRIu32 " file%s changed since, so checked", progress.resumeChanged,
                   (progress.resumeChanged == 1u) ? "" : "s");
        }
        printf("\n");
    }
    if (progress.resumeBlocks > 0) {
        printf("%s            %" PRIu32 " block%s of unfinished pieces kept from last time\n",
               t->tag, progress.resumeBlocks, (progress.resumeBlocks == 1u) ? "" : "s");
    }
    if (progress.done < progress.wanted) {
        printf("\n%sDownloading %" PRIu32 " piece(s); Ctrl-C to stop\n", t->tag,
               progress.wanted - progress.done);
    } else if (t->run->seed) {
        t->complete = 1;                            /* complete already: seeding only */
        printf("\n%sSeeding; Ctrl-C to stop\n", t->tag);
    }
    fflush(stdout);
    t->startedAt = uv_hrtime();
    scheduleReports(t, 0);
}

static void onTaskComplete(void *privateArg, torrentTask *task)
{
    cliTorrent      *t = privateArg;
    download        *transfer = torrentDownload(task);
    downloadProgress progress;
    double           seconds = (double)(uv_hrtime() - t->startedAt) / 1e9;
    char             size[32];
    char             wasted[32];
    char             sent[32];

    t->complete = 1;
    downloadGetProgress(transfer, &progress);
    formatBytes(progress.bytesReceived, size, sizeof(size));
    formatBytes(progress.bytesWasted, wasted, sizeof(wasted));
    formatBytes(progress.bytesUploaded, sent, sizeof(sent));
    printf("\n%sDownload complete: %s\n  %s%" PRIu32 " piece(s), %s received in %.1f s"
           " (%.1f KiB/s)", t->tag, downloadRoot(transfer), t->tag,
           progress.done - progress.resumed, size, seconds,
           (seconds > 0.0) ? (double)progress.bytesReceived / 1024.0 / seconds : 0.0);
    if (progress.bytesWasted > 0 || progress.hashFailures > 0) {
        printf("; %s wasted, %" PRIu32 " piece(s) failed verification", wasted,
               progress.hashFailures);
    }
    printf("; %s uploaded\n", sent);
    fflush(stdout);
}

static void onTaskFinished(void *privateArg, torrentTask *task, int32_t status,
                           const char *message)
{
    cliTorrent      *t = privateArg;
    downloadProgress progress;

    t->finished = 1;
    downloadGetProgress(torrentDownload(task), &progress);
    if (status == DOWNLOAD_OK && !t->complete) {
        printf("\n%sNothing to download: the files are already complete.\n", t->tag);
        t->complete = 1;
    } else if (status != DOWNLOAD_OK) {
        printf("\n%sDownload failed: %s (%" PRIu32 " of %" PRIu32 " pieces on disk)\n", t->tag,
               message, progress.done, progress.wanted);
        t->failed = 1;
    }
    fflush(stdout);
}

static void onTaskEnded(void *privateArg, torrentTask *task, int32_t result, int32_t detail)
{
    cliTorrent  *t = privateArg;
    downloadRun *run = t->run;

    switch (result) {
    case TORRENT_ENOSOURCES:
        printf("%sThe magnet link names no trackers or peers, and DHT is off (-X): there is\n"
               "no way to find peers.\n", t->tag);
        break;
    case TORRENT_EMETADATA:
        printf("%sCould not get the metadata.\n", t->tag);
        break;
    case TORRENT_EPARSE:
        fprintf(stderr, "%sthe fetched torrent does not parse: %s\n", t->tag,
                parserErrorString(detail));
        break;
    case TORRENT_ENOPIECE:
        printf("\n%sPiece %" PRId64 " does not exist: the torrent has %" PRIu32 " pieces.\n",
               t->tag, run->onlyPiece, torrentMetainfo(task)->pieceCount);
        break;
    case TORRENT_EDOWNLOAD:
        if (!t->finished) {
            printf("%sCould not start the download: %s\n", t->tag,
                   downloadStatusString(detail));
        }
        t->failed = 1;
        break;
    case TORRENT_ENOMEM:
        fprintf(stderr, "%sout of memory\n", t->tag);
        break;
    default:
        break;
    }
    fflush(stdout);
    t->ended = 1;
    t->interval = 0;
    run->ended++;
    if (run->ended == run->count && !run->stopRequested) {
        run->stopRequested = 1;                     /* all done: close the session */
        sessionStop(run->owner);
    }
}

/*
 * Download (and with -S seed) the torrents and magnet links in sources[]
 * with one session, until they are done or Ctrl-C. 0 if everything wanted
 * of every torrent is on disk.
 */
static int runSession(parserContext *context, const cliSettings *cli,
                      const sessionOptions *shared, const torrentOptions *each,
                      char **sources, uint32_t count)
{
    static const sessionEvents sessionCallbacks = {
        onSessionNotice, onSessionPortmap, onSessionEnded
    };
    static const torrentEvents taskCallbacks = {
        onTaskState, onTaskNotice, onTaskEvent, onTaskTracker, onTaskDht,
        onTaskFilesChosen, onTaskReady, onTaskComplete, onTaskFinished, onTaskEnded
    };
    uv_loop_t   loop;
    downloadRun run;
    int32_t     status;
    uint32_t    i;
    int         ok = 1;

    memset(&run, 0, sizeof(run));
    run.torrents = context->allocator.realloc(NULL, count * sizeof(*run.torrents));
    if (run.torrents == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    memset(run.torrents, 0, count * sizeof(*run.torrents));
    uv_loop_init(&loop);
    run.loop = &loop;
    run.cli = cli;
    run.onlyPiece = each->onlyPiece;
    run.seed = each->seed;
    uv_timer_init(&loop, &run.timer);
    run.timer.data = &run;
    uv_signal_init(&loop, &run.interrupt);
    run.interrupt.data = &run;
#ifndef _WIN32
    uv_signal_init(&loop, &run.terminate);
    run.terminate.data = &run;
#endif

    status = sessionBegin(&loop, context, shared, &sessionCallbacks, &run, &run.owner);
    if (status == SESSION_EPEER) {
        printf("Could not start the peer module.\n");
    } else if (status != SESSION_OK) {
        fprintf(stderr, "%s\n", sessionStatusString(status));
    } else {
        for (i = 0; i < count; i++) {
            cliTorrent    *t = &run.torrents[i];
            torrentOptions options = *each;
            uint8_t       *buffer = NULL;
            uint64_t       length = 0;
            parserTorrent  torrent;

            t->run = &run;
            t->source = sources[i];
            if (count > 1) {
                snprintf(t->tag, sizeof(t->tag), "[%" PRIu32 "] ", i + 1u);
            }
            if (magnetIs(sources[i])) {
                magnetLink link;

                if (magnetParse(context, sources[i], &link) != MAGNET_OK) {
                    fprintf(stderr, "%snot a usable magnet link (it needs xt=urn:btih:...)\n",
                            t->tag);
                    ok = 0;
                    break;
                }
                magnetFree(context, &link);
                options.magnet = sources[i];
            } else {
                if (readFile(&context->allocator, sources[i], &buffer, &length) != 0) {
                    ok = 0;
                    break;
                }
                status = parserParse(context, buffer, length, &torrent);
                if (status != PARSER_OK) {
                    fprintf(stderr, "%s: parse failed: %s\n", sources[i],
                            parserErrorString(status));
                    context->allocator.free(buffer);
                    ok = 0;
                    break;
                }
                parserFree(context, &torrent);
                options.data = buffer;
                options.length = length;
            }
            if (count > 1) {
                printf("%s%s\n", t->tag, sources[i]);
            }
            status = sessionAddTorrent(run.owner, &options, &taskCallbacks, t, &t->task);
            if (buffer != NULL) {
                context->allocator.free(buffer);    /* the task keeps a copy */
            }
            if (status != SESSION_OK) {
                fprintf(stderr, "%s%s: %s\n", t->tag, sources[i], sessionStatusString(status));
                ok = 0;
                break;
            }
        }
        run.count = i;                              /* the ones added */
        for (i = 0; i < run.count; i++) {
            run.torrents[i].run = &run;
        }
        if (!ok) {
            run.stopRequested = 1;
            sessionStop(run.owner);
            sessionStop(run.owner);                 /* at once: nothing has started */
        } else {
            uv_signal_start(&run.interrupt, onInterrupt, SIGINT);
#ifndef _WIN32
            uv_signal_start(&run.terminate, onInterrupt, SIGTERM);
#endif
            uv_timer_start(&run.timer, onReportTick, REPORT_TICK_MS, REPORT_TICK_MS);
        }
        uv_run(&loop, UV_RUN_DEFAULT);              /* until the session has ended */
    }

    uv_close((uv_handle_t *)&run.timer, NULL);
    uv_close((uv_handle_t *)&run.interrupt, NULL);
#ifndef _WIN32
    uv_close((uv_handle_t *)&run.terminate, NULL);
#endif
    uv_run(&loop, UV_RUN_DEFAULT);                  /* closing handles */
    sessionEnd(run.owner);
    uv_loop_close(&loop);

    for (i = 0; i < run.count; i++) {
        if (!run.torrents[i].complete || run.torrents[i].failed) {
            ok = 0;
        }
    }
    if (status != SESSION_OK || run.count == 0) {
        ok = 0;
    }
    context->allocator.free(run.torrents);
    return ok ? 0 : -1;
}

/* Our peer id: "-TI0001-" in the usual client style, then random characters. */
static void makePeerId(uint8_t peerId[PEER_ID_LENGTH])
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    uint8_t           randomBytes[12];
    uint32_t          i;

    memcpy(peerId, "-TI0001-", 8);
    if (uv_random(NULL, NULL, randomBytes, sizeof(randomBytes), 0, NULL) != 0) {
        memset(randomBytes, 0, sizeof(randomBytes));
    }
    for (i = 0; i < sizeof(randomBytes); i++) {
        peerId[8 + i] = (uint8_t)alphabet[randomBytes[i] % (sizeof(alphabet) - 1u)];
    }
}

/* -D: the session's settings from the settings file and the options, and
 * each torrent's from the options. */
static int downloadTorrents(parserContext *context, const cliSettings *cli, char **sources,
                            uint32_t count, uint32_t timeoutSeconds, const char *caFile,
                            int64_t wanted, const char *outputDirectory,
                            const netBinding *binding, uint8_t seed)
{
    sessionOptions shared;
    torrentOptions each;
    char           routers[CONFIG_MAX_VALUE];
    const char    *bootstrap[SETTINGS_MAX_BOOTSTRAP + 1u];

    memset(&shared, 0, sizeof(shared));
    makePeerId(shared.peerId);
    shared.listenPort = cli->values.listenPort;
    shared.portForwarding = cli->values.portForwarding;
    shared.dht = cli->values.dht;
    shared.blocklist = cli->blocklist;
    shared.maxDownloadRate = cli->values.maxDownloadRate;    /* for all torrents */
    shared.maxUploadRate = cli->values.maxUploadRate;
    shared.caFile = caFile;
    shared.binding = binding;
    /* dht_bootstrap (or TORRENTINFO_DHT_BOOTSTRAP): routers to use instead of
     * the well-known ones (a private DHT, or a network that blocks them). */
    if (settingsSplitBootstrap(cli->values.dhtBootstrap, routers, sizeof(routers),
                               bootstrap) > 0) {
        shared.dhtBootstrap = bootstrap;
    }

    memset(&each, 0, sizeof(each));
    each.directory = outputDirectory;
    each.handshakeTimeoutSeconds = timeoutSeconds;
    each.onlyPiece = wanted;
    each.seed = seed;
    each.dht = cli->values.dht;
    each.webSeeds = cli->values.webSeeds;
    each.recheck = cli->recheckAll;
    each.maxPeers = cli->values.maxPeers;
    each.uploadSlots = cli->values.uploadSlots;
    each.choice = &cli->choice;
    return runSession(context, cli, &shared, &each, sources, count);
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
    int32_t     status;
    char        message[STORAGE_ERROR_MESSAGE_LENGTH];
    uint8_t     done;
    storage    *store;
    uv_timer_t  timer;              /* progress while hashing */
} verifyRun;

static void onVerifyTick(uv_timer_t *timer)
{
    verifyRun *run = timer->data;
    uint32_t   checked;
    uint32_t   total;

    storageCheckProgress(run->store, &checked, &total);
    if (total > 0) {
        printf("  %" PRIu32 " of %" PRIu32 " pieces (%.0f%%)\n", checked, total,
               100.0 * checked / total);
        fflush(stdout);
    }
}

static void onVerifyOpen(void *privateArg, storage *store, int32_t status, const char *errorMessage)
{
    verifyRun *run = privateArg;

    (void)store;
    run->status = status;
    run->done = 1;
    uv_timer_stop(&run->timer);
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
                         const cliSettings *cli, const char *path, const char *directory)
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
    uint32_t          skipped = 0;
    uint32_t          wantedCount = 0;      /* pieces of the chosen files       */
    uint32_t          wantedHave = 0;       /* ...that are verified             */
    uint8_t          *fileWanted = NULL;    /* -f / -F, or NULL for every file */
    uint8_t          *pieceWanted = NULL;   /* bitfield                         */
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

    if (selectionActive(&cli->choice)) {
        fileWanted = context->allocator.realloc(NULL, torrent.fileCount);
        if (fileWanted == NULL || chooseFiles(&cli->choice, &torrent, fileWanted) != 0) {
            if (fileWanted != NULL) {
                context->allocator.free(fileWanted);
            }
            parserFree(context, &torrent);
            return -1;
        }
    }
    pieceWanted = context->allocator.realloc(NULL, messageBitfieldBytes(torrent.pieceCount) + 1u);
    if (pieceWanted == NULL) {
        if (fileWanted != NULL) {
            context->allocator.free(fileWanted);
        }
        parserFree(context, &torrent);
        return -1;
    }
    memset(pieceWanted, 0, messageBitfieldBytes(torrent.pieceCount) + 1u);
    TAILQ_FOREACH(file, &torrent.files, entries) {
        if ((fileWanted == NULL || fileWanted[fileIndex]) && file->length > 0 && !file->isPadding) {
            for (i = (uint32_t)(file->offset / torrent.pieceLength);
                 i <= (uint32_t)((file->offset + file->length - 1u) / torrent.pieceLength); i++) {
                messageBitfieldSet(pieceWanted, i);
            }
        }
        fileIndex++;
    }
    fileIndex = 0;
    for (i = 0; i < torrent.pieceCount; i++) {
        wantedCount += messageBitfieldGet(pieceWanted, i) ? 1u : 0u;
    }

    memset(&run, 0, sizeof(run));
    uv_loop_init(&loop);
    status = storageOpen(&loop, context, &torrent, directory, STORAGE_READ_ONLY, fileWanted, NULL,
                         onVerifyOpen, &run, &store);
    if (status != STORAGE_OK) {
        printf("Could not check the files: %s\n", storageStatusString(status));
        goto done;
    }
    printf("Location:   %s\n\nHashing the data on disk...\n", storageRoot(store));
    fflush(stdout);
    run.store = store;
    uv_timer_init(&loop, &run.timer);
    run.timer.data = &run;
    uv_timer_start(&run.timer, onVerifyTick, 2000u, 2000u);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_close((uv_handle_t *)&run.timer, NULL);
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
        if (state.skipped) {
            skipped++;
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
        } else if (state.inPartFile && verified > 0) {
            label = "PART FILE";                    /* edges kept for a skipped file */
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
        if (state.inPartFile && verified > 0) {
            printf("             not on disk: the bytes shown are kept in the part file,\n"
                   "             as the file was not chosen for download\n");
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
    if (skipped > 0) {
        printf("  (%" PRIu32 " file%s not chosen, not shown)\n", skipped,
               (skipped == 1u) ? "" : "s");
    }
    for (i = 0; i < torrent.pieceCount; i++) {
        if (messageBitfieldGet(pieceWanted, i) && messageBitfieldGet(have, i)) {
            wantedHave++;
        }
    }

    formatSize(bytesDone, size, sizeof(size));
    formatSize(dataLength, total, sizeof(total));
    printf("\nSummary\n");
    printf("  Status:          %s\n", (wantedHave == wantedCount) ? "COMPLETE" : "INCOMPLETE");
    printf("  Files:           %" PRIu32 " of %" PRIu32 "%s complete\n", filesComplete, shown,
           (fileWanted != NULL) ? " chosen" : "");
    printf("  Pieces:          %" PRIu32 " of %" PRIu32 " verified (%.1f%%)\n", wantedHave,
           wantedCount, percentOf(wantedHave, wantedCount));
    printf("  Bytes:           %s of %s verified (%.1f%%)\n", size, total,
           percentOf(bytesDone, dataLength));
    if (wantedHave < wantedCount) {
        uint32_t first = 0;
        uint32_t last = torrent.pieceCount - 1u;

        printf("  Missing pieces:  %" PRIu32 "", wantedCount - wantedHave);
        if (fileWanted == NULL) {
            printf(": ");
            printMissing(have, first, last);
        } else {
            printf(" (see the files above)");
        }
        printf("\n");
    }
    result = (wantedHave == wantedCount) ? 0 : 2;

done:
    storageClose(store);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    if (fileWanted != NULL) {
        context->allocator.free(fileWanted);
    }
    context->allocator.free(pieceWanted);
    parserFree(context, &torrent);
    return result;
}

/* Print the magnet link for a .torrent file. */
static int printMagnet(parserContext *context, const parserAllocator *allocator, const char *path)
{
    parserTorrent torrent;
    uint8_t      *buffer = NULL;
    uint64_t      length = 0;
    char         *uri = NULL;
    int32_t       status;

    if (readFile(allocator, path, &buffer, &length) != 0) {
        return -1;
    }
    status = parserParse(context, buffer, length, &torrent);
    allocator->free(buffer);
    if (status != PARSER_OK) {
        fprintf(stderr, "%s: parse failed: %s\n", path, parserErrorString(status));
        return -1;
    }
    status = magnetFromTorrent(context, &torrent, &uri);
    if (status == MAGNET_OK && torrent.isPrivate) {
        fprintf(stderr, "Note: this torrent is private. Its peers will not send the torrent's "
                        "details to a\nmagnet link (BEP 27), so the link cannot be used to "
                        "download it: share the\n.torrent file instead.\n");
    }
    parserFree(context, &torrent);
    if (status != MAGNET_OK) {
        fprintf(stderr, "%s: out of memory\n", path);
        return -1;
    }
    printf("%s\n", uri);
    allocator->free(uri);
    return 0;
}

/* Show what a magnet link says. */
static int showMagnet(parserContext *context, const char *uri)
{
    magnetLink link;
    uint32_t   i;

    if (magnetParse(context, uri, &link) != MAGNET_OK) {
        fprintf(stderr, "not a usable magnet link (it needs xt=urn:btih:...)\n");
        return -1;
    }
    printf("Magnet link\n  Info hash:  ");
    printHex(link.infoHash, PARSER_SHA1_LENGTH);
    printf("\n  Name:       %s\n", (link.name != NULL) ? link.name : "(not given)");
    for (i = 0; i < link.trackerCount; i++) {
        printf("  %s %s\n", (i == 0) ? "Trackers:  " : "           ", link.trackers[i]);
    }
    for (i = 0; i < link.peerCount; i++) {
        printf("  %s %s\n", (i == 0) ? "Peers:     " : "           ", link.peers[i]);
    }
    for (i = 0; i < link.webSeedCount; i++) {
        printf("  %s %s\n", (i == 0) ? "Web seeds: " : "           ", link.webSeeds[i]);
    }
    printf("\nUse -D to download it: the torrent itself is fetched from peers first.\n");
    magnetFree(context, &link);
    return 0;
}

/* -t (announce to the trackers) and -P (and handshake with the peers). */
static int announceTorrent(parserContext *context, const parserAllocator *allocator,
                           const char *path, uint32_t timeoutSeconds, const char *caFile,
                           uint32_t mode, const netBinding *binding, uint16_t listenPort)
{
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
    uint32_t         *ids = NULL;
    uint8_t          *buffer = NULL;
    uint64_t          length = 0;
    uint32_t          urlCount = 1;
    uint32_t          count = 0;
    uint32_t          succeeded = 0;
    uint32_t          i;
    uint32_t          j;
    int32_t           status;

    memset(&config, 0, sizeof(config));
    makePeerId(config.peerId);
    config.port = (listenPort != 0) ? listenPort : 6881;
    config.caFile = caFile;
    config.binding = binding;

    if (magnetIs(path)) {
        fprintf(stderr, "magnet links work with -D (or on their own, to show them)\n");
        return -1;
    }
    if (readFile(allocator, path, &buffer, &length) != 0) {
        return -1;
    }
    status = parserParse(context, buffer, length, &torrent);
    if (status != PARSER_OK) {
        fprintf(stderr, "%s: parse failed: %s\n", path, parserErrorString(status));
        allocator->free(buffer);
        return -1;
    }

    uv_loop_init(&loop);
    allocator->free(buffer);
    buffer = NULL;
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
typedef struct optionParser {
    int         index;                  /* next argv element to examine      */
    const char *value;                  /* value of the last option          */
    const char *cursor;                 /* position within grouped flags     */
} optionParser;

static int nextOption(optionParser *parser, int argc, char **argv, const char *spec)
{
    const char *found;
    int         letter;

    if (parser->cursor == NULL || *parser->cursor == '\0') {
        if (parser->index >= argc || argv[parser->index][0] != '-' ||
            argv[parser->index][1] == '\0') {
            return -1;
        }
        if (strcmp(argv[parser->index], "--") == 0) {
            parser->index++;
            return -1;
        }
        parser->cursor = argv[parser->index++] + 1;
    }

    letter = (unsigned char)*parser->cursor++;
    found = (letter != ':') ? strchr(spec, letter) : NULL;
    if (found == NULL) {
        fprintf(stderr, "unknown option -%c\n", letter);
        return '?';
    }
    if (found[1] == ':') {
        if (*parser->cursor != '\0') {          /* "-ofile" */
            parser->value = parser->cursor;
        } else if (parser->index < argc) {      /* "-o file" */
            parser->value = argv[parser->index++];
        } else {
            fprintf(stderr, "option -%c needs a value\n", letter);
            return '?';
        }
        parser->cursor = NULL;
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

/* ------------------------------------------------------------------------ */
/* Settings file and blocklist                                              */
/* ------------------------------------------------------------------------ */

static void onSettingsWarning(void *privateArg, uint32_t line, const char *message)
{
    fprintf(stderr, "%s:%" PRIu32 ": %s (ignored)\n", (const char *)privateArg, line, message);
}

/*
 * Read the settings file: $TORRENTINFO_CONFIG, or "config" in the data
 * directory, which is created, as a commented template listing every
 * setting, if it does not exist. Problems are reported, never fatal.
 */
static void loadSettings(parserContext *context, configSettings *file)
{
    char        path[1024];
    const char *environment = getenv("TORRENTINFO_CONFIG");
    uint32_t    what;
    int32_t     error = 0;
    int32_t     result;

    path[0] = '\0';
    result = settingsLoadFile(context,
                              (environment != NULL && environment[0] != '\0') ? environment : NULL,
                              file, onSettingsWarning, path, &what, path, sizeof(path), &error);
    if (result == SETTINGS_OK) {
        if (what == SETTINGS_FILE_CREATED) {
            printf("Settings:   created %s (all settings there are off until you edit it)\n",
                   path);
        }
        return;
    }
    fprintf(stderr, "%s: cannot read the settings: %s\n", path,
            (result == SETTINGS_EIO && error != 0) ? uv_strerror(error)
                                                   : settingsErrorString(result));
}

/* Group digits in threes: 1234567 -> "1,234,567". */
static const char *formatCount(uint64_t value, char *text, size_t size)
{
    char   digits[32];
    size_t length = (size_t)snprintf(digits, sizeof(digits), "%" PRIu64, value);
    size_t out = 0;
    size_t i;

    for (i = 0; i < length && out + 2u < size; i++) {
        if (i > 0 && (length - i) % 3u == 0) {
            text[out++] = ',';
        }
        text[out++] = digits[i];
    }
    text[out] = '\0';
    return text;
}

/*
 * Load the IP blocklist the settings name (or blocklist.txt in the data
 * directory, if there is one) into cli->blocklist, saying what it holds.
 * -1 if a list the settings name cannot be read (better not to run
 * unprotected).
 */
static int loadBlocklist(parserContext *context, cliSettings *cli)
{
    char           path[CONFIG_MAX_VALUE + 64];
    blocklistStats stats;
    int32_t        error = 0;
    int32_t        result;
    char           addresses[32];

    result = settingsLoadBlocklist(context, &cli->values, &cli->blocklist, &stats, path,
                                   sizeof(path), &error);
    if (result != SETTINGS_OK) {
        fprintf(stderr, "Blocklist %s: %s\n", path,
                (result == SETTINGS_EIO && error != 0) ? uv_strerror(error)
                                                       : settingsErrorString(result));
        return -1;
    }
    if (path[0] == '\0') {
        return 0;                                   /* no list */
    }
    printf("Blocklist:  %" PRIu32 " IPv4 range%s (%s addresses)", stats.ranges4,
           (stats.ranges4 == 1u) ? "" : "s", formatCount(stats.addresses4, addresses,
                                                         sizeof(addresses)));
    if (stats.ranges6 > 0) {
        printf(" and %" PRIu32 " IPv6 range%s", stats.ranges6, (stats.ranges6 == 1u) ? "" : "s");
    }
    printf(" from %s\n", path);
    if (stats.rejected > 0) {
        printf("            %" PRIu32 " line%s not understood (the first is line %" PRIu32 ")\n",
               stats.rejected, (stats.rejected == 1u) ? "" : "s", stats.firstRejected);
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <file.torrent | magnet-link>\n"
            "       %s -o <output.torrent> [options] <file>...\n"
            "       %s -t [-T seconds] [-C ca.pem] [-I interface] <file.torrent>\n"
            "       %s -P [-T seconds] [-C ca.pem] [-I interface] <file.torrent>\n"
            "       %s -D [-S] [-L port] [-N] [-X] [-W] [-R] [-E mode] [-d rate] [-u rate] [-f list]\n"
            "            [-F glob]... [-O dir] [-i piece]\n"
            "            [-T seconds] [-C ca.pem] [-I interface] <file.torrent | magnet-link>...\n"
            "       %s -V [-O dir] [-f list] [-F glob]... <file.torrent>\n"
            "       %s -m <file.torrent>\n"
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
            "  -N        with -D: do not ask the router to forward the port (UPnP and\n"
            "            NAT-PMP are tried by default)\n"
            "  -X        with -D: do not use the DHT (by default it finds peers too,\n"
            "            except for private torrents)\n"
            "  -f list   with -D or -V: only these files, by the numbers the torrent's\n"
            "            listing shows: 1,3,5-7\n"
            "  -F glob   with -D or -V: only files whose path or name matches, like\n"
            "            '*.mkv' (any case; may be repeated, and combined with -f)\n"
            "  -W        with -D: do not download from the torrent's web seeds\n"
            "  -R        with -D: check every piece already on disk, ignoring the\n"
            "            fast-resume file kept from last time\n"
            "  -E mode   with -D: protocol encryption: prefer (the default: fall back\n"
            "            to plain for peers without it), require, or off\n"
            "  -d rate   with -D: download no faster than this, in KiB/s or with a\n"
            "            unit: 500k, 1.5M (0: no limit)\n"
            "  -u rate   with -D: upload no faster than this (same units)\n"
            "  -m        print the torrent's magnet link (info hash, name, trackers,\n"
            "            web seeds)\n"
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
            program, program, program, program, program, program, program);
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
    uint8_t         makeMagnet = 0;     /* -m */
    uint8_t         seed = 0;           /* -S */
    uint8_t         downloadOnly = 0;   /* -S, -L or -N given */
    cliSettings     cli;                /* what the settings file and the options say */
    configSettings  settingsFile;       /* the settings file                          */
    configSettings  overrides;          /* the options, which win over the file       */
    selectionResult choiceError;
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
    optionParser    parser = { 1, NULL, NULL };
    int             status = -1;

#ifndef _WIN32
    /* A peer may close its connection while we are writing to it; that must
     * be an error from the write, not a signal that ends the program. */
    signal(SIGPIPE, SIG_IGN);
#endif

    memset(&options, 0, sizeof(options));
    memset(&cli, 0, sizeof(cli));
    settingsDefaults(&cli.values);
    selectionInit(&cli.choice);
    memset(&settingsFile, 0, sizeof(settingsFile));
    memset(&overrides, 0, sizeof(overrides));

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

    while ((option = nextOption(&parser, argc, argv, "o:a:w:c:n:l:ptPDVSNXWRmL:O:i:T:C:I:d:u:f:F:E:h")) != -1) {
        switch (option) {
        case 'o':
            outputPath = parser.value;
            break;
        case 'a':
            trackers[options.trackerCount++] = parser.value;
            createOnly = 1;
            break;
        case 'w':
            webSeeds[options.webSeedCount++] = parser.value;
            createOnly = 1;
            break;
        case 'c':
            options.comment = parser.value;
            createOnly = 1;
            break;
        case 'n':
            options.name = parser.value;
            createOnly = 1;
            break;
        case 'l':
            if (parsePieceLength(parser.value, &options.pieceLength) != 0) {
                fprintf(stderr, "invalid piece length '%s': use a power of two "
                        "from 16K to 64M\n", parser.value);
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
        case 'N':
            overrides.portForwarding = 0;
            overrides.set |= CONFIG_SET_PORT_FORWARDING;
            downloadOnly = 1;
            break;
        case 'X':
            overrides.dht = 0;
            overrides.set |= CONFIG_SET_DHT;
            downloadOnly = 1;
            break;
        case 'L': {
            char         *portEnd;
            unsigned long value = strtoul(parser.value, &portEnd, 10);

            if (*portEnd != '\0' || portEnd == parser.value || value > 65535u) {
                fprintf(stderr, "-L: not a port number: %s\n", parser.value);
                goto cleanup;
            }
            overrides.listenPort = (uint16_t)value;
            overrides.set |= CONFIG_SET_LISTEN_PORT;
            downloadOnly = 1;
            break;
        }
        case 'd':
        case 'u':
            if (!configParseRate(parser.value, (option == 'd') ? &overrides.maxDownloadRate
                                                               : &overrides.maxUploadRate)) {
                fprintf(stderr, "-%c: not a speed: %s (examples: 500k, 1.5M, 0 for no limit)\n",
                        option, parser.value);
                goto cleanup;
            }
            overrides.set |= (option == 'd') ? CONFIG_SET_MAX_DOWNLOAD_RATE
                                             : CONFIG_SET_MAX_UPLOAD_RATE;
            downloadOnly = 1;
            break;
        case 'f':
            result = selectionSetList(&cli.choice, parser.value, &choiceError);
            if (result == SELECTION_EBACKWARDS) {
                fprintf(stderr, "-f: %" PRIu64 "-%" PRIu64 " is backwards: write %" PRIu64
                        "-%" PRIu64 "\n", choiceError.first, choiceError.last, choiceError.last,
                        choiceError.first);
                goto cleanup;
            }
            if (result != SELECTION_OK) {
                fprintf(stderr, "-f: expected file numbers like 1,3,5-7, not \"%s\"\n",
                        parser.value);
                goto cleanup;
            }
            break;
        case 'R':
            cli.recheckAll = 1;
            downloadOnly = 1;
            break;
        case 'W':
            overrides.webSeeds = 0;
            overrides.set |= CONFIG_SET_WEB_SEEDS;
            downloadOnly = 1;
            break;
        case 'E':
            if (!configParseEncryption(parser.value, &overrides.encryption)) {
                fprintf(stderr, "-E: use prefer, require or off, not %s\n", parser.value);
                goto cleanup;
            }
            overrides.set |= CONFIG_SET_ENCRYPTION;
            downloadOnly = 1;
            break;
        case 'F':
            if (selectionAddPattern(&cli.choice, parser.value) != SELECTION_OK) {
                fprintf(stderr, "-F: at most %u patterns\n", SELECTION_MAX_PATTERNS);
                goto cleanup;
            }
            break;
        case 'V':
            verify = 1;
            break;
        case 'm':
            makeMagnet = 1;
            break;
        case 'O':
            outputDirectory = parser.value;
            directoryGiven = 1;
            break;
        case 'i':
            number = strtoul(parser.value, &end, 10);
            if (*parser.value < '0' || *parser.value > '9' || *end != '\0' || number > UINT32_MAX) {
                fprintf(stderr, "invalid piece index '%s'\n", parser.value);
                goto cleanup;
            }
            wanted = (int64_t)number;
            announceOnly = 1;
            break;
        case 'T':
            number = strtoul(parser.value, &end, 10);
            if (*parser.value < '0' || *parser.value > '9' || *end != '\0' || number == 0 || number > 3600u) {
                fprintf(stderr, "invalid timeout '%s': use 1 to 3600 seconds\n", parser.value);
                goto cleanup;
            }
            timeoutSeconds = (uint32_t)number;
            announceOnly = 1;
            break;
        case 'C':
            caFile = parser.value;
            announceOnly = 1;
            break;
        case 'I':
            interfaceSpec = parser.value;
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
        (downloadOnly && mode != MODE_DOWNLOAD) ||
        (selectionActive(&cli.choice) && !verify && mode != MODE_DOWNLOAD)) {
        usage(argv[0]);
        goto cleanup;
    }

    if (makeMagnet) {
        if (verify || announceMode || announceOnly || downloadOnly || directoryGiven ||
            outputPath != NULL || createOnly || argc - parser.index != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        status = printMagnet(&context, &allocator, argv[parser.index]);
    } else if (verify) {
        if (announceMode || announceOnly || outputPath != NULL || createOnly || argc - parser.index != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        loadSettings(&context, &settingsFile);
        settingsMerge(&cli.values, &settingsFile, &overrides);
        if (!directoryGiven && cli.values.downloadDirectory[0] != '\0') {
            outputDirectory = cli.values.downloadDirectory;
        }
        status = verifyTorrent(&context, &allocator, &cli, argv[parser.index], outputDirectory);
    } else if (announceMode) {
        /* -D takes several torrents; -f, -F and -i only make sense for one. */
        if (outputPath != NULL || createOnly || argc - parser.index < 1 ||
            (argc - parser.index > 1 &&
             (mode != MODE_DOWNLOAD || wanted >= 0 || selectionActive(&cli.choice)))) {
            usage(argv[0]);
            goto cleanup;
        }
        if ((wanted >= 0 || outputDirectory[0] != '\0' || downloadOnly) && mode != MODE_DOWNLOAD) {
            usage(argv[0]);
            goto cleanup;
        }
        /* The settings file fills in what the command line did not say. An
         * environment variable names DHT routers for tests and private DHTs. */
        loadSettings(&context, &settingsFile);
        {
            const char *routers = getenv("TORRENTINFO_DHT_BOOTSTRAP");

            if (routers != NULL && routers[0] != '\0' &&
                strlen(routers) < sizeof(overrides.dhtBootstrap)) {
                snprintf(overrides.dhtBootstrap, sizeof(overrides.dhtBootstrap), "%s", routers);
                overrides.set |= CONFIG_SET_DHT_BOOTSTRAP;
            }
        }
        settingsMerge(&cli.values, &settingsFile, &overrides);
        /* CONFIG_ENCRYPTION_* and PEER_ENCRYPTION_* have the same values. */
        peerSetEncryption(cli.values.encryption);
        if (mode == MODE_DOWNLOAD) {
            if (!directoryGiven && cli.values.downloadDirectory[0] != '\0') {
                outputDirectory = cli.values.downloadDirectory;
            }
            if (loadBlocklist(&context, &cli) != 0) {
                goto cleanup;
            }
            if (cli.values.maxDownloadRate != 0 || cli.values.maxUploadRate != 0) {
                char down[32] = "no limit";
                char up[32] = "no limit";

                if (cli.values.maxDownloadRate != 0) {
                    formatBytes(cli.values.maxDownloadRate, down, sizeof(down));
                    strcat(down, "/s");
                }
                if (cli.values.maxUploadRate != 0) {
                    formatBytes(cli.values.maxUploadRate, up, sizeof(up));
                    strcat(up, "/s");
                }
                printf("Speed:      download %s, upload %s\n", down, up);
            }
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
        if (mode == MODE_DOWNLOAD) {
            status = downloadTorrents(&context, &cli, argv + parser.index,
                                      (uint32_t)(argc - parser.index), timeoutSeconds, caFile,
                                      wanted, outputDirectory, &binding, seed);
        } else {
            status = announceTorrent(&context, &allocator, argv[parser.index], timeoutSeconds,
                                     caFile, mode, &binding, cli.values.listenPort);
        }
    } else if (outputPath != NULL) {
        if (parser.index >= argc) {
            usage(argv[0]);
            goto cleanup;
        }
        options.createdBy = "torrentinfo";
        options.creationDate = (int64_t)time(NULL);
        options.hasCreationDate = 1;
        status = createTorrent(&context, outputPath, &options, argv + parser.index, argc - parser.index);
    } else {
        if (createOnly || argc - parser.index != 1) {
            usage(argv[0]);
            goto cleanup;
        }
        status = magnetIs(argv[parser.index]) ? showMagnet(&context, argv[parser.index])
                                             : inspectTorrent(&context, &allocator, argv[parser.index]);
    }

cleanup:
    if (cli.blocklist != NULL) {
        blocklistFree(&context, cli.blocklist);
        cli.blocklist = NULL;
    }
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
