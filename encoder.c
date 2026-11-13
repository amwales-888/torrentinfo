/*
 * encoder.c - BitTorrent metainfo (.torrent) encoder and creator.
 *
 * Output goes into a buffer that grows through the context's realloc hook.
 * Write errors are remembered in the writer and checked once at the end,
 * which keeps the encoding functions linear and easy to compare with the
 * specification.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "encoder.h"

#define ENCODER_STATE_MAGIC             0x454E4344u     /* "ENCD" */
#define ENCODER_WRITER_MIN_CAPACITY     256u

/* Evaluate an expression; return its result early if it is not ENCODER_OK. */
#define ENCODER_TRY(expression)                                 \
    do {                                                        \
        int32_t encoderTryResult = (expression);                \
        if (encoderTryResult != ENCODER_OK) {                   \
            return encoderTryResult;                            \
        }                                                       \
    } while (0)

typedef struct encoderWriter {
    parserContext *context;
    uint8_t       *data;
    uint64_t       length;
    uint64_t       capacity;
    int32_t        error;       /* First error seen; later writes are ignored */
} encoderWriter;

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static uint8_t contextValid(const parserContext *context)
{
    return (uint8_t)(context != NULL && context->magic == PARSER_CONTEXT_MAGIC);
}

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

static int32_t copyString(parserContext *context, const char *text, char **out)
{
    size_t length = strlen(text);
    char  *copy = allocate(context, (uint64_t)length + 1u);

    if (copy == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }
    memcpy(copy, text, length);
    *out = copy;
    return ENCODER_OK;
}

/* A name used as a single path component: not "", ".", "..", and no '/'. */
static uint8_t isSafeName(const char *name)
{
    return (uint8_t)(name[0] != '\0' &&
                     strcmp(name, ".") != 0 &&
                     strcmp(name, "..") != 0 &&
                     strchr(name, '/') == NULL);
}

static uint8_t isPowerOfTwo(uint64_t value)
{
    return (uint8_t)(value != 0 && (value & (value - 1u)) == 0);
}

static void initTorrent(parserTorrent *torrent)
{
    memset(torrent, 0, sizeof(*torrent));
    TAILQ_INIT(&torrent->announceList);
    TAILQ_INIT(&torrent->webSeeds);
    TAILQ_INIT(&torrent->files);
}

static uint64_t countPieces(uint64_t total, uint64_t pieceLength)
{
    return total / pieceLength + (uint64_t)(total % pieceLength != 0);
}

/* ======================================================================== */
/* Bencode writer                                                           */
/* ======================================================================== */

