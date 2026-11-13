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
 *     encoderEnd() is asynchronous: the files are examined, read and hashed
 *     on libuv's thread pool, and a callback reports the result on the loop
 *     thread. No function here blocks on file I/O.
 *
 * All memory comes from the allocator in the parserContext (see parser.h),
 * so a torrent from encoderEnd() is released with parserFree() as usual.
 * The allocator is only ever called from the loop thread, never from the
 * thread pool, so it does not need to be thread-safe.
 *
 * Usage:
 *     encoderOptions options = { .trackers = urls, .trackerCount = 1 };
 *     encoderState   encoder;      must stay valid until the callback runs
 *     parserTorrent  torrent;      likewise
 *
 *     encoderBegin(&encoder, &context, loop, &options);
 *     encoderAddFile(&encoder, "/path/to/file1");
 *     encoderAddFile(&encoder, "/path/to/file2");
 *     encoderEnd(&encoder, &torrent, onCreated, me);
 *     uv_run(loop, UV_RUN_DEFAULT);
 *
 *     static void onCreated(void *privateArg, int32_t status) {
 *         if (status == ENCODER_OK) {
 *             encoderEncode(&context, &torrent, &buffer, &length);
 *             ... write buffer to a file, encoderFreeBuffer() ...
 *             parserFree(&context, &torrent);
 *         }
 *     }
 *
 * Creates v1 torrents (BEP 3), with optional BEP 12 announce-list,
 * BEP 19 web seeds and BEP 27 private flag.
 *
 * Building: <uv.h> needs _POSIX_C_SOURCE=200809L with -std=c99 (see tracker.h).
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include "queue.h"      /* bundled 4.4BSD queue.h */
#include <uv.h>

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
#define ENCODER_ERROR_IO                (-20)   /* File access failed: see systemError */
#define ENCODER_ERROR_NOT_REGULAR_FILE  (-21)   /* Path is a directory, device, ...    */
#define ENCODER_ERROR_DUPLICATE_FILE    (-22)   /* Two files with the same name        */
#define ENCODER_ERROR_NO_FILES          (-23)   /* encoderEnd() before any AddFile     */
#define ENCODER_ERROR_FILE_CHANGED      (-24)   /* File size changed while hashing     */
#define ENCODER_ERROR_TOO_LARGE         (-25)   /* Too much data for the piece length  */
#define ENCODER_ERROR_INVALID_NAME      (-26)   /* Name is "", ".", ".." or has / or \ */
#define ENCODER_ERROR_CANCELLED         (-27)   /* encoderAbort() during encoderEnd()  */

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

/* Reports the end of encoderEnd(), on the loop thread. */
typedef void (*encoderCallback)(void *privateArg, int32_t status);

/* A file queued by encoderAddFile(). Internal, but visible so that
 * encoderState can be allocated by the caller. */
typedef struct encoderSourceFile {
    char                            *diskPath;  /* Path as given            */
    const char                      *name;      /* File name within diskPath */
    uint64_t                         length;    /* Found by encoderEnd()     */
    TAILQ_ENTRY(encoderSourceFile)   entries;
} encoderSourceFile;
TAILQ_HEAD(encoderSourceList, encoderSourceFile);

/* Creation state, allocated by the caller. Treat as opaque apart from the
 * two error fields. Do not copy by value, and while encoderEnd() is running
 * do not move or free it until its callback has run. */
typedef struct encoderState {
    parserContext           *context;
    uv_loop_t               *loop;
    encoderOptions           options;
    struct encoderSourceList files;
    uint32_t                 fileCount;
    uint64_t                 totalLength;
    int32_t                  systemError;    /* For ENCODER_ERROR_IO: a libuv
                                                error code (UV_ENOENT, ...); see
                                                uv_strerror()                    */
    uint32_t                 errorFileIndex; /* Which file (0-based, in the order
                                                added) caused the last error     */

    /* Internal: used while encoderEnd() is running */
    parserTorrent           *torrent;
    encoderCallback          callback;
    void                    *privateArg;
    uv_work_t                work;
    uv_mutex_t               lock;           /* guards 'cancelled'               */
    uint8_t                 *pieceBuffer;
    int32_t                  workResult;
    uint8_t                  busy;
    uint8_t                  cancelled;
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

/*
 * Start building a torrent. 'context' must already be set up by
 * parserInit(); 'loop' is the libuv loop that encoderEnd() will use.
 */
int32_t encoderBegin(encoderState *encoder, parserContext *context, uv_loop_t *loop,
                     const encoderOptions *options);

/*
 * Queue a file. No I/O happens here: the path is only recorded. The file
 * appears in the torrent under its own name (the last part of 'path';
 * on Windows '\\' and "C:" also separate parts), so
 * two files with the same name are rejected. Files appear in the order
 * they are added. Whether the file exists and is a regular file is checked
 * by encoderEnd(), which reports problems with errorFileIndex.
 */
int32_t encoderAddFile(encoderState *encoder, const char *path);

/*
 * Build the torrent in the background and fill in the caller-allocated
 * 'torrent', including its info hash. One file gives a single-file torrent;
 * several give a multi-file torrent.
 *
 * Returns ENCODER_OK once the work is under way; 'callback' then runs once
 * on the loop thread with the final status. Until it runs, 'encoder' and
 * 'torrent' must stay valid. On any other return value nothing was started
 * and the callback will not run.
 *
 * Either way the encoder state is released when encoderEnd() finishes (by
 * the time the callback runs it may be reused or freed). On failure the
 * torrent is left empty; on success release it with parserFree().
 *
 * infoOffset and infoLength are set to 0: they describe a position in a
 * parsed buffer, and a created torrent has none yet.
 */
int32_t encoderEnd(encoderState *encoder, parserTorrent *torrent,
                   encoderCallback callback, void *privateArg);

/*
 * Before encoderEnd(): release the encoder state. While encoderEnd() is
 * running: stop the work as soon as possible; the callback still runs,
 * with ENCODER_ERROR_CANCELLED. Safe to call more than once.
 */
void encoderAbort(encoderState *encoder);

/* Human-readable description of an ENCODER_* or PARSER_* code. Never NULL. */
const char *encoderErrorString(int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
