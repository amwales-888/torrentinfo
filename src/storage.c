/*
 * storage.c - A torrent's files on disk.
 *
 * The torrent's data is one long byte range; each file covers a slice of
 * it (parserFile.offset/length). A piece is the byte range
 * [index * pieceLength, ...) and may span several files, so every read or
 * write walks the files that overlap it (found by binary search).
 *
 * Threading: storageOpen() builds everything the workers need - full paths,
 * the file table, a copy of the piece hashes, the "have" bitfield and a
 * scratch buffer - on the loop thread. The open job then runs on the thread
 * pool; it is the only thing touching the storage until its completion
 * callback, which is why storageWritePiece() refuses until the storage is
 * open. Each write job works on its own piece; pieces never overlap, so
 * concurrent writes touch different bytes. The "have" bitfield is updated
 * only on the loop thread after a write completes.
 *
 * File handles are cached (see fileHandle()).
 *
 * Skipped files: a file the caller does not want (storageOpen()'s 'wanted')
 * is not created. A piece that is partly in a wanted file and partly in a
 * skipped one must still be downloaded whole to be checked, so the skipped
 * part of it is kept in the part file, "<directory>/.<name>.parts": records
 * of an 8-byte header ("TIP1" and the piece index, big-endian) followed by
 * one piece's worth of space, of which only the bytes belonging to files
 * not on disk are used. partSlot[] maps pieces to records; it is read when
 * the storage opens and extended on the loop thread before a write is
 * queued. A skipped file that already exists on disk is used as it is. In
 * STORAGE_READ_ONLY mode every missing file is looked for in the part file.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "message.h"    /* bitfield helpers */
#include "platform.h"
#include "storage.h"

typedef struct storageFile {
    char       *path;
    uint32_t    parentLength;   /* bytes of 'path' before its last separator */
    uint64_t    offset;         /* in the torrent's byte range               */
    uint64_t    length;
    uint8_t     padding;        /* BEP 47: not on disk, all zeros            */
    uint8_t     existed;        /* had data before storageOpen()             */
    uint8_t     notRegular;     /* the path is a directory or something else */
    uint64_t    sizeOnDisk;     /* when opened (before any resizing)         */
    platformFile handle;        /* kept open once used (see fileHandle())    */
    uint8_t     handleOpen;
    uint8_t     skipped;        /* not wanted by the caller                  */
    uint8_t     inPart;         /* not on disk: its bytes are in the part file */
} storageFile;

#define STORAGE_PART_HEADER     8u      /* "TIP1" + piece index             */

#define STORAGE_CHECK_JOBS      4u      /* parallel resume-check jobs          */
#define STORAGE_MAX_OPEN_FILES  64u     /* handles kept open; more are opened
                                           and closed per access              */

struct storage;

/* One of the parallel jobs hashing existing data. */
typedef struct storageCheck {
    uv_work_t               work;
    struct storage         *store;
    uint32_t                first;          /* pieces [first, last)           */
    uint32_t                last;
    uint8_t                *scratch;        /* one piece                      */
} storageCheck;

struct storage {
    uv_loop_t              *loop;
    parserContext          *context;
    char                   *root;
    storageFile            *files;
    uint32_t                fileCount;
    uint64_t                pieceLength;
    uint64_t                totalLength;
    uint32_t                pieceCount;
    uint8_t                *hashes;         /* copy of the torrent's piece hashes */
    uint8_t                *have;           /* bitfield                           */
    uint32_t                haveCount;
    uint8_t                 checkExisting;
    uint8_t                 readOnly;       /* STORAGE_READ_ONLY: check only      */
    storageFile             part;           /* the part file (path, handle only)  */
    uint32_t               *partSlot;       /* per piece: record + 1, or 0        */
    uint32_t                partSlots;      /* records in the part file           */
    uint8_t                 anyInPart;      /* some file is kept in the part file */

    /* The open job, then the check jobs */
    uv_work_t               openWork;
    storageOpenCallback     openCallback;
    void                   *openArg;
    int32_t                 openResult;
    char                    openMessage[STORAGE_ERROR_MESSAGE_LENGTH];
    uint8_t                 anyExisted;     /* some file had data             */
    storageCheck            checks[STORAGE_CHECK_JOBS];
    uint32_t                checksRunning;
    uint8_t                *verified;       /* one byte per piece (no sharing) */

    /* Shared with the thread pool, under 'lock' */
    uv_mutex_t              lock;
    uint8_t                 lockReady;
    uint32_t                openFiles;      /* cached handles                  */
    uint32_t                checkTotal;     /* pieces to check                 */
    uint32_t                checked;        /* of those, done                  */
    uint8_t                 stopChecking;

    uint32_t                jobs;           /* thread-pool jobs not yet completed */
    uint8_t                 opened;
    uint8_t                 closed;
};

typedef struct storageRead {
    uv_work_t               work;
    storage                *store;
    uint32_t                index;
    uint32_t                begin;
    uint32_t                length;
    uint8_t                *data;
    storageReadCallback     callback;
    void                   *privateArg;
    int32_t                 result;
    char                    message[STORAGE_ERROR_MESSAGE_LENGTH];
} storageRead;