static void writerReserve(encoderWriter *writer, uint64_t extra)
{
    uint64_t needed;
    uint64_t capacity;
    uint8_t *data;

    if (writer->error != ENCODER_OK) {
        return;
    }
    if (extra > UINT64_MAX - writer->length) {
        writer->error = PARSER_ERROR_OVERFLOW;
        return;
    }
    needed = writer->length + extra;
    if (needed <= writer->capacity) {
        return;
    }

    capacity = (writer->capacity != 0) ? writer->capacity : ENCODER_WRITER_MIN_CAPACITY;
    while (capacity < needed) {
        if (capacity > UINT64_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > (uint64_t)(SIZE_MAX / 2u)) {
        writer->error = PARSER_ERROR_NO_MEMORY;
        return;
    }

    data = writer->context->allocator.realloc(writer->data, (size_t)capacity);
    if (data == NULL) {
        writer->error = PARSER_ERROR_NO_MEMORY;     /* old buffer is still valid */
        return;
    }
    writer->data = data;
    writer->capacity = capacity;
}

static void writeBytes(encoderWriter *writer, const void *bytes, uint64_t length)
{
    writerReserve(writer, length);
    if (writer->error != ENCODER_OK) {
        return;
    }
    if (length > 0) {
        memcpy(writer->data + writer->length, bytes, (size_t)length);
    }
    writer->length += length;
}

static void writeByte(encoderWriter *writer, uint8_t value)
{
    writeBytes(writer, &value, 1u);
}

static void writeUnsigned(encoderWriter *writer, uint64_t value)
{
    uint8_t  digits[20];
    uint32_t count = 0;

    do {
        digits[sizeof(digits) - 1u - count] = (uint8_t)('0' + (value % 10u));
        value /= 10u;
        count++;
    } while (value != 0);

    writeBytes(writer, digits + sizeof(digits) - count, count);
}

/* i<value>e */
static void writeInteger(encoderWriter *writer, int64_t value)
{
    writeByte(writer, 'i');
    if (value < 0) {
        writeByte(writer, '-');
        writeUnsigned(writer, (uint64_t)(-(value + 1)) + 1u);  /* safe for INT64_MIN */
    } else {
        writeUnsigned(writer, (uint64_t)value);
    }
    writeByte(writer, 'e');
}

/* <length>:<bytes> */
static void writeString(encoderWriter *writer, const uint8_t *data, uint64_t length)
{
    writeUnsigned(writer, length);
    writeByte(writer, ':');
    writeBytes(writer, data, length);
}

static void writeText(encoderWriter *writer, const char *text)
{
    writeString(writer, (const uint8_t *)text, strlen(text));
}

static void writeUrlList(encoderWriter *writer, const struct parserUrlList *list)
{
    const parserUrl *url;

    writeByte(writer, 'l');
    TAILQ_FOREACH(url, list, entries) {
        writeText(writer, url->url);
    }
    writeByte(writer, 'e');
}

/* ======================================================================== */
/* Torrent encoding. Keys must be written in sorted (raw byte) order.       */
/* ======================================================================== */

/* Check that the torrent is complete enough to produce a valid file. */
static int32_t validateTorrent(const parserTorrent *torrent)
{
    const parserFile *file;
    uint64_t          total = 0;
    uint32_t          count = 0;

    if (torrent->name == NULL || torrent->pieceLength == 0 ||
        TAILQ_EMPTY(&torrent->files) ||
        (torrent->pieceCount > 0 && torrent->pieces == NULL)) {
        return PARSER_ERROR_INVALID_FIELD;
    }
    TAILQ_FOREACH(file, &torrent->files, entries) {
        if (TAILQ_EMPTY(&file->path)) {
            return PARSER_ERROR_INVALID_FIELD;
        }
        if (file->length > UINT64_MAX - total) {
            return PARSER_ERROR_OVERFLOW;
        }
        total += file->length;
        count++;
    }
    if (!torrent->isMultiFile && count != 1) {
        return PARSER_ERROR_INVALID_FIELD;
    }
    if (countPieces(total, torrent->pieceLength) != (uint64_t)torrent->pieceCount) {
        return PARSER_ERROR_INVALID_FIELD;
    }
    return ENCODER_OK;
}

/* info: files | length, name, piece length, pieces, private */
static void encodeInfo(encoderWriter *writer, const parserTorrent *torrent)
{
    const parserFile          *file;
    const parserPathComponent *component;

    writeByte(writer, 'd');

    if (torrent->isMultiFile) {
        writeText(writer, "files");
        writeByte(writer, 'l');
        TAILQ_FOREACH(file, &torrent->files, entries) {
            /* attr, length, path */
            writeByte(writer, 'd');
            if (file->isPadding) {
                writeText(writer, "attr");
                writeText(writer, "p");
            }
            writeText(writer, "length");
            writeInteger(writer, (int64_t)file->length);
            writeText(writer, "path");
            writeByte(writer, 'l');
            TAILQ_FOREACH(component, &file->path, entries) {
                writeText(writer, component->name);
            }
            writeByte(writer, 'e');
            writeByte(writer, 'e');
        }
        writeByte(writer, 'e');
    } else {
        writeText(writer, "length");
        writeInteger(writer, (int64_t)TAILQ_FIRST(&torrent->files)->length);
    }

    writeText(writer, "name");
    writeText(writer, torrent->name);
    writeText(writer, "piece length");
    writeInteger(writer, (int64_t)torrent->pieceLength);
    writeText(writer, "pieces");
    writeString(writer, torrent->pieces,
                (uint64_t)torrent->pieceCount * PARSER_SHA1_LENGTH);
    if (torrent->isPrivate) {
        writeText(writer, "private");
        writeInteger(writer, 1);
    }

    writeByte(writer, 'e');
}

/* root: announce, announce-list, comment, created by, creation date,
 *       encoding, info, url-list */
static void encodeTorrent(encoderWriter *writer, const parserTorrent *torrent)
{
    const parserTier *tier;

    writeByte(writer, 'd');

    if (torrent->announce != NULL) {
        writeText(writer, "announce");
        writeText(writer, torrent->announce);
    }
    if (!TAILQ_EMPTY(&torrent->announceList)) {
        writeText(writer, "announce-list");
        writeByte(writer, 'l');
        TAILQ_FOREACH(tier, &torrent->announceList, entries) {
            writeUrlList(writer, &tier->urls);
        }
        writeByte(writer, 'e');
    }
    if (torrent->comment != NULL) {
        writeText(writer, "comment");
        writeText(writer, torrent->comment);
    }
    if (torrent->createdBy != NULL) {
        writeText(writer, "created by");
        writeText(writer, torrent->createdBy);
    }
    if (torrent->hasCreationDate) {
        writeText(writer, "creation date");
        writeInteger(writer, torrent->creationDate);
    }
    if (torrent->encoding != NULL) {
        writeText(writer, "encoding");
        writeText(writer, torrent->encoding);
    }

    writeText(writer, "info");
    encodeInfo(writer, torrent);

    if (!TAILQ_EMPTY(&torrent->webSeeds)) {
        writeText(writer, "url-list");
        writeUrlList(writer, &torrent->webSeeds);
    }

    writeByte(writer, 'e');
}

/* ======================================================================== */
/* Torrent creation                                                         */
/* ======================================================================== */

static void freeSources(encoderState *encoder)
{
    encoderSourceFile *source;

    while ((source = TAILQ_FIRST(&encoder->files)) != NULL) {
        TAILQ_REMOVE(&encoder->files, source, entries);
        release(encoder->context, source->diskPath);
        release(encoder->context, source);
    }
    encoder->fileCount = 0;
    encoder->totalLength = 0;
    encoder->magic = 0;
}

static uint64_t choosePieceLength(uint64_t total)
{
    uint64_t length = ENCODER_MIN_PIECE_LENGTH;

    while (length < ENCODER_AUTO_MAX_PIECE_LENGTH &&
           total / length > ENCODER_TARGET_PIECE_COUNT) {
        length <<= 1;
    }
    return length;
}

static int32_t appendUrl(parserContext *context, struct parserUrlList *list,
                         uint32_t *count, const char *text)
{
    parserUrl *url;

    if (text == NULL) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    if (text[0] == '\0') {
        return ENCODER_OK;                          /* ignore empty URLs */
    }
    url = allocate(context, sizeof(*url));
    if (url == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }
    if (copyString(context, text, &url->url) != ENCODER_OK) {
        release(context, url);
        return PARSER_ERROR_NO_MEMORY;
    }
    TAILQ_INSERT_TAIL(list, url, entries);
    (*count)++;
    return ENCODER_OK;
}

/* Copy trackers, web seeds and descriptive fields from the options. */
static int32_t buildMetadata(parserContext *context, const encoderOptions *options,
                             parserTorrent *torrent)
{
    parserTier *tier;
    uint32_t    i;

    if (options->trackerCount > 0) {
        if (options->trackers[0] == NULL) {
            return PARSER_ERROR_INVALID_ARGUMENT;
        }
        ENCODER_TRY(copyString(context, options->trackers[0], &torrent->announce));
    }
    /* With more than one tracker, also write announce-list: one tier each. */
    if (options->trackerCount > 1) {
        for (i = 0; i < options->trackerCount; i++) {
            tier = allocate(context, sizeof(*tier));
            if (tier == NULL) {
                return PARSER_ERROR_NO_MEMORY;
            }
            TAILQ_INIT(&tier->urls);
            TAILQ_INSERT_TAIL(&torrent->announceList, tier, entries);
            torrent->tierCount++;
            ENCODER_TRY(appendUrl(context, &tier->urls, &tier->urlCount,
                                  options->trackers[i]));
            if (tier->urlCount == 0) {
                TAILQ_REMOVE(&torrent->announceList, tier, entries);
                torrent->tierCount--;
                release(context, tier);
            }
        }
    }
    for (i = 0; i < options->webSeedCount; i++) {
        ENCODER_TRY(appendUrl(context, &torrent->webSeeds, &torrent->webSeedCount,
                              options->webSeeds[i]));
    }
    if (options->comment != NULL) {
        ENCODER_TRY(copyString(context, options->comment, &torrent->comment));
    }
    if (options->createdBy != NULL) {
        ENCODER_TRY(copyString(context, options->createdBy, &torrent->createdBy));
    }
    torrent->creationDate = options->creationDate;
    torrent->hasCreationDate = options->hasCreationDate ? 1u : 0u;
    torrent->isPrivate = options->isPrivate ? 1u : 0u;
    return ENCODER_OK;
}

static int32_t buildFiles(encoderState *encoder, parserTorrent *torrent)
{
    parserContext       *context = encoder->context;
    encoderSourceFile   *source;
    parserFile          *file;
    parserPathComponent *component;
    const char          *componentName;

    torrent->isMultiFile = (uint8_t)(encoder->fileCount > 1);

    TAILQ_FOREACH(source, &encoder->files, entries) {
        file = allocate(context, sizeof(*file));
        if (file == NULL) {
            return PARSER_ERROR_NO_MEMORY;
        }
        TAILQ_INIT(&file->path);
        TAILQ_INSERT_TAIL(&torrent->files, file, entries);
        torrent->fileCount++;

        file->length = source->length;
        file->offset = torrent->totalLength;
        torrent->totalLength += source->length;     /* overflow checked in AddFile */

        /* Single-file torrents use the torrent name as the file's path,
         * matching what parserParse() produces. */
        componentName = torrent->isMultiFile ? source->name : torrent->name;

        component = allocate(context, sizeof(*component));
        if (component == NULL) {
            return PARSER_ERROR_NO_MEMORY;
        }
        if (copyString(context, componentName, &component->name) != ENCODER_OK) {
            release(context, component);
            return PARSER_ERROR_NO_MEMORY;
        }
        TAILQ_INSERT_TAIL(&file->path, component, entries);
        file->pathCount = 1;
    }
    return ENCODER_OK;
}

/*
 * Read one file into the piece buffer, hashing each piece as it fills.
 * Pieces span file boundaries, so 'fill' carries over between files.
 */
static int32_t hashFile(encoderState *encoder, const encoderSourceFile *source,
                        parserTorrent *torrent, uint8_t *piece,
                        uint64_t *fill, uint32_t *index)
{
    struct stat info;
    uint64_t    remaining = source->length;
    uint64_t    wanted;
    ssize_t     count;
    int         fd;

    fd = open(source->diskPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        encoder->systemError = errno;
        return ENCODER_ERROR_IO;
    }
    if (fstat(fd, &info) != 0) {
        encoder->systemError = errno;
        close(fd);
        return ENCODER_ERROR_IO;
    }
    if (!S_ISREG(info.st_mode) || (uint64_t)info.st_size != source->length) {
        close(fd);
        return ENCODER_ERROR_FILE_CHANGED;
    }

    while (remaining > 0) {
        wanted = torrent->pieceLength - *fill;
        if (wanted > remaining) {
            wanted = remaining;
        }
        count = read(fd, piece + *fill, (size_t)wanted);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            encoder->systemError = errno;
            close(fd);
            return ENCODER_ERROR_IO;
        }
        if (count == 0) {
            close(fd);
            return ENCODER_ERROR_FILE_CHANGED;      /* file shrank */
        }
        *fill += (uint64_t)count;
        remaining -= (uint64_t)count;

        if (*fill == torrent->pieceLength) {
            if (*index >= torrent->pieceCount) {
                close(fd);
                return ENCODER_ERROR_FILE_CHANGED;
            }
            parserSha1(piece, *fill,
                       torrent->pieces + (uint64_t)*index * PARSER_SHA1_LENGTH);
            (*index)++;
            *fill = 0;
        }
    }
    close(fd);
    return ENCODER_OK;
}

