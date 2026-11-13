/*
 * piece.h - Assembling one piece from 16 KiB blocks, and verifying it.
 *
 * Peers send data in blocks: a request asks for <piece index, offset,
 * length>, and a piece message returns that block. This module keeps the
 * bookkeeping for one piece:
 *
 *   pieceBegin()          allocate the piece; every block "missing"
 *   pieceNextRequest()    the next missing block, as a request message;
 *                         marks it "requested"
 *   pieceReceive()        check a piece message and store its block
 *   pieceCancelRequests() "requested" blocks back to "missing" (e.g. when
 *                         the peer chokes us, which discards our requests)
 *   pieceVerify()         SHA-1 of the data against the torrent's hash
 *   pieceTakeData()       hand the data to the caller (e.g. for storage)
 *   pieceEnd()            release the memory
 *
 * No I/O: the caller sends the requests and passes in the replies.
 * Memory comes from the allocator in the parserContext.
 */
#ifndef PIECE_H
#define PIECE_H

#include <stdint.h>

#include "message.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define PIECE_BLOCK_LENGTH      16384u  /* what we request (the de facto standard) */

/* Return codes */
#define PIECE_OK                0       /* done                                   */
#define PIECE_COMPLETE          1       /* pieceReceive(): that was the last block */
#define PIECE_NONE              2       /* pieceNextRequest(): nothing to request  */
#define PIECE_DUPLICATE         3       /* pieceReceive(): block already held      */
#define PIECE_EINVAL            (-3)    /* bad argument                           */
#define PIECE_ENOMEM            (-4)    /* allocator returned NULL                */
#define PIECE_EHASH             (-11)   /* data does not match the torrent's hash */
#define PIECE_EUNEXPECTED       (-15)   /* block not for this piece, not on a block
                                           boundary, wrong length, or not requested */

/* Block states */
#define PIECE_BLOCK_MISSING     0u
#define PIECE_BLOCK_REQUESTED   1u
#define PIECE_BLOCK_RECEIVED    2u

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* One piece being assembled. Allocated by the caller; treat as read-only. */
typedef struct pieceAssembly {
    parserContext  *context;
    uint32_t        index;          /* piece index                          */
    uint32_t        length;         /* bytes (the last piece may be shorter) */
    uint8_t         expectedHash[PARSER_SHA1_LENGTH];
    uint8_t        *data;           /* 'length' bytes                       */
    uint8_t        *blocks;         /* PIECE_BLOCK_* per block              */
    uint32_t        blockCount;
    uint32_t        receivedCount;
    uint32_t        requestedCount; /* requested and not yet received       */
} pieceAssembly;

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/* Start assembling piece 'index' of 'torrent'. */
int32_t pieceBegin(pieceAssembly *piece, parserContext *context,
                   const parserTorrent *torrent, uint32_t index);

/*
 * Fill in 'request' (MESSAGE_REQUEST) for the next missing block and mark
 * it requested. Returns PIECE_NONE when no block is missing.
 */
int32_t pieceNextRequest(pieceAssembly *piece, messageFrame *request);

/*
 * Store the block carried by a MESSAGE_PIECE. Returns PIECE_OK, or
 * PIECE_COMPLETE when every block has arrived, PIECE_DUPLICATE for a block
 * already held (ignored), or PIECE_EUNEXPECTED for a block that does not
 * fit this piece or was never requested.
 */
int32_t pieceReceive(pieceAssembly *piece, const messageFrame *message);

/* Mark every requested-but-not-received block as missing again. */
void pieceCancelRequests(pieceAssembly *piece);

/* Mark one requested block (by byte offset) as missing again. */
void pieceCancelBlock(pieceAssembly *piece, uint32_t begin);

/* Mark every block missing (e.g. after a failed verification). */
void pieceReset(pieceAssembly *piece);

/* PIECE_OK if the complete data matches the torrent's hash, else PIECE_EHASH. */
int32_t pieceVerify(const pieceAssembly *piece);

/*
 * Take the piece's data buffer: the caller becomes its owner (free it with
 * the context's allocator, or pass it to storageWritePiece()). Afterwards
 * the piece has no data; call pieceEnd() to release the rest.
 */
uint8_t *pieceTakeData(pieceAssembly *piece);

/* Release the piece's memory. Safe to call more than once. */
void pieceEnd(pieceAssembly *piece);

#ifdef __cplusplus
}
#endif

#endif /* PIECE_H */
