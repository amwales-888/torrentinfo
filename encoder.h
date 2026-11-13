/*
 * encoder.h - BitTorrent metainfo (.torrent) encoder and creator.
 *
 * Two jobs:
 *
 *  1. Encoding: encoderEncode() turns a parserTorrent (from parserParse()
 *     or encoderEnd()) into a bencoded buffer ready to write to disk.
 *
 *  2. Creation: encoderBegin() / encoderAddFile() / encoderEnd() build a
 *     new parserTorrent from files on disk, hashing their contents.
 *
 * All memory comes from the allocator in the parserContext (see parser.h),
 * so a torrent from encoderEnd() is released with parserFree() as usual.
 *
 * Usage:
 *     encoderOptions options = { .trackers = urls, .trackerCount = 1 };
 *     encoderState   encoder;
 *     parserTorrent  torrent;
 *     uint8_t       *buffer;
 *     uint64_t       length;
 *
 *     encoderBegin(&encoder, &context, &options);
 *     encoderAddFile(&encoder, "/path/to/file1");
 *     encoderAddFile(&encoder, "/path/to/file2");
 *     if (encoderEnd(&encoder, &torrent) == ENCODER_OK) {
 *         if (encoderEncode(&context, &torrent, &buffer, &length) == ENCODER_OK) {
 *             ... write buffer to a file ...
 *             encoderFreeBuffer(&context, buffer);
 *         }
 *         parserFree(&context, &torrent);
 *     }
 *
 * Creates v1 torrents (BEP 3), with optional BEP 12 announce-list,
 * BEP 19 web seeds and BEP 27 private flag.
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <sys/queue.h>  /* BSD queue.h */

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define ENCODER_MIN_PIECE_LENGTH        (16u * 1024u)           /* 16 KiB */
#define ENCODER_MAX_PIECE_LENGTH        (64u * 1024u * 1024u)   /* 64 MiB */
#define ENCODER_AUTO_MAX_PIECE_LENGTH   (16u * 1024u * 1024u)   /* 16 MiB */
#define ENCODER_TARGET_PIECE_COUNT      1500u   /* Automatic sizing aims for about this */

/*
 * Return codes. The encoder may also return PARSER_ERROR_* codes
 * (e.g. PARSER_ERROR_NO_MEMORY); encoderErrorString() describes both.
 */
#define ENCODER_OK                      0
#define ENCODER_ERROR_IO                (-20)   /* System call failed: see systemError */
#define ENCODER_ERROR_NOT_REGULAR_FILE  (-21)   /* Path is a directory, device, ...    */
#define ENCODER_ERROR_DUPLICATE_FILE    (-22)   /* Two files with the same name        */
#define ENCODER_ERROR_NO_FILES          (-23)   /* encoderEnd() before any AddFile     */
#define ENCODER_ERROR_FILE_CHANGED      (-24)   /* File size changed while hashing     */
#define ENCODER_ERROR_TOO_LARGE         (-25)   /* Too much data for the piece length  */
#define ENCODER_ERROR_INVALID_NAME      (-26)   /* Name is empty, ".", ".." or has '/' */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * Settings for a new torrent. Every field is optional (zero / NULL).
 * The structure is copied by encoderBegin(), but the strings and arrays it
 * points to are not: they must stay valid until encoderEnd()/encoderAbort().
 */
typedef struct encoderOptions {
    const char         *name;           /* Torrent name. Default for one file:
                                           its file name. Required for several. */
    const char *const  *trackers;       /* trackers[0] becomes 'announce'; with  */
    uint32_t            trackerCount;   /* more than one, each gets its own tier */
    const char *const  *webSeeds;       /* BEP 19 url-list                       */
    uint32_t            webSeedCount;
    const char         *comment;
    const char         *createdBy;
    int64_t             creationDate;   /* Unix time, used if hasCreationDate    */
    uint8_t             hasCreationDate;
    uint64_t            pieceLength;    /* 0 = automatic; otherwise a power of two
                                           from ENCODER_MIN_PIECE_LENGTH to
                                           ENCODER_MAX_PIECE_LENGTH              */
    uint8_t             isPrivate;      /* BEP 27                                */
} encoderOptions;

/* A file queued by encoderAddFile(). Internal, but visible so that
 * encoderState can be allocated by the caller. */
typedef struct encoderSourceFile {
    char                            *diskPath;  /* Path as given            */
    const char                      *name;      /* File name within diskPath */
    uint64_t                         length;
    TAILQ_ENTRY(encoderSourceFile)   entries;
} encoderSourceFile;
TAILQ_HEAD(encoderSourceList, encoderSourceFile);

/* Creation state, allocated by the caller. Treat as opaque apart from the
 * two error fields. Do not copy by value. */
typedef struct encoderState {
    parserContext           *context;
    encoderOptions           options;
    struct encoderSourceList files;
    uint32_t                 fileCount;
    uint64_t                 totalLength;
    int32_t                  systemError;    /* errno for ENCODER_ERROR_IO       */
    uint32_t                 errorFileIndex; /* Which file (0-based, in the order
                                                added) caused the last error     */
    uint32_t                 magic;
} encoderState;

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Encode 'torrent' as bencode. On success *buffer (allocated with the
 * context's allocator) holds *length bytes; release it with
 * encoderFreeBuffer(). Dictionary keys are written in sorted order as the
 * specification requires.
 *
 * Only the fields held in parserTorrent are written. If a parsed torrent
 * had extra keys in its info dictionary (e.g. v2 hybrid fields or "source"),
 * they are dropped and the re-encoded torrent will have a different info hash.
 */
int32_t encoderEncode(parserContext *context, const parserTorrent *torrent,
                      uint8_t **buffer, uint64_t *length);

/* Release a buffer returned by encoderEncode(). */
void encoderFreeBuffer(parserContext *context, uint8_t *buffer);

/* Start building a torrent. 'context' must already be set up by parserInit(). */
int32_t encoderBegin(encoderState *encoder, parserContext *context,
                     const encoderOptions *options);

/*
 * Queue a regular file. Its size is recorded now; its contents are read
 * and hashed in encoderEnd(). The file appears in the torrent under its
 * own name (the last part of 'path'), so two files with the same name are
 * rejected. Files appear in the order they are added.
 */
int32_t encoderAddFile(encoderState *encoder, const char *path);

/*
 * Hash the queued files and fill in the caller-allocated 'torrent',
 * including its info hash. One file gives a single-file torrent; several
 * give a multi-file torrent. The encoder state is always released,
 * whether or not this succeeds. On failure the torrent is left empty.
 *
 * infoOffset and infoLength are set to 0: they describe a position in a
 * parsed buffer, and a created torrent has none yet.
 */
int32_t encoderEnd(encoderState *encoder, parserTorrent *torrent);

/* Release the encoder state without building a torrent. Safe to call twice. */
void encoderAbort(encoderState *encoder);

/* Human-readable description of an ENCODER_* or PARSER_* code. Never NULL. */
const char *encoderErrorString(int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