typedef struct storageWrite {
    uv_work_t               work;
    storage                *store;
    uint32_t                index;
    uint8_t                *data;
    uint32_t                length;
    storageWriteCallback    callback;
    void                   *privateArg;
    int32_t                 result;
    char                    message[STORAGE_ERROR_MESSAGE_LENGTH];
} storageWrite;

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static void *allocate(parserContext *context, uint64_t size)
{
    void *memory;

    if (size == 0 || size > (uint64_t)(SIZE_MAX / 2u)) {
        return NULL;
    }
    memory = context->allocator.realloc(NULL, (size_t)size);
    if (memory != NULL) {
        memset(memory, 0, (size_t)size);
    }
    return memory;
}

static void release(parserContext *context, void *memory)
{
    if (memory != NULL) {
        context->allocator.free(memory);
    }
}

static void freeStorage(storage *store)
{
    parserContext *context = store->context;
    uint32_t       i;

    for (i = 0; store->files != NULL && i < store->fileCount; i++) {
        if (store->files[i].handleOpen) {
            platformFileClose(&store->files[i].handle);
        }
        release(context, store->files[i].path);
    }
    for (i = 0; i < STORAGE_CHECK_JOBS; i++) {
        release(context, store->checks[i].scratch);
    }
    release(context, store->verified);
    if (store->part.handleOpen) {
        platformFileClose(&store->part.handle);
    }
    release(context, store->part.path);
    release(context, store->partSlot);
    if (store->lockReady) {
        uv_mutex_destroy(&store->lock);
    }
    release(context, store->files);
    release(context, store->root);
    release(context, store->hashes);
    release(context, store->have);
    release(context, store);
}

static void maybeFree(storage *store)
{
    if (store->closed && store->jobs == 0) {
        freeStorage(store);
    }
}

static uint32_t pieceSize(const storage *store, uint32_t index)
{
    uint64_t start = (uint64_t)index * store->pieceLength;
    uint64_t size = store->totalLength - start;

    return (uint32_t)((size > store->pieceLength) ? store->pieceLength : size);
}

/*
 * Append one path component, adjusted on Windows so that it can be
 * created there (see storage.h). 'out' has room for strlen(name) + 2.
 * Returns the number of bytes written.
 */
static size_t appendComponent(char *out, const char *name)
{
    size_t length = strlen(name);
#ifdef _WIN32
    static const char *const reserved[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    size_t   stem = strcspn(name, ".");             /* "CON.txt" is reserved too */
    size_t   used = 0;
    size_t   i;
    size_t   j;

    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (stem == strlen(reserved[i])) {
            for (j = 0; j < stem; j++) {
                char c = (name[j] >= 'a' && name[j] <= 'z') ? (char)(name[j] - 'a' + 'A')
                                                            : name[j];
                if (c != reserved[i][j]) {
                    break;
                }
            }
            if (j == stem) {
                out[used++] = '_';
                break;
            }
        }
    }
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];

        out[used++] = (c < 0x20u || strchr("<>:\"|?*", c) != NULL) ? '_' : (char)c;
    }
    if (used > 0 && (out[used - 1] == '.' || out[used - 1] == ' ')) {
        out[used - 1] = '_';                        /* Windows drops these silently */
    }
    return used;
#else
    memcpy(out, name, length);
    return length;
#endif
}

/* First file whose byte range ends after 'position' (skips empty files). */
static uint32_t findFile(const storage *store, uint64_t position)
{
    uint32_t low = 0;
    uint32_t high = store->fileCount;

    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;

        if (store->files[middle].offset + store->files[middle].length <= position) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return low;
}

/*
 * Worker thread: read or write piece 'index' through the files it spans.
 * For reads, *missing is set if a file is absent or too short. Returns
 * STORAGE_OK or STORAGE_EIO with 'message' describing the failure.
 */
/*
 * A handle for the file: the cached one, or a new one that is cached (up to
 * STORAGE_MAX_OPEN_FILES), or else a temporary one the caller must close.
 * Positional reads and writes make one handle safe for every thread.
 */
static int32_t fileHandle(storage *store, storageFile *entry, uint8_t write, platformFile *file,
                          uint8_t *temporary)
{
    int32_t result;

    *temporary = 0;
    uv_mutex_lock(&store->lock);
    if (entry->handleOpen) {
        *file = entry->handle;
        uv_mutex_unlock(&store->lock);
        return 0;
    }
    if (store->openFiles < STORAGE_MAX_OPEN_FILES) {
        result = store->readOnly ? platformFileOpenRead(entry->path, file)
                                 : platformFileOpenReadWrite(entry->path, file);
        if (result == 0) {
            entry->handle = *file;
            entry->handleOpen = 1;
            store->openFiles++;
        }
        uv_mutex_unlock(&store->lock);
        return result;
    }
    uv_mutex_unlock(&store->lock);
    *temporary = 1;
    return write ? platformFileOpenReadWrite(entry->path, file)
                 : platformFileOpenRead(entry->path, file);
}

