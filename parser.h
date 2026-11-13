/*
 * parser.h - BitTorrent metainfo (.torrent) parser.
 *
 * Parses BEP 3 (v1) metainfo files, plus the common extensions:
 *   - BEP 12 announce-list (multi-tracker tiers)
 *   - BEP 19 url-list (HTTP/FTP web seeds)
 *   - BEP 27 private flag
 *   - BEP 47 padding file attribute
 *
 * The library never calls malloc/realloc/free directly. Every allocation
 * goes through the parserAllocator supplied to parserInit().
 *
 * Lists use the BSD <sys/queue.h> TAILQ macros.
 *
 * Usage:
 *     parserContext context;
 *     parserTorrent torrent;
 *     parserAllocator allocator = { .realloc = realloc, .free = free };
 *
 *     parserInit(&context, &allocator);
 *     if (parserParse(&context, buffer, length, &torrent) == PARSER_OK) {
 *         ... use torrent ...
 *         parserFree(&context, &torrent);
 *     }
 *
 * NOTE: A parserTorrent contains TAILQ heads, which point back into the
 * structure itself. Do not copy a parsed parserTorrent by value; pass it
 * around by pointer.
 */
#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>     /* size_t: needed so realloc()/free() can be passed directly */
#include <stdint.h>
#include <sys/queue.h>  /* BSD queue.h */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define PARSER_SHA1_LENGTH              20u     /* Bytes in a SHA-1 digest      */
#define PARSER_MAX_DEPTH                64u     /* Max nesting of lists/dicts   */
#define PARSER_CONTEXT_MAGIC            0x50415253u /* Marks an initialised context */

/* Return codes */
#define PARSER_OK                       0
#define PARSER_ERROR_INVALID_ARGUMENT   (-1)    /* NULL pointer / bad argument  */
#define PARSER_ERROR_NOT_INITIALISED    (-2)    /* parserInit() not called      */
#define PARSER_ERROR_NO_MEMORY          (-3)    /* Allocator returned NULL      */
#define PARSER_ERROR_TRUNCATED          (-4)    /* Unexpected end of data       */
#define PARSER_ERROR_SYNTAX             (-5)    /* Malformed bencode            */
#define PARSER_ERROR_OVERFLOW           (-6)    /* Integer out of range         */
#define PARSER_ERROR_TOO_DEEP           (-7)    /* Nesting > PARSER_MAX_DEPTH   */
#define PARSER_ERROR_DUPLICATE_KEY      (-8)    /* Same key twice in a dict     */
#define PARSER_ERROR_MISSING_FIELD      (-9)    /* Required key not present     */
#define PARSER_ERROR_INVALID_FIELD      (-10)   /* Key present, value invalid   */

/* ------------------------------------------------------------------------ */
/* Library context                                                          */
/* ------------------------------------------------------------------------ */

/*
 * Memory hooks. The signatures match the C library so that the standard
 * realloc and free may be passed directly. The parser only ever calls
 * realloc(NULL, size) to allocate and free(ptr) to release.
 */
typedef struct parserAllocator {
    void *(*realloc)(void *ptr, size_t size);
    void  (*free)(void *ptr);
} parserAllocator;

/*
 * Library context, initialised by parserInit(). A context may be reused for
 * any number of parses, but must not be shared between threads concurrently
 * (errorOffset is written on failure).
 */
typedef struct parserContext {
    parserAllocator allocator;
    uint64_t        errorOffset;    /* Byte offset of the last parse error   */
    uint32_t        magic;          /* Internal: set by parserInit()         */
} parserContext;

/* ------------------------------------------------------------------------ */
/* Torrent structure                                                        */
/* ------------------------------------------------------------------------ */

/* A URL: used for both tracker URLs and web seeds. */
typedef struct parserUrl {
    char                   *url;            /* NUL-terminated                */
    TAILQ_ENTRY(parserUrl)  entries;
} parserUrl;
TAILQ_HEAD(parserUrlList, parserUrl);