static int32_t hashPieces(encoderState *encoder, parserTorrent *torrent)
{
    parserContext     *context = encoder->context;
    encoderSourceFile *source;
    uint8_t           *piece;
    uint64_t           fill = 0;
    uint32_t           index = 0;
    uint32_t           fileIndex = 0;
    int32_t            result = ENCODER_OK;

    if (torrent->pieceCount == 0) {
        return ENCODER_OK;                          /* only empty files */
    }
    torrent->pieces = allocate(context, (uint64_t)torrent->pieceCount * PARSER_SHA1_LENGTH);
    if (torrent->pieces == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }
    piece = allocate(context, torrent->pieceLength);
    if (piece == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }

    TAILQ_FOREACH(source, &encoder->files, entries) {
        result = hashFile(encoder, source, torrent, piece, &fill, &index);
        if (result != ENCODER_OK) {
            encoder->errorFileIndex = fileIndex;
            break;
        }
        fileIndex++;
    }

    if (result == ENCODER_OK && fill > 0) {         /* final, shorter piece */
        if (index >= torrent->pieceCount) {
            result = ENCODER_ERROR_FILE_CHANGED;
        } else {
            parserSha1(piece, fill, torrent->pieces + (uint64_t)index * PARSER_SHA1_LENGTH);
            index++;
        }
    }
    if (result == ENCODER_OK && index != torrent->pieceCount) {
        result = ENCODER_ERROR_FILE_CHANGED;
    }

    release(context, piece);
    return result;
}

