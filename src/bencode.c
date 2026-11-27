/*
 * bencode.c - the bencode writer shared by the DHT, extension messages,
 * magnet links, the torrent creator and the daemon protocol.
 */
#include <string.h>

#include "bencode.h"

void bencodeWriterInitFixed(bencodeWriter *writer, uint8_t *buffer, uint64_t size)
{
    memset(writer, 0, sizeof(*writer));
    writer->data = buffer;
    writer->capacity = (buffer != NULL) ? size : 0;
}

void bencodeWriterInit(bencodeWriter *writer, const parserAllocator *allocator)
{
    memset(writer, 0, sizeof(*writer));
    writer->allocator = allocator;
}

void bencodeWriterFree(bencodeWriter *writer)
{
    if (writer->allocator != NULL && writer->data != NULL) {
        writer->allocator->free(writer->data);
    }
    if (writer->allocator != NULL) {
        writer->data = NULL;
        writer->capacity = 0;
    }
    writer->length = 0;
}

int32_t bencodeWriterTake(bencodeWriter *writer, uint8_t **data, uint64_t *length)
{
    int32_t error = writer->error;

    if (error != BENCODE_OK || writer->allocator == NULL) {
        bencodeWriterFree(writer);
        *data = NULL;
        *length = 0;
        return (error != BENCODE_OK) ? error : PARSER_ERROR_INVALID_ARGUMENT;
    }
    *data = writer->data;
    *length = writer->length;
    writer->data = NULL;
    writer->length = 0;
    writer->capacity = 0;
    return BENCODE_OK;
}

/* Make room for extra more bytes; sets writer->error if it cannot. */
static int reserve(bencodeWriter *writer, uint64_t extra)
{
    uint64_t needed;
    uint64_t capacity;
    uint8_t *grown;

    if (writer->error != BENCODE_OK) {
        return 0;
    }
    if (extra > UINT64_MAX - writer->length) {
        writer->error = PARSER_ERROR_OVERFLOW;
        return 0;
    }
    needed = writer->length + extra;
    if (needed <= writer->capacity) {
        return 1;
    }
    if (writer->allocator == NULL) {
        writer->error = BENCODE_ERROR_NO_SPACE;
        return 0;
    }
    capacity = (writer->capacity != 0) ? writer->capacity : BENCODE_MIN_CAPACITY;
    while (capacity < needed) {
        if (capacity > UINT64_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > (uint64_t)(SIZE_MAX / 2u)) {
        writer->error = PARSER_ERROR_NO_MEMORY;
        return 0;
    }
    grown = writer->allocator->realloc(writer->data, (size_t)capacity);
    if (grown == NULL) {
        writer->error = PARSER_ERROR_NO_MEMORY;     /* the old buffer is still valid */
        return 0;
    }
    writer->data = grown;
    writer->capacity = capacity;
    return 1;
}

void bencodeRaw(bencodeWriter *writer, const void *data, uint64_t length)
{
    if (!reserve(writer, length)) {
        return;
    }
    if (length > 0) {
        memcpy(writer->data + writer->length, data, (size_t)length);
    }
    writer->length += length;
}

static void putByte(bencodeWriter *writer, uint8_t value)
{
    bencodeRaw(writer, &value, 1u);
}

static void putDecimal(bencodeWriter *writer, uint64_t value)
{
    uint8_t  digits[20];
    uint32_t count = 0;

    do {
        digits[sizeof(digits) - 1u - count] = (uint8_t)('0' + (value % 10u));
        value /= 10u;
        count++;
    } while (value != 0);
    bencodeRaw(writer, digits + sizeof(digits) - count, count);
}

void bencodeUnsigned(bencodeWriter *writer, uint64_t value)
{
    putByte(writer, 'i');
    putDecimal(writer, value);
    putByte(writer, 'e');
}

void bencodeInteger(bencodeWriter *writer, int64_t value)
{
    putByte(writer, 'i');
    if (value < 0) {
        putByte(writer, '-');
        putDecimal(writer, (uint64_t)(-(value + 1)) + 1u);     /* safe for INT64_MIN */
    } else {
        putDecimal(writer, (uint64_t)value);
    }
    putByte(writer, 'e');
}

void bencodeStringHeader(bencodeWriter *writer, uint64_t length)
{
    putDecimal(writer, length);
    putByte(writer, ':');
}

void bencodeString(bencodeWriter *writer, const void *data, uint64_t length)
{
    bencodeStringHeader(writer, length);
    bencodeRaw(writer, data, length);
}

void bencodeText(bencodeWriter *writer, const char *text)
{
    bencodeString(writer, text, strlen(text));
}

void bencodeListBegin(bencodeWriter *writer)
{
    putByte(writer, 'l');
}

void bencodeDictBegin(bencodeWriter *writer)
{
    putByte(writer, 'd');
}

void bencodeEnd(bencodeWriter *writer)
{
    putByte(writer, 'e');
}

const char *bencodeErrorString(int32_t error)
{
    switch (error) {
    case BENCODE_OK:              return "success";
    case BENCODE_ERROR_NO_SPACE:  return "message too large for its buffer";
    case PARSER_ERROR_NO_MEMORY:  return "out of memory";
    case PARSER_ERROR_OVERFLOW:   return "message too large";
    default:                      return "bencode error";
    }
}
