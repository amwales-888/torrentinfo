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
 * Files are opened per operation, which keeps the code simple and the
 * handle count low; a handle cache can come later if needed.
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
} storageFile;

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

    /* The open job */
    uv_work_t               openWork;
    storageOpenCallback     openCallback;
    void                   *openArg;
    uint8_t                *scratch;        /* one piece, for the resume check    */
    int32_t                 openResult;
    char                    openMessage[STORAGE_ERROR_MESSAGE_LENGTH];

    uint32_t                jobs;           /* thread-pool jobs not yet completed */
    uint8_t                 opened;
    uint8_t                 closed;
};

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
        release(context, store->files[i].path);
    }
    release(context, store->files);
    release(context, store->root);
    release(context, store->hashes);
    release(context, store->have);
    release(context, store->scratch);
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
static int32_t pieceIo(const storage *store, uint32_t index, uint8_t *buffer, uint8_t write,
                       char *message, size_t messageSize, uint8_t *missing)
{
    uint64_t     position = (uint64_t)index * store->pieceLength;
    uint32_t     remaining = pieceSize(store, index);
    uint32_t     fileIndex = findFile(store, position);
    uint32_t     done;
    uint32_t     count;
    uint32_t     chunk;
    uint64_t     within;
    platformFile file;
    int32_t      result;

    while (remaining > 0 && fileIndex < store->fileCount) {
        const storageFile *entry = &store->files[fileIndex++];

        within = position - entry->offset;
        if (within >= entry->length) {
            continue;                               /* an empty file */
        }
        chunk = (entry->length - within < remaining) ? (uint32_t)(entry->length - within)
                                                     : remaining;
        if (entry->padding) {
            if (!write) {
                memset(buffer, 0, chunk);
            }
        } else {
            result = write ? platformFileOpenReadWrite(entry->path, &file)
                           : platformFileOpenRead(entry->path, &file);
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
                        platformFileClose(&file);
                        return STORAGE_OK;
                    }
                    result = UV_EIO;
                }
                if (result != 0) {
                    snprintf(message, messageSize, "%s: %s", entry->path, uv_strerror(result));
                    platformFileClose(&file);
                    return STORAGE_EIO;
                }
            }
            result = platformFileClose(&file);
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

/* Worker thread: does piece 'index' lie entirely in data that existed before? */
static uint8_t pieceInExistingData(const storage *store, uint32_t index)
{
    uint64_t start = (uint64_t)index * store->pieceLength;
    uint64_t end = start + pieceSize(store, index);
    uint32_t i;

    for (i = findFile(store, start); i < store->fileCount && store->files[i].offset < end; i++) {
        if (store->files[i].length > 0 && !store->files[i].padding && !store->files[i].existed) {
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
    uint8_t      missing;
    uint8_t      digest[PARSER_SHA1_LENGTH];
    uint32_t     i;
    int32_t      result;

    store->openResult = STORAGE_OK;
    for (i = 0; i < store->fileCount; i++) {
        storageFile *entry = &store->files[i];

        if (entry->padding) {
            continue;
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

    /* Resume: hash every piece that lies entirely in data that was there. */
    if (!store->checkExisting || !anyExisted) {
        return;
    }
    for (i = 0; i < store->pieceCount; i++) {
        if (!pieceInExistingData(store, i)) {
            continue;
        }
        missing = 0;
        if (pieceIo(store, i, store->scratch, 0, store->openMessage,
                    sizeof(store->openMessage), &missing) != STORAGE_OK) {
            store->openResult = STORAGE_EIO;
            return;
        }
        if (missing) {
            continue;
        }
        parserSha1(store->scratch, pieceSize(store, i), digest);
        if (memcmp(digest, store->hashes + (uint64_t)i * PARSER_SHA1_LENGTH,
                   PARSER_SHA1_LENGTH) == 0) {
            messageBitfieldSet(store->have, i);
            store->haveCount++;
        }
    }
}

/*
 * In openDone() and writeDone() the job still counts in 'jobs' while the
 * callback runs, so a storageClose() from inside the callback cannot free
 * the storage under our feet; the count drops afterwards.
 */
static void openDone(uv_work_t *work, int status)
{
    storage *store = work->data;

    release(store->context, store->scratch);
    store->scratch = NULL;
    if (status == UV_ECANCELED) {
        store->openResult = STORAGE_ECANCELLED;
    }
    if (!store->closed) {
        store->opened = (uint8_t)(store->openResult == STORAGE_OK);
        store->openCallback(store->openArg, store,
                            store->openResult,
                            (store->openResult == STORAGE_OK) ? NULL
                            : (store->openMessage[0] != '\0') ? store->openMessage
                                                              : storageStatusString(store->openResult));
    }
    store->jobs--;
    maybeFree(store);
}

/* Build every file's full path, adjusted for the platform. */
static int32_t buildFiles(storage *store, const parserTorrent *torrent, const char *directory)
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
                    const char *directory, uint32_t flags, storageOpenCallback callback,
                    void *privateArg, storage **handle)
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
    store->checkExisting = (uint8_t)((flags & STORAGE_CHECK_EXISTING) != 0);
    store->openCallback = callback;
    store->openArg = privateArg;
    store->fileCount = torrent->fileCount;

    hashBytes = (uint64_t)torrent->pieceCount * PARSER_SHA1_LENGTH;
    store->files = allocate(context, (uint64_t)store->fileCount * sizeof(*store->files));
    store->hashes = (hashBytes > 0) ? allocate(context, hashBytes) : NULL;
    store->have = (torrent->pieceCount > 0)
                ? allocate(context, messageBitfieldBytes(torrent->pieceCount)) : NULL;
    if (store->checkExisting && torrent->pieceCount > 0) {
        store->scratch = allocate(context, torrent->pieceLength);
    }
    result = STORAGE_OK;
    if (store->files == NULL || (hashBytes > 0 && (store->hashes == NULL || store->have == NULL)) ||
        (store->checkExisting && torrent->pieceCount > 0 && store->scratch == NULL)) {
        result = STORAGE_ENOMEM;
    }
    if (result == STORAGE_OK) {
        if (hashBytes > 0) {
            memcpy(store->hashes, torrent->pieces, (size_t)hashBytes);
        }
        result = buildFiles(store, torrent, directory);
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
    if (!store->opened || store->closed || pieceIndex >= store->pieceCount) {
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

const char *storageRoot(const storage *store)
{
    return (store != NULL) ? store->root : NULL;
}

void storageClose(storage *store)
{
    if (store == NULL || store->closed) {
        return;
    }
    store->closed = 1;
    if (!store->opened) {
        uv_cancel((uv_req_t *)&store->openWork);    /* only if it has not started */
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