/* The info hash is the SHA-1 of the bencoded info dictionary. */
static int32_t computeInfoHash(parserContext *context, parserTorrent *torrent)
{
    encoderWriter writer;

    memset(&writer, 0, sizeof(writer));
    writer.context = context;
    encodeInfo(&writer, torrent);
    if (writer.error != ENCODER_OK) {
        release(context, writer.data);
        return writer.error;
    }
    parserSha1(writer.data, writer.length, torrent->infoHash);
    release(context, writer.data);
    return ENCODER_OK;
}

static int32_t buildTorrent(encoderState *encoder, parserTorrent *torrent)
{
    parserContext        *context = encoder->context;
    const encoderOptions *options = &encoder->options;
    const char           *name;
    uint64_t              pieceCount;

    if (encoder->fileCount == 0) {
        return ENCODER_ERROR_NO_FILES;
    }

    if (options->name != NULL) {
        name = options->name;
    } else if (encoder->fileCount == 1) {
        name = TAILQ_FIRST(&encoder->files)->name;
    } else {
        return ENCODER_ERROR_INVALID_NAME;          /* several files need a name */
    }
    ENCODER_TRY(copyString(context, name, &torrent->name));

    torrent->pieceLength = (options->pieceLength != 0)
                         ? options->pieceLength
                         : choosePieceLength(encoder->totalLength);
    pieceCount = countPieces(encoder->totalLength, torrent->pieceLength);
    if (pieceCount > UINT32_MAX) {
        return ENCODER_ERROR_TOO_LARGE;
    }
    torrent->pieceCount = (uint32_t)pieceCount;

    ENCODER_TRY(buildMetadata(context, options, torrent));
    ENCODER_TRY(buildFiles(encoder, torrent));
    ENCODER_TRY(hashPieces(encoder, torrent));
    ENCODER_TRY(computeInfoHash(context, torrent));
    return ENCODER_OK;
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int32_t encoderEncode(parserContext *context, const parserTorrent *torrent,
                      uint8_t **buffer, uint64_t *length)
{
    encoderWriter writer;

    if (buffer == NULL || length == NULL) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    *buffer = NULL;
    *length = 0;
    if (context == NULL || torrent == NULL) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    if (!contextValid(context)) {
        return PARSER_ERROR_NOT_INITIALISED;
    }
    ENCODER_TRY(validateTorrent(torrent));

    memset(&writer, 0, sizeof(writer));
    writer.context = context;
    encodeTorrent(&writer, torrent);
    if (writer.error != ENCODER_OK) {
        release(context, writer.data);
        return writer.error;
    }

    *buffer = writer.data;
    *length = writer.length;
    return ENCODER_OK;
}

void encoderFreeBuffer(parserContext *context, uint8_t *buffer)
{
    if (contextValid(context)) {
        release(context, buffer);
    }
}

int32_t encoderBegin(encoderState *encoder, parserContext *context,
                     const encoderOptions *options)
{
    if (encoder == NULL) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    memset(encoder, 0, sizeof(*encoder));
    TAILQ_INIT(&encoder->files);

    if (context == NULL || options == NULL) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    if (!contextValid(context)) {
        return PARSER_ERROR_NOT_INITIALISED;
    }
    if (options->pieceLength != 0 &&
        (options->pieceLength < ENCODER_MIN_PIECE_LENGTH ||
         options->pieceLength > ENCODER_MAX_PIECE_LENGTH ||
         !isPowerOfTwo(options->pieceLength))) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    if ((options->trackerCount > 0 && options->trackers == NULL) ||
        (options->webSeedCount > 0 && options->webSeeds == NULL)) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    if (options->name != NULL && !isSafeName(options->name)) {
        return ENCODER_ERROR_INVALID_NAME;
    }

    encoder->context = context;
    encoder->options = *options;
    encoder->magic = ENCODER_STATE_MAGIC;
    return ENCODER_OK;
}

int32_t encoderAddFile(encoderState *encoder, const char *path)
{
    parserContext     *context;
    encoderSourceFile *source;
    encoderSourceFile *existing;
    const char        *slash;
    const char        *name;
    struct stat        info;
    uint64_t           length;

    if (encoder == NULL || path == NULL || encoder->magic != ENCODER_STATE_MAGIC) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    context = encoder->context;
    encoder->errorFileIndex = encoder->fileCount;
    encoder->systemError = 0;

    if (stat(path, &info) != 0) {
        encoder->systemError = errno;
        return ENCODER_ERROR_IO;
    }
    if (!S_ISREG(info.st_mode)) {
        return ENCODER_ERROR_NOT_REGULAR_FILE;
    }
    length = (uint64_t)info.st_size;

    slash = strrchr(path, '/');
    name = (slash != NULL) ? slash + 1 : path;
    if (!isSafeName(name)) {
        return ENCODER_ERROR_INVALID_NAME;
    }

    TAILQ_FOREACH(existing, &encoder->files, entries) {
        if (strcmp(existing->name, name) == 0) {
            return ENCODER_ERROR_DUPLICATE_FILE;
        }
    }
    if (length > UINT64_MAX - encoder->totalLength ||
        encoder->totalLength + length > (uint64_t)INT64_MAX) {
        return ENCODER_ERROR_TOO_LARGE;             /* bencode integers are signed */
    }

    source = allocate(context, sizeof(*source));
    if (source == NULL) {
        return PARSER_ERROR_NO_MEMORY;
    }
    if (copyString(context, path, &source->diskPath) != ENCODER_OK) {
        release(context, source);
        return PARSER_ERROR_NO_MEMORY;
    }
    source->name = source->diskPath + (name - path);
    source->length = length;

    TAILQ_INSERT_TAIL(&encoder->files, source, entries);
    encoder->fileCount++;
    encoder->totalLength += length;
    return ENCODER_OK;
}

int32_t encoderEnd(encoderState *encoder, parserTorrent *torrent)
{
    int32_t result;

    if (torrent != NULL) {
        initTorrent(torrent);
    }
    if (encoder == NULL || encoder->magic != ENCODER_STATE_MAGIC) {
        return PARSER_ERROR_INVALID_ARGUMENT;
    }
    encoder->systemError = 0;

    if (torrent == NULL) {
        result = PARSER_ERROR_INVALID_ARGUMENT;
    } else {
        result = buildTorrent(encoder, torrent);
        if (result != ENCODER_OK) {
            parserFree(encoder->context, torrent);
        }
    }
    freeSources(encoder);
    return result;
}

void encoderAbort(encoderState *encoder)
{
    if (encoder != NULL && encoder->magic == ENCODER_STATE_MAGIC) {
        freeSources(encoder);
    }
}

const char *encoderErrorString(int32_t error)
{
    switch (error) {
    case ENCODER_ERROR_IO:               return "I/O error";
    case ENCODER_ERROR_NOT_REGULAR_FILE: return "Not a regular file";
    case ENCODER_ERROR_DUPLICATE_FILE:   return "A file with the same name was already added";
    case ENCODER_ERROR_NO_FILES:         return "No files added";
    case ENCODER_ERROR_FILE_CHANGED:     return "File changed while being hashed";
    case ENCODER_ERROR_TOO_LARGE:        return "Too much data";
    case ENCODER_ERROR_INVALID_NAME:     return "Invalid or missing torrent name";
    default:                             return parserErrorString(error);
    }
}
