/*
 * resume.h - Fast resume files.
 *
 * A resume file records what a download knew when it last stopped, so the
 * next start does not have to read and hash all the data again: which
 * pieces were verified and on disk, each file's size and modification time
 * at that moment (storageResume), and optionally the blocks of pieces that
 * were only partly downloaded.
 *
 * Format (integers big-endian):
 *
 *   "TIRS"  uint32 version (1)  info hash[20]
 *   uint32 pieceCount  uint32 fileCount
 *   fileCount x { uint64 size, int64 modified }
 *   bitfield of verified pieces, (pieceCount + 7) / 8 bytes
 *   uint32 partialCount
 *   partialCount x { uint32 index, uint32 length, uint32 blockCount,
 *                    block bitfield (blockCount + 7) / 8 bytes,
 *                    the present blocks' data, in order }
 *   SHA-1 of everything above[20]
 *
 * Saving writes "<path>.tmp" and then replaces <path> in one step, so a
 * crash leaves the old file or the new one. A file that is damaged, from
 * another version, or for another torrent is refused (RESUME_EINVAL) and
 * the caller falls back to checking the data.
 *
 * Memory comes from the parser context; nothing here keeps state.
 * resumeSeal() and resumeWrite() allocate nothing, so they may run on a
 * thread-pool thread.
 */
#ifndef RESUME_H
#define RESUME_H

#include <stdint.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RESUME_OK               0
#define RESUME_EINVAL           (-3)    /* damaged, or not for this torrent */
#define RESUME_ENOMEM           (-4)
#define RESUME_EIO              (-5)    /* cannot read or write the file    */

#define RESUME_VERSION          1u
#define RESUME_MAX_FILE         (512u * 1024u * 1024u)

/* A partly downloaded piece. */
typedef struct resumePartial {
    uint32_t    index;
    uint32_t    length;                 /* the piece's length               */
    uint32_t    blockCount;
    uint8_t    *blocks;                 /* bitfield: blocks present          */
    uint8_t    *data;                   /* 'length' bytes; only the present
                                           blocks mean anything             */
} resumePartial;

typedef struct resumeData {
    uint8_t         infoHash[PARSER_SHA1_LENGTH];
    uint32_t        pieceCount;
    uint32_t        fileCount;
    uint8_t        *have;               /* bitfield                          */
    uint64_t       *fileSizes;
    int64_t        *fileTimes;
    resumePartial  *partials;
    uint32_t        partialCount;
} resumeData;

/*
 * Read 'path' into *data, which must be resumeFree()d. RESUME_EIO (with the
 * libuv error in *systemError: UV_ENOENT if there is no file) if it cannot
 * be read; RESUME_EINVAL if it is damaged or describes a different torrent
 * (another info hash, piece count or file count).
 */
int32_t resumeLoad(parserContext *context, const char *path,
                   const uint8_t infoHash[PARSER_SHA1_LENGTH], uint32_t pieceCount,
                   uint32_t fileCount, resumeData *data, int32_t *systemError);

/* Write *data to 'path' (through "<path>.tmp"). */
int32_t resumeSave(parserContext *context, const char *path, const resumeData *data,
                   int32_t *systemError);

/*
 * resumeSave() in three steps, for saving from a thread-pool thread without
 * allocating there: resumeEncode() (allocates; the caller frees *text) on
 * the loop thread; then, anywhere, resumeSeal() puts the files' stamps
 * into the encoded file and its checksum, and resumeWrite() writes it
 * through 'temporary' (conventionally "<path>.tmp").
 */
int32_t resumeEncode(parserContext *context, const resumeData *data, uint8_t **text,
                     uint64_t *size);
void resumeSeal(uint8_t *text, uint64_t size, uint32_t fileCount, const uint64_t *sizes,
                const int64_t *times);
int32_t resumeWrite(const char *path, const char *temporary, const uint8_t *text, uint64_t size,
                    int32_t *systemError);

/* Free what resumeLoad() allocated (and anything else the caller put in
 * *data with the same allocator), and clear it. */
void resumeFree(parserContext *context, resumeData *data);

#ifdef __cplusplus
}
#endif

#endif /* RESUME_H */
