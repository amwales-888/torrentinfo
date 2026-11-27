/*
 * bencode.h - one bencode writer for the whole library.
 *
 * A writer either fills a caller's fixed buffer (DHT packets, extension
 * messages) or grows a buffer through a parserAllocator (torrent files,
 * the daemon protocol). Errors are sticky: after the first failure every
 * further call does nothing, so a message can be written in one go and
 * the error checked once at the end.
 *
 *     bencodeWriter w;
 *     bencodeWriterInitFixed(&w, packet, sizeof(packet));
 *     bencodeDictBegin(&w);
 *       bencodeText(&w, "id");  bencodeString(&w, nodeId, 20);
 *       bencodeText(&w, "port"); bencodeInteger(&w, 6881);
 *     bencodeEnd(&w);
 *     if (w.error != BENCODE_OK) { ...too big... }
 *     send(packet, w.length);
 *
 * Reading bencode is done with parserCursor (parser.h).
 */
#ifndef BENCODE_H
#define BENCODE_H

#include <stdint.h>
#include "parser.h"

#define BENCODE_OK                 0
#define BENCODE_ERROR_NO_SPACE    (-40)     /* fixed buffer too small          */
/* A growable writer can also fail with PARSER_ERROR_NO_MEMORY or
 * PARSER_ERROR_OVERFLOW. */

#define BENCODE_MIN_CAPACITY      256u      /* first allocation of a growable writer */

typedef struct bencodeWriter {
    uint8_t               *data;
    uint64_t               length;          /* bytes written                       */
    uint64_t               capacity;
    const parserAllocator *allocator;       /* NULL: fixed buffer, never grows     */
    int32_t                error;           /* BENCODE_OK or the first failure     */
} bencodeWriter;

/* Write into buffer[0..size); fails with BENCODE_ERROR_NO_SPACE if full. */
void bencodeWriterInitFixed(bencodeWriter *writer, uint8_t *buffer, uint64_t size);

/* Grow a buffer with allocator->realloc. The allocator must stay valid while
 * the writer is in use. Release the buffer with bencodeWriterFree, or take it
 * with bencodeWriterTake. */
void bencodeWriterInit(bencodeWriter *writer, const parserAllocator *allocator);

/* Hand the buffer to the caller (who frees it with allocator->free) and reset
 * the writer. Returns writer->error; on failure the buffer is freed and
 * *data is NULL. */
int32_t bencodeWriterTake(bencodeWriter *writer, uint8_t **data, uint64_t *length);

/* Free a growable writer's buffer (nothing for a fixed one). */
void bencodeWriterFree(bencodeWriter *writer);

/* Raw bytes, copied as they are (for pre-encoded values). */
void bencodeRaw(bencodeWriter *writer, const void *data, uint64_t length);

/* i<value>e */
void bencodeInteger(bencodeWriter *writer, int64_t value);
void bencodeUnsigned(bencodeWriter *writer, uint64_t value);

/* <length>:<bytes> */
void bencodeString(bencodeWriter *writer, const void *data, uint64_t length);
void bencodeText(bencodeWriter *writer, const char *text);    /* NUL-terminated */

/* Only the "<length>:" prefix, for a string the caller then writes in pieces
 * with bencodeRaw (exactly length bytes). */
void bencodeStringHeader(bencodeWriter *writer, uint64_t length);

/* l ... e and d ... e; dictionary keys are written with bencodeText or
 * bencodeString, in sorted (raw byte) order. */
void bencodeListBegin(bencodeWriter *writer);
void bencodeDictBegin(bencodeWriter *writer);
void bencodeEnd(bencodeWriter *writer);

const char *bencodeErrorString(int32_t error);

#endif /* BENCODE_H */
