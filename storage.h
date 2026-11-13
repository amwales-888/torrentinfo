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

/* storageOpen() has finished. errorMessage is NULL on success. */
typedef void (*storageOpenCallback)(void *privateArg, storage *store, int32_t status,
                                    const char *errorMessage);

/* A piece has been written (or failed to be). errorMessage is NULL on success. */
typedef void (*storageWriteCallback)(void *privateArg, storage *store, uint32_t pieceIndex,
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

/* Pieces complete on disk, as a bitfield (messageBitfieldGet), and their count. */
const uint8_t *storageHave(const storage *store, uint32_t *haveCount);

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