/* Where piece 'index' keeps its skipped bytes in the part file. */
static uint64_t partRecord(const storage *store, uint32_t index)
{
    return (uint64_t)(store->partSlot[index] - 1u) * (STORAGE_PART_HEADER + store->pieceLength);
}

/* Does piece 'index' have bytes in files kept in the part file? */
static int pieceInPart(const storage *store, uint32_t index)
{
    uint64_t start = (uint64_t)index * store->pieceLength;
    uint64_t end = start + pieceSize(store, index);
    uint32_t i;

    if (!store->anyInPart) {
        return 0;
    }
    for (i = findFile(store, start); i < store->fileCount && store->files[i].offset < end; i++) {
        if (store->files[i].inPart && store->files[i].length > 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Worker thread: read or write the bytes [position, position + remaining)
 * of piece 'index' through the files they span. For reads, *missing is set
 * if a file is absent or too short. Returns STORAGE_OK or STORAGE_EIO with
 * 'message' describing the failure.
 */
static int32_t rangeIo(storage *store, uint32_t index, uint64_t position, uint32_t remaining,
                       uint8_t *buffer, uint8_t write, char *message, size_t messageSize,
                       uint8_t *missing)
{
    uint8_t      temporary;
    uint32_t     fileIndex = findFile(store, position);
    uint32_t     done;
    uint32_t     count;
    uint32_t     chunk;
    uint64_t     within;
    platformFile file;
    int32_t      result;

    while (remaining > 0 && fileIndex < store->fileCount) {
        storageFile *entry = &store->files[fileIndex++];

        within = position - entry->offset;
        if (within >= entry->length) {
            continue;                               /* an empty file */
        }
        chunk = (entry->length - within < remaining) ? (uint32_t)(entry->length - within)
                                                     : remaining;
        if (entry->inPart) {
            if (store->partSlot[index] == 0) {
                if (!write) {
                    *missing = 1;
                    return STORAGE_OK;
                }
                snprintf(message, messageSize, "%s: no room kept for piece %u",
                         store->part.path, (unsigned)index);
                return STORAGE_EIO;
            }
            /* The same bytes, in the piece's record of the part file. */
            within = partRecord(store, index) + STORAGE_PART_HEADER +
                     (position - (uint64_t)index * store->pieceLength);
            entry = &store->part;
        }
        if (entry->padding) {
            if (!write) {
                memset(buffer, 0, chunk);
            }
        } else {
            result = fileHandle(store, entry, write, &file, &temporary);
            if (result != 0) {
                if (!write && result == UV_ENOENT) {
                    *missing = 1;
                    return STORAGE_OK;
                }
                snprintf(message, messageSize, "%s: %s", entry->path, uv_strerror(result));
                return STORAGE_EIO;
            }
            for (done = 0; done < chunk; done += count) {
                result = write ? platformFileWriteAt(&file, within + done, buffer + done,
                                                     chunk - done, &count)
                               : platformFileReadAt(&file, within + done, buffer + done,
                                                    chunk - done, &count);
                if (result == 0 && count == 0) {
                    if (!write) {
                        *missing = 1;               /* file shorter than it should be */
                        if (temporary) {
                            platformFileClose(&file);
                        }
                        return STORAGE_OK;
                    }
                    result = UV_EIO;
                }
                if (result != 0) {
                    snprintf(message, messageSize, "%s: %s", entry->path, uv_strerror(result));
                    if (temporary) {
                        platformFileClose(&file);
                    }
                    return STORAGE_EIO;
                }
            }
            result = temporary ? platformFileClose(&file) : 0;
            if (result != 0) {
                snprintf(message, messageSize, "%s: %s", entry->path, uv_strerror(result));
                return STORAGE_EIO;
            }
        }
        position += chunk;
        buffer += chunk;
        remaining -= chunk;
    }
    return STORAGE_OK;
}

static int32_t pieceIo(storage *store, uint32_t index, uint8_t *buffer, uint8_t write,
                       char *message, size_t messageSize, uint8_t *missing)
{
    if (write && pieceInPart(store, index)) {
        uint8_t      header[STORAGE_PART_HEADER] = { 'T', 'I', 'P', '1', 0, 0, 0, 0 };
        uint8_t      temporary;
        uint32_t     count = 0;
        platformFile file;
        int32_t      result;

        header[4] = (uint8_t)(index >> 24);
        header[5] = (uint8_t)(index >> 16);
        header[6] = (uint8_t)(index >> 8);
        header[7] = (uint8_t)index;
        result = fileHandle(store, &store->part, 1, &file, &temporary);
        if (result == 0) {
            result = platformFileWriteAt(&file, partRecord(store, index), header,
                                         STORAGE_PART_HEADER, &count);
            if (result == 0 && count != STORAGE_PART_HEADER) {
                result = UV_EIO;
            }
            if (temporary) {
                platformFileClose(&file);
            }
        }
        if (result != 0) {
            snprintf(message, messageSize, "%s: %s", store->part.path, uv_strerror(result));
            return STORAGE_EIO;
        }
    }
    return rangeIo(store, index, (uint64_t)index * store->pieceLength, pieceSize(store, index),
                   buffer, write, message, messageSize, missing);
}

/* Worker thread (open): find the records already in the part file. */
static void readPartIndex(storage *store)
{
    platformFile file;
    uint64_t     size;
    uint8_t      isRegular;
    uint8_t      header[STORAGE_PART_HEADER];
    uint64_t     record = STORAGE_PART_HEADER + store->pieceLength;
    uint32_t     count;
    uint32_t     slot;
    uint32_t     index;

    if (platformFileStat(store->part.path, &size, &isRegular) != 0 || !isRegular ||
        platformFileOpenRead(store->part.path, &file) != 0) {
        return;
    }
    for (slot = 0; (uint64_t)slot * record + STORAGE_PART_HEADER <= size && slot < UINT32_MAX - 1u;
         slot++) {
        if (platformFileReadAt(&file, (uint64_t)slot * record, header, STORAGE_PART_HEADER,
                               &count) != 0 || count != STORAGE_PART_HEADER) {
            break;
        }
        index = ((uint32_t)header[4] << 24) | ((uint32_t)header[5] << 16) |
                ((uint32_t)header[6] << 8) | header[7];
        if (memcmp(header, "TIP1", 4) == 0 && index < store->pieceCount &&
            store->partSlot[index] == 0) {
            store->partSlot[index] = slot + 1u;
        }
    }
    /* A record cut short (by a crash) is reused space: count it anyway, so a
     * new record never overlaps it. */
    store->partSlots = (uint32_t)((size + record - 1u) / record);
    platformFileClose(&file);
}

static void removeFile(const char *path)
{
    uv_fs_t request;

    uv_fs_unlink(NULL, &request, path, NULL);
    uv_fs_req_cleanup(&request);
}

/* Worker thread: does piece 'index' lie entirely in data that existed before? */
static uint8_t pieceInExistingData(const storage *store, uint32_t index)
{
    uint64_t start = (uint64_t)index * store->pieceLength;
    uint64_t end = start + pieceSize(store, index);
    uint32_t i;

    for (i = findFile(store, start); i < store->fileCount && store->files[i].offset < end; i++) {
        const storageFile *entry = &store->files[i];

        if (entry->length > 0 && !entry->padding &&
            (entry->inPart ? store->partSlot[index] == 0 : !entry->existed)) {
            return 0;
        }
    }
    return 1;
}

/* ======================================================================== */
/* Opening                                                                  */
/* ======================================================================== */

/*
 * Worker thread: create every parent directory of 'path' (whose parent part
 * is 'parentLength' bytes), separator by separator. Only the final
 * directory's result matters: earlier ones may legitimately fail (a drive
 * root, a directory we may not create but that exists).
 */
static int32_t makeParents(char *path, uint32_t parentLength)
{
    uint32_t i;
    int32_t  result;
    char     saved;

    if (parentLength == 0) {
        return 0;
    }
    for (i = 1; i < parentLength; i++) {
        if (path[i] == '/' || path[i] == PLATFORM_SEPARATOR) {
            saved = path[i];
            path[i] = '\0';
            platformMakeDirectory(path);
            path[i] = saved;
        }
    }
    saved = path[parentLength];
    path[parentLength] = '\0';
    result = platformMakeDirectory(path);
    path[parentLength] = saved;
    return result;
}

/* Worker thread: create the files at their final sizes, then the resume check. */
static void openWork(uv_work_t *work)
{
    storage     *store = work->data;
    platformFile file;
    uint64_t     size;
    uint8_t      isRegular;
    uint8_t      anyExisted = 0;
    uint32_t     i;
    int32_t      result;

    store->openResult = STORAGE_OK;
    for (i = 0; i < store->fileCount; i++) {
        storageFile *entry = &store->files[i];

        if (entry->padding) {
            continue;
        }
        if (store->readOnly) {
            /* Look, do not touch: a missing or odd file just has no pieces
             * (but its edges may be in the part file). */
            result = platformFileStat(entry->path, &size, &isRegular);
            entry->notRegular = (uint8_t)(result == 0 && !isRegular);
            entry->sizeOnDisk = (result == 0 && isRegular) ? size : 0;
            entry->existed = (uint8_t)(result == 0 && isRegular && size > 0);
            entry->inPart = (uint8_t)(result != 0 && entry->length > 0);
            store->anyInPart = (uint8_t)(store->anyInPart | entry->inPart);
            anyExisted = (uint8_t)(anyExisted | entry->existed);
            continue;
        }
        if (entry->skipped) {
            /* Not wanted: leave it alone if it is not there already. */
            result = platformFileStat(entry->path, &size, &isRegular);
            if (result != 0 || !isRegular || size == 0) {
                entry->notRegular = (uint8_t)(result == 0 && !isRegular);
                entry->inPart = (uint8_t)(entry->length > 0);
                store->anyInPart = (uint8_t)(store->anyInPart | entry->inPart);
                continue;
            }
        }
        result = makeParents(entry->path, entry->parentLength);
        if (result != 0) {
            snprintf(store->openMessage, sizeof(store->openMessage),
                     "cannot create the directory for %s: %s", entry->path, uv_strerror(result));
            store->openResult = STORAGE_EIO;
            return;
        }

        result = platformFileStat(entry->path, &size, &isRegular);
        if (result == 0 && !isRegular) {
            snprintf(store->openMessage, sizeof(store->openMessage),
                     "%s exists and is not a regular file", entry->path);
            store->openResult = STORAGE_EIO;
            return;
        }
        entry->existed = (uint8_t)(result == 0 && size > 0);
        entry->sizeOnDisk = (result == 0) ? size : 0;
        anyExisted = (uint8_t)(anyExisted | entry->existed);

        result = platformFileOpenReadWrite(entry->path, &file);
        if (result == 0 && (!entry->existed || size != entry->length)) {
            result = platformFileSetSize(&file, entry->length);
            if (result != 0) {
                platformFileClose(&file);
            }
        }
        if (result == 0) {
            result = platformFileClose(&file);
        }
        if (result != 0) {
            snprintf(store->openMessage, sizeof(store->openMessage), "%s: %s",
                     entry->path, uv_strerror(result));
            store->openResult = STORAGE_EIO;
            return;
        }
    }

    if (store->anyInPart) {
        readPartIndex(store);
        anyExisted = (uint8_t)(anyExisted | (store->partSlots > 0));
    } else if (!store->readOnly) {
        removeFile(store->part.path);               /* every file is on disk now */
    }

    /* The pieces in data that was already there are checked next, in
     * parallel (startChecks()). */
    store->anyExisted = anyExisted;
}

/* ------------------------------------------------------------------------ */
/* Checking existing data, in parallel                                      */
/* ------------------------------------------------------------------------ */

static void checkWork(uv_work_t *work)
{
    storageCheck *job = work->data;
    storage      *store = job->store;
    uint8_t       digest[PARSER_SHA1_LENGTH];
    char          message[STORAGE_ERROR_MESSAGE_LENGTH];
    uint8_t       missing;
    uint8_t       stop;
    uint32_t      i;

    for (i = job->first; i < job->last; i++) {
        if (!pieceInExistingData(store, i)) {
            continue;
        }
        uv_mutex_lock(&store->lock);
        stop = store->stopChecking;
        uv_mutex_unlock(&store->lock);
        if (stop) {
            return;
        }
        missing = 0;
        message[0] = '\0';
        if (pieceIo(store, i, job->scratch, 0, message, sizeof(message), &missing) !=
            STORAGE_OK) {
            uv_mutex_lock(&store->lock);
            if (store->openResult == STORAGE_OK) {
                store->openResult = STORAGE_EIO;
                snprintf(store->openMessage, sizeof(store->openMessage), "%s", message);
            }
            store->stopChecking = 1;
            uv_mutex_unlock(&store->lock);
            return;
        }
        if (!missing) {
            parserSha1(job->scratch, pieceSize(store, i), digest);
            store->verified[i] = (uint8_t)(memcmp(digest,
                                                  store->hashes + (uint64_t)i * PARSER_SHA1_LENGTH,
                                                  PARSER_SHA1_LENGTH) == 0);
        }
        uv_mutex_lock(&store->lock);
        store->checked++;
        uv_mutex_unlock(&store->lock);
    }
}

static void finishOpen(storage *store);

static void checkDone(uv_work_t *work, int status)
{
    storageCheck *job = work->data;
    storage      *store = job->store;
    uint32_t      i;

    (void)status;
    release(store->context, job->scratch);
    job->scratch = NULL;
    if (--store->checksRunning == 0) {
        if (store->openResult == STORAGE_OK) {
            for (i = 0; i < store->pieceCount; i++) {
                if (store->verified[i]) {
                    messageBitfieldSet(store->have, i);
                    store->haveCount++;
                }
            }
        }
        release(store->context, store->verified);
        store->verified = NULL;
        finishOpen(store);
    }
    store->jobs--;
    maybeFree(store);
}

/* Hash existing pieces on several threads. 0 if they are under way. */
static int startChecks(storage *store)
{
    uint32_t total = 0;
    uint32_t jobs;
    uint32_t i;
    uint32_t start = 0;

    for (i = 0; i < store->pieceCount; i++) {
        total += pieceInExistingData(store, i) ? 1u : 0u;
    }
    if (total == 0) {
        return -1;
    }
    store->verified = allocate(store->context, store->pieceCount);
    if (store->verified == NULL) {
        store->openResult = STORAGE_ENOMEM;
        return -1;
    }
    store->checkTotal = total;
    jobs = (total < STORAGE_CHECK_JOBS) ? total : STORAGE_CHECK_JOBS;
    for (i = 0; i < jobs; i++) {
        storageCheck *job = &store->checks[i];
        uint32_t      end = (i + 1u == jobs) ? store->pieceCount
                          : (uint32_t)((uint64_t)store->pieceCount * (i + 1u) / jobs);

        job->store = store;
        job->first = start;
        job->last = end;
        start = end;
        job->scratch = allocate(store->context, store->pieceLength);
        job->work.data = job;
        if (job->scratch == NULL ||
            uv_queue_work(store->loop, &job->work, checkWork, checkDone) != 0) {
            /* Run what fits in the jobs already queued: none left to take
             * this range, so give up on the whole check. */
            release(store->context, job->scratch);
            job->scratch = NULL;
            store->openResult = STORAGE_ENOMEM;
            uv_mutex_lock(&store->lock);
            store->stopChecking = 1;
            uv_mutex_unlock(&store->lock);
            break;
        }
        store->checksRunning++;
        store->jobs++;
    }
    if (store->checksRunning == 0) {
        release(store->context, store->verified);
        store->verified = NULL;
        return -1;
    }
    return 0;
}

/*
 * In openDone() and writeDone() the job still counts in 'jobs' while the
 * callback runs, so a storageClose() from inside the callback cannot free
 * the storage under our feet; the count drops afterwards.
 */
/* Report the outcome of storageOpen(). */
static void finishOpen(storage *store)
{
    if (!store->closed) {
        store->opened = (uint8_t)(store->openResult == STORAGE_OK);
        store->openCallback(store->openArg, store,
                            store->openResult,
                            (store->openResult == STORAGE_OK) ? NULL
                            : (store->openMessage[0] != '\0') ? store->openMessage
                                                              : storageStatusString(store->openResult));
    }
}

static void openDone(uv_work_t *work, int status)
{
    storage *store = work->data;

    if (status == UV_ECANCELED) {
        store->openResult = STORAGE_ECANCELLED;
    }
    if (store->openResult != STORAGE_OK || store->closed || !store->checkExisting ||
        !store->anyExisted || startChecks(store) != 0) {
        finishOpen(store);
    }
    store->jobs--;
    maybeFree(store);
}

/* Build every file's full path, adjusted for the platform. */
static int32_t buildFiles(storage *store, const parserTorrent *torrent, const char *directory,
                          const uint8_t *wanted)
{
    const parserFile          *file;
    const parserPathComponent *component;
    size_t                     directoryLength = strlen(directory);
    size_t                     nameLength = strlen(torrent->name);
    size_t                     capacity;
    size_t                     used;
    char                      *path;
    uint32_t                   i = 0;

    /* Drop trailing separators, but keep a lone "/" as the root. */
    while (directoryLength > 1 && (directory[directoryLength - 1] == '/' ||
                                   directory[directoryLength - 1] == PLATFORM_SEPARATOR)) {
        directoryLength--;
    }
    if (directoryLength == 0) {
        directory = ".";
        directoryLength = 1;
    }

    store->root = allocate(store->context, directoryLength + nameLength + 4u);
    if (store->root == NULL) {
        return STORAGE_ENOMEM;
    }
    memcpy(store->root, directory, directoryLength);
    used = directoryLength;
    if (store->root[used - 1] != '/' && store->root[used - 1] != PLATFORM_SEPARATOR) {
        store->root[used++] = PLATFORM_SEPARATOR;
    }
    appendComponent(store->root + used, torrent->name);

    /* The part file: <directory>/.<name>.parts */
    store->part.path = allocate(store->context, directoryLength + nameLength + 16u);
    if (store->part.path == NULL) {
        return STORAGE_ENOMEM;
    }
    memcpy(store->part.path, store->root, used);
    store->part.path[used] = '.';
    {
        size_t length = appendComponent(store->part.path + used + 1u, torrent->name);

        memcpy(store->part.path + used + 1u + length, ".parts", 7u);
    }

    TAILQ_FOREACH(file, &torrent->files, entries) {
        storageFile *entry = &store->files[i++];

        capacity = directoryLength + nameLength + 4u;
        TAILQ_FOREACH(component, &file->path, entries) {
            capacity += strlen(component->name) + 2u;
        }
        path = allocate(store->context, capacity);
        if (path == NULL) {
            return STORAGE_ENOMEM;
        }
        entry->path = path;

        /* <directory>/<name> for a single file; <directory>/<name>/<path...> otherwise
         * (a single-file torrent's one path component is its name). */
        memcpy(path, directory, directoryLength);
        used = directoryLength;
        if (path[used - 1] != '/' && path[used - 1] != PLATFORM_SEPARATOR) {
            path[used++] = PLATFORM_SEPARATOR;
        }
        if (torrent->isMultiFile) {
            used += appendComponent(path + used, torrent->name);
            path[used++] = PLATFORM_SEPARATOR;
        }
        TAILQ_FOREACH(component, &file->path, entries) {
            entry->parentLength = (uint32_t)(used - 1u);
            used += appendComponent(path + used, component->name);
            path[used++] = PLATFORM_SEPARATOR;
        }
        path[used - 1] = '\0';                      /* replace the final separator */

        entry->offset = file->offset;
        entry->length = file->length;
        entry->padding = file->isPadding;
        entry->skipped = (uint8_t)(wanted != NULL && !wanted[i - 1u] && !file->isPadding);
    }
    return STORAGE_OK;
}

/* ======================================================================== */
/* Writing                                                                  */
/* ======================================================================== */

static void writeWork(uv_work_t *work)
{
    storageWrite *job = work->data;
    uint8_t       missing = 0;

    job->result = pieceIo(job->store, job->index, job->data, 1, job->message,
                          sizeof(job->message), &missing);
}

static void writeDone(uv_work_t *work, int status)
{
    storageWrite  *job = work->data;
    storage       *store = job->store;
    parserContext *context = store->context;

    release(context, job->data);
    if (status == UV_ECANCELED) {
        job->result = STORAGE_ECANCELLED;
    }
    if (job->result == STORAGE_OK && !messageBitfieldGet(store->have, job->index)) {
        messageBitfieldSet(store->have, job->index);
        store->haveCount++;
    }
    if (!store->closed && job->callback != NULL) {
        job->callback(job->privateArg, store, job->index, job->result,
                      (job->result == STORAGE_OK) ? NULL
                      : (job->message[0] != '\0') ? job->message
                                                  : storageStatusString(job->result));
    }
    release(context, job);
    store->jobs--;
    maybeFree(store);
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t storageOpen(uv_loop_t *loop, parserContext *context, const parserTorrent *torrent,
                    const char *directory, uint32_t flags, const uint8_t *wanted,
                    storageOpenCallback callback, void *privateArg, storage **handle)
{
    storage *store;
    uint64_t hashBytes;
    int32_t  result;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (loop == NULL || context == NULL || torrent == NULL || callback == NULL ||
        handle == NULL || context->magic != PARSER_CONTEXT_MAGIC || torrent->name == NULL ||
        torrent->fileCount == 0 || torrent->pieceLength == 0) {
        return STORAGE_EINVAL;
    }
    if (directory == NULL) {
        directory = "";
    }

    store = allocate(context, sizeof(*store));
    if (store == NULL) {
        return STORAGE_ENOMEM;
    }
    store->loop = loop;
    store->context = context;
    store->pieceLength = torrent->pieceLength;
    store->totalLength = torrent->totalLength;
    store->pieceCount = torrent->pieceCount;
    store->readOnly = (uint8_t)((flags & STORAGE_READ_ONLY) != 0);
    store->checkExisting = (uint8_t)((flags & (STORAGE_CHECK_EXISTING | STORAGE_READ_ONLY)) != 0);
    store->openCallback = callback;
    store->openArg = privateArg;
    store->fileCount = torrent->fileCount;

    hashBytes = (uint64_t)torrent->pieceCount * PARSER_SHA1_LENGTH;
    store->files = allocate(context, (uint64_t)store->fileCount * sizeof(*store->files));
    store->hashes = (hashBytes > 0) ? allocate(context, hashBytes) : NULL;
    store->have = (torrent->pieceCount > 0)
                ? allocate(context, messageBitfieldBytes(torrent->pieceCount)) : NULL;
    store->partSlot = (torrent->pieceCount > 0)
                    ? allocate(context, (uint64_t)torrent->pieceCount * sizeof(uint32_t)) : NULL;
    result = STORAGE_OK;
    if (store->files == NULL || (hashBytes > 0 && (store->hashes == NULL || store->have == NULL ||
                                                   store->partSlot == NULL))) {
        result = STORAGE_ENOMEM;
    }
    if (result == STORAGE_OK && uv_mutex_init(&store->lock) == 0) {
        store->lockReady = 1;
    } else if (result == STORAGE_OK) {
        result = STORAGE_EIO;
    }
    if (result == STORAGE_OK) {
        if (hashBytes > 0) {
            memcpy(store->hashes, torrent->pieces, (size_t)hashBytes);
        }
        result = buildFiles(store, torrent, directory, wanted);
    }
    if (result == STORAGE_OK) {
        store->openWork.data = store;
        if (uv_queue_work(loop, &store->openWork, openWork, openDone) != 0) {
            result = STORAGE_EIO;
        }
    }
    if (result != STORAGE_OK) {
        freeStorage(store);
        return result;
    }
    store->jobs = 1;
    *handle = store;
    return STORAGE_OK;
}

int32_t storageWritePiece(storage *store, uint32_t pieceIndex, uint8_t *data,
                          storageWriteCallback callback, void *privateArg)
{
    storageWrite *job;

    if (store == NULL || data == NULL) {
        return STORAGE_EINVAL;                      /* nothing we can free safely */
    }
    if (!store->opened || store->closed || store->readOnly || pieceIndex >= store->pieceCount) {
        release(store->context, data);
        return STORAGE_EINVAL;
    }
    job = allocate(store->context, sizeof(*job));
    if (job == NULL) {
        release(store->context, data);
        return STORAGE_ENOMEM;
    }
    if (store->partSlot[pieceIndex] == 0 && pieceInPart(store, pieceIndex)) {
        if (store->partSlots == UINT32_MAX - 1u) {
            release(store->context, data);
            release(store->context, job);
            return STORAGE_EIO;
        }
        store->partSlot[pieceIndex] = ++store->partSlots;   /* before any worker looks */
    }
    job->store = store;
    job->index = pieceIndex;
    job->data = data;
    job->length = pieceSize(store, pieceIndex);
    job->callback = callback;
    job->privateArg = privateArg;
    job->work.data = job;
    if (uv_queue_work(store->loop, &job->work, writeWork, writeDone) != 0) {
        release(store->context, data);
        release(store->context, job);
        return STORAGE_EIO;
    }
    store->jobs++;
    return STORAGE_OK;
}

const uint8_t *storageHave(const storage *store, uint32_t *haveCount)
{
    if (haveCount != NULL) {
        *haveCount = (store != NULL) ? store->haveCount : 0;
    }
    return (store != NULL) ? store->have : NULL;
}

void storageCheckProgress(storage *store, uint32_t *checked, uint32_t *total)
{
    uint32_t done = 0;
    uint32_t all = 0;

    if (store != NULL && store->lockReady) {
        uv_mutex_lock(&store->lock);
        done = store->checked;
        all = store->checkTotal;
        uv_mutex_unlock(&store->lock);
    }
    if (checked != NULL) {
        *checked = done;
    }
    if (total != NULL) {
        *total = all;
    }
}

uint32_t storageFileCount(const storage *store)
{
    return (store != NULL) ? store->fileCount : 0;
}

int32_t storageGetFile(const storage *store, uint32_t index, storageFileState *state)
{
    const storageFile *entry;

    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
    if (store == NULL || state == NULL || !store->opened || index >= store->fileCount) {
        return STORAGE_EINVAL;
    }
    entry = &store->files[index];
    state->path = entry->path;
    state->offset = entry->offset;
    state->length = entry->length;
    state->sizeOnDisk = entry->padding ? entry->length : entry->sizeOnDisk;
    state->isPadding = entry->padding;
    state->existed = (uint8_t)(entry->existed || entry->padding);
    state->notRegular = entry->notRegular;
    state->skipped = entry->skipped;
    state->inPartFile = entry->inPart;
    return STORAGE_OK;
}

const char *storageRoot(const storage *store)
{
    return (store != NULL) ? store->root : NULL;
}

static void readWork(uv_work_t *work)
{
    storageRead *job = work->data;
    uint8_t      missing = 0;

    job->result = rangeIo(job->store, job->index,
                          (uint64_t)job->index * job->store->pieceLength + job->begin,
                          job->length, job->data, 0, job->message, sizeof(job->message),
                          &missing);
    if (job->result == STORAGE_OK && missing) {
        snprintf(job->message, sizeof(job->message), "piece %u is no longer on disk",
                 (unsigned)job->index);
        job->result = STORAGE_EIO;
    }
}

static void readDone(uv_work_t *work, int status)
{
    storageRead   *job = work->data;
    storage       *store = job->store;
    parserContext *context = store->context;

    if (status == UV_ECANCELED) {
        job->result = STORAGE_ECANCELLED;
    }
    if (!store->closed && job->callback != NULL) {
        uint8_t *data = (job->result == STORAGE_OK) ? job->data : NULL;

        if (data == NULL) {
            release(context, job->data);
        }
        job->callback(job->privateArg, store, job->index, job->begin, job->length, data,
                      job->result,
                      (job->result == STORAGE_OK) ? NULL
                      : (job->message[0] != '\0') ? job->message
                                                  : storageStatusString(job->result));
    } else {
        release(context, job->data);
    }
    release(context, job);
    store->jobs--;
    maybeFree(store);
}

int32_t storageReadBlock(storage *store, uint32_t pieceIndex, uint32_t begin, uint32_t length,
                         uint8_t *data, storageReadCallback callback, void *privateArg)
{
    storageRead *job;

    if (store == NULL || data == NULL) {
        return STORAGE_EINVAL;
    }
    if (!store->opened || store->closed || pieceIndex >= store->pieceCount || length == 0 ||
        (uint64_t)begin + length > pieceSize(store, pieceIndex) ||
        !messageBitfieldGet(store->have, pieceIndex)) {
        release(store->context, data);
        return STORAGE_EINVAL;
    }
    job = allocate(store->context, sizeof(*job));
    if (job == NULL) {
        release(store->context, data);
        return STORAGE_ENOMEM;
    }
    job->store = store;
    job->index = pieceIndex;
    job->begin = begin;
    job->length = length;
    job->data = data;
    job->callback = callback;
    job->privateArg = privateArg;
    job->work.data = job;
    if (uv_queue_work(store->loop, &job->work, readWork, readDone) != 0) {
        release(store->context, data);
        release(store->context, job);
        return STORAGE_EIO;
    }
    store->jobs++;
    return STORAGE_OK;
}

void storageClose(storage *store)
{
    if (store == NULL || store->closed) {
        return;
    }
    store->closed = 1;
    if (!store->opened) {
        uv_cancel((uv_req_t *)&store->openWork);    /* only if it has not started */
        uv_mutex_lock(&store->lock);
        store->stopChecking = 1;                    /* check jobs stop at the next piece */
        uv_mutex_unlock(&store->lock);
    }
    maybeFree(store);
}

const char *storageStatusString(int32_t status)
{
    switch (status) {
    case STORAGE_OK:         return "OK";
    case STORAGE_EINVAL:     return "Invalid argument";
    case STORAGE_ENOMEM:     return "Out of memory";
    case STORAGE_EIO:        return "File access failed";
    case STORAGE_ECANCELLED: return "Cancelled";
    default:                 return "Unknown status";
    }
}
