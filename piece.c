/*
 * piece.c - Assembling one piece from 16 KiB blocks, and verifying it.
 *
 * Block i covers bytes [i * 16 KiB, min((i + 1) * 16 KiB, length)), so only
 * the last block of a piece can be short. A received block must match one
 * of these exactly; anything else is rejected rather than patched in.
 */
#include <string.h>

#include "piece.h"

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

/* Length of block 'block'. */
static uint32_t blockLength(const pieceAssembly *piece, uint32_t block)
{
    uint32_t begin = block * PIECE_BLOCK_LENGTH;

    return (piece->length - begin < PIECE_BLOCK_LENGTH) ? piece->length - begin
                                                        : PIECE_BLOCK_LENGTH;
}

int32_t pieceBegin(pieceAssembly *piece, parserContext *context,
                   const parserTorrent *torrent, uint32_t index)
{
    uint64_t start;
    uint64_t length;

    if (piece == NULL) {
        return PIECE_EINVAL;
    }
    memset(piece, 0, sizeof(*piece));
    if (context == NULL || torrent == NULL || context->magic != PARSER_CONTEXT_MAGIC ||
        index >= torrent->pieceCount) {
        return PIECE_EINVAL;
    }
    start = (uint64_t)index * torrent->pieceLength;
    length = torrent->totalLength - start;
    if (length > torrent->pieceLength) {
        length = torrent->pieceLength;
    }
    if (length == 0 || length > UINT32_MAX - PIECE_BLOCK_LENGTH) {
        return PIECE_EINVAL;
    }

    piece->context = context;
    piece->index = index;
    piece->length = (uint32_t)length;
    piece->blockCount = (piece->length + PIECE_BLOCK_LENGTH - 1u) / PIECE_BLOCK_LENGTH;
    memcpy(piece->expectedHash, torrent->pieces + (uint64_t)index * PARSER_SHA1_LENGTH,
           PARSER_SHA1_LENGTH);

    piece->data = allocate(context, piece->length);
    piece->blocks = allocate(context, piece->blockCount);
    if (piece->data == NULL || piece->blocks == NULL) {
        pieceEnd(piece);
        return PIECE_ENOMEM;
    }
    return PIECE_OK;
}

int32_t pieceNextRequest(pieceAssembly *piece, messageFrame *request)
{
    uint32_t block;

    if (piece == NULL || request == NULL || piece->blocks == NULL) {
        return PIECE_EINVAL;
    }
    for (block = 0; block < piece->blockCount; block++) {
        if (piece->blocks[block] == PIECE_BLOCK_MISSING) {
            memset(request, 0, sizeof(*request));
            request->id = MESSAGE_REQUEST;
            request->index = piece->index;
            request->begin = block * PIECE_BLOCK_LENGTH;
            request->length = blockLength(piece, block);
            piece->blocks[block] = PIECE_BLOCK_REQUESTED;
            piece->requestedCount++;
            return PIECE_OK;
        }
    }
    return PIECE_NONE;
}

int32_t pieceReceive(pieceAssembly *piece, const messageFrame *message)
{
    uint32_t block;

    if (piece == NULL || message == NULL || piece->blocks == NULL ||
        message->id != MESSAGE_PIECE) {
        return PIECE_EINVAL;
    }
    if (message->index != piece->index || message->begin % PIECE_BLOCK_LENGTH != 0 ||
        message->begin >= piece->length) {
        return PIECE_EUNEXPECTED;
    }
    block = message->begin / PIECE_BLOCK_LENGTH;
    if (message->payloadLength != blockLength(piece, block)) {
        return PIECE_EUNEXPECTED;
    }
    if (piece->blocks[block] == PIECE_BLOCK_RECEIVED) {
        return PIECE_DUPLICATE;
    }
    if (piece->blocks[block] != PIECE_BLOCK_REQUESTED) {
        return PIECE_EUNEXPECTED;                   /* we never asked for it */
    }

    memcpy(piece->data + message->begin, message->payload, message->payloadLength);
    piece->blocks[block] = PIECE_BLOCK_RECEIVED;
    piece->requestedCount--;
    piece->receivedCount++;
    return (piece->receivedCount == piece->blockCount) ? PIECE_COMPLETE : PIECE_OK;
}

void pieceCancelRequests(pieceAssembly *piece)
{
    uint32_t block;

    if (piece == NULL || piece->blocks == NULL) {
        return;
    }
    for (block = 0; block < piece->blockCount; block++) {
        if (piece->blocks[block] == PIECE_BLOCK_REQUESTED) {
            piece->blocks[block] = PIECE_BLOCK_MISSING;
        }
    }
    piece->requestedCount = 0;
}

void pieceReset(pieceAssembly *piece)
{
    if (piece == NULL || piece->blocks == NULL) {
        return;
    }
    memset(piece->blocks, PIECE_BLOCK_MISSING, piece->blockCount);
    piece->requestedCount = 0;
    piece->receivedCount = 0;
}

int32_t pieceVerify(const pieceAssembly *piece)
{
    uint8_t digest[PARSER_SHA1_LENGTH];

    if (piece == NULL || piece->data == NULL || piece->receivedCount != piece->blockCount) {
        return PIECE_EINVAL;
    }
    parserSha1(piece->data, piece->length, digest);
    return (memcmp(digest, piece->expectedHash, PARSER_SHA1_LENGTH) == 0) ? PIECE_OK
                                                                          : PIECE_EHASH;
}

void pieceEnd(pieceAssembly *piece)
{
    if (piece == NULL || piece->context == NULL) {
        return;
    }
    if (piece->data != NULL) {
        piece->context->allocator.free(piece->data);
    }
    if (piece->blocks != NULL) {
        piece->context->allocator.free(piece->blocks);
    }
    piece->data = NULL;
    piece->blocks = NULL;
}
