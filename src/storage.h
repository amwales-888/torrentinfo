/*
 * storage.h - A torrent's files on disk.
 *
 *   storageOpen()        create the directories and files (at their final
 *                        sizes) under a download directory, and optionally
 *                        hash any data already there to find the pieces that
 *                        are complete (resuming an interrupted download)
 *   storageWritePiece()  write a verified piece across the files it spans
 *   storageHave()        which pieces are complete on disk
 *   storageClose()       release everything
 *
 * Layout: a single-file torrent is <directory>/<name>; a multi-file torrent
 * is <directory>/<name>/<path...>. BEP 47 padding files are not created:
 * their bytes are zeros and are never written. On Windows, names Windows
 * cannot create are adjusted: the characters < > : " | ? * and control
 * characters become '_', a trailing '.' or space becomes '_', and reserved
 * device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9) get a '_' prefix.
 *
 * All disk work runs on libuv's thread pool, and the callbacks run on the
 * loop thread. Worker threads never call the allocator: everything they
 * need is prepared on the loop thread first.
 *
 * Memory comes from the allocator in the parserContext.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <uv.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define STORAGE_CHECK_EXISTING      0x01u   /* storageOpen(): hash existing data */
#define STORAGE_READ_ONLY           0x02u   /* storageOpen(): only check what is
                                               on disk: create, resize and write
                                               nothing (implies CHECK_EXISTING) */
#define STORAGE_ERROR_MESSAGE_LENGTH 320u

/* Status codes */
#define STORAGE_OK                  0
#define STORAGE_EINVAL              (-3)    /* bad argument, or not open yet      */
#define STORAGE_ENOMEM              (-4)    /* allocator returned NULL            */
#define STORAGE_EIO                 (-20)   /* file access failed: see the message */
#define STORAGE_ECANCELLED          (-27)   /* closed before the work ran         */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* Opaque handle. */
typedef struct storage storage;

/* One of the torrent's files, from storageGetFile(). */
typedef struct storageFileState {
    const char     *path;           /* on disk; valid until storageClose()  */
    uint64_t        offset;         /* in the torrent's byte range          */
    uint64_t        length;         /* the size the torrent says            */
    uint64_t        sizeOnDisk;     /* when storageOpen() looked (0 if absent) */
    uint8_t         existed;        /* had data when storageOpen() looked   */
    uint8_t         notRegular;     /* the path is a directory or the like  */
    uint8_t         isPadding;      /* BEP 47 padding: never on disk        */
} storageFileState;

/* storageOpen() has finished. errorMessage is NULL on success. */
typedef void (*storageOpenCallback)(void *privateArg, storage *store, int32_t status,
                                    const char *errorMessage);

/* A piece has been written (or failed to be). errorMessage is NULL on success. */
typedef void (*storageWriteCallback)(void *privateArg, storage *store, uint32_t pieceIndex,
                                     int32_t status, const char *errorMessage);

/*
 * A block read has finished. On STORAGE_OK, 'data' holds the block and now
 * belongs to the caller (release it with the parserContext's allocator);
 * otherwise 'data' is NULL. Not called after storageClose().
 */
typedef void (*storageReadCallback)(void *privateArg, storage *store, uint32_t pieceIndex,
                                    uint32_t begin, uint32_t length, uint8_t *data,
                                    int32_t status, const char *errorMessage);

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Prepare the files for 'torrent' under 'directory' (created if needed;
 * "" means the current directory). With STORAGE_CHECK_EXISTING, data
 * already present is hashed piece by piece and storageHave() reports the
 * pieces that match. On STORAGE_OK the work is under way and 'callback'
 * will run once; *store receives the handle, which stays valid until
 * storageClose(). The torrent is not needed after this call returns.
 */
int32_t storageOpen(uv_loop_t *loop, parserContext *context, const parserTorrent *torrent,
                    const char *directory, uint32_t flags, storageOpenCallback callback,
                    void *privateArg, storage **store);

/*
 * Write piece 'pieceIndex'. 'data' must hold the whole piece and must have
 * been allocated with the context's allocator: the storage takes ownership
 * and frees it, whatever the outcome (even if this call fails). On
 * STORAGE_OK the write is queued and 'callback' will run once, unless the
 * storage is closed first. Writes may complete in any order.
 */
int32_t storageWritePiece(storage *store, uint32_t pieceIndex, uint8_t *data,
                          storageWriteCallback callback, void *privateArg);

/*
 * Read 'length' bytes at offset 'begin' of a piece that is on disk (for
 * uploading), on the thread pool. 'data' must come from the parserContext's
 * allocator and hold 'length' bytes; storage takes ownership of it (even on
 * failure) and hands it back through the callback.
 */
int32_t storageReadBlock(storage *store, uint32_t pieceIndex, uint32_t begin, uint32_t length,
                         uint8_t *data, storageReadCallback callback, void *privateArg);

/* Pieces complete on disk, as a bitfield (messageBitfieldGet), and their count. */
const uint8_t *storageHave(const storage *store, uint32_t *haveCount);

/*
 * While storageOpen() checks the data already on disk (on several threads):
 * how many pieces it has checked so far, of how many. Both are 0 until the
 * check starts, and stay at their final values afterwards.
 */
void storageCheckProgress(storage *store, uint32_t *checked, uint32_t *total);

/* The torrent's files, in torrent order (padding files included). */
uint32_t storageFileCount(const storage *store);

/* Describe file 'index' once the storage is open. */
int32_t storageGetFile(const storage *store, uint32_t index, storageFileState *state);

/* The download's top-level path: the file, or the directory of a multi-file torrent. */
const char *storageRoot(const storage *store);

/*
 * Release the storage. Work already running finishes in the background (no
 * callbacks); keep running the loop until uv_run() returns so that all
 * memory is released. Safe to call from a callback.
 */
void storageClose(storage *store);

/* Human-readable description of a STORAGE_* status. Never NULL. */
const char *storageStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