/* One tier of the announce-list (BEP 12). */
typedef struct parserTier {
    struct parserUrlList    urls;
    uint32_t                urlCount;
    TAILQ_ENTRY(parserTier) entries;
} parserTier;
TAILQ_HEAD(parserTierList, parserTier);

/* One component of a file path, e.g. "dir" and "file.txt" in dir/file.txt. */
typedef struct parserPathComponent {
    char                             *name; /* NUL-terminated, never "", ".", ".." and
                                               never contains '/'               */
    TAILQ_ENTRY(parserPathComponent)  entries;
} parserPathComponent;
TAILQ_HEAD(parserPathList, parserPathComponent);

/*
 * A file within the torrent.
 *
 * Single-file torrents are presented as a file list containing one entry
 * whose path is the torrent name, so callers can always walk 'files'.
 * In multi-file torrents paths are relative to a directory called 'name'.
 */
typedef struct parserFile {
    uint64_t                    length;     /* Size in bytes                 */
    uint64_t                    offset;     /* Byte offset within the torrent's
                                               concatenated data             */
    struct parserPathList       path;
    uint32_t                    pathCount;
    uint8_t                     isPadding;  /* BEP 47 attr contains 'p'      */
    TAILQ_ENTRY(parserFile)     entries;
} parserFile;
TAILQ_HEAD(parserFileList, parserFile);

typedef struct parserTorrent {
    /* Root dictionary (all optional) */
    char                   *announce;       /* Primary tracker, or NULL       */
    struct parserTierList   announceList;   /* BEP 12 tiers                   */
    uint32_t                tierCount;
    struct parserUrlList    webSeeds;       /* BEP 19 url-list                */
    uint32_t                webSeedCount;
    char                   *comment;        /* or NULL                        */
    char                   *createdBy;      /* or NULL                        */
    char                   *encoding;       /* or NULL                        */
    int64_t                 creationDate;   /* Unix time, if hasCreationDate  */
    uint8_t                 hasCreationDate;

    /* Info dictionary */
    uint8_t                 infoHash[PARSER_SHA1_LENGTH]; /* SHA-1 of raw info dict */
    uint64_t                infoOffset;     /* Raw info dict location within  */
    uint64_t                infoLength;     /* the buffer passed to parserParse */
    char                   *name;
    uint64_t                pieceLength;    /* Bytes per piece, > 0           */
    uint8_t                *pieces;         /* pieceCount * 20 bytes of SHA-1 */
    uint32_t                pieceCount;
    uint8_t                 isPrivate;      /* BEP 27                         */
    uint8_t                 isMultiFile;
    uint64_t                totalLength;    /* Sum of all file lengths        */
    struct parserFileList   files;
    uint32_t                fileCount;
} parserTorrent;

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Initialise a library context. The allocator structure is copied, so it
 * need not outlive this call. Both function pointers must be non-NULL.
 */
int32_t parserInit(parserContext *context, const parserAllocator *allocator);

/*
 * Parse a .torrent file held in 'buffer' into a caller-allocated 'torrent'.
 *
 * The buffer is only read during this call; all data is copied into the
 * torrent, so the buffer may be released afterwards.
 *
 * On success returns PARSER_OK; release the contents with parserFree().
 * On failure returns a PARSER_ERROR_* code, sets context->errorOffset, and
 * leaves the torrent empty (anything partially allocated is already freed;
 * calling parserFree() on it is harmless).
 */
int32_t parserParse(parserContext *context, const uint8_t *buffer,
                    uint64_t length, parserTorrent *torrent);

/*
 * Free everything owned by 'torrent' (not the structure itself) and reset
 * it to an empty state. Safe to call more than once.
 */
void parserFree(parserContext *context, parserTorrent *torrent);

/* Human-readable description of a PARSER_* return code. Never NULL. */
const char *parserErrorString(int32_t error);

/* One-shot SHA-1 of 'length' bytes. Used for info hashes and piece hashes. */
void parserSha1(const uint8_t *data, uint64_t length,
                uint8_t digest[PARSER_SHA1_LENGTH]);

#ifdef __cplusplus
}
#endif

#endif /* PARSER_H */
