/*
 * message.c - BitTorrent peer wire messages (BEP 3): encoding and decoding.
 *
 * Pure functions: no allocation, no I/O. Every length is checked against
 * the bytes available before anything is read.
 */
#include <string.h>

#include "message.h"

static uint32_t get32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

/*
 * Payload size the id requires: exact sizes return that size; ids with a
 * variable payload return -1 (any size) or -2 (at least 8 bytes: piece).
 */
static int32_t payloadRule(uint32_t id)
{
    switch (id) {
    case MESSAGE_CHOKE:
    case MESSAGE_UNCHOKE:
    case MESSAGE_INTERESTED:
    case MESSAGE_NOT_INTERESTED:
        return 0;
    case MESSAGE_HAVE:
        return 4;
    case MESSAGE_REQUEST:
    case MESSAGE_CANCEL:
        return 12;
    case MESSAGE_PORT:
        return 2;
    case MESSAGE_PIECE:
        return -2;
    default:                                        /* bitfield and any other id */
        return -1;
    }
}

int32_t messageDecode(const uint8_t *data, uint64_t length, uint32_t maxLength,
                      messageFrame *frame, uint32_t *consumed)
{
    const uint8_t *payload;
    uint32_t       frameLength;
    uint32_t       payloadLength;
    int32_t        rule;

    if (data == NULL || frame == NULL || consumed == NULL) {
        return MESSAGE_EINVAL;
    }
    memset(frame, 0, sizeof(*frame));
    *consumed = 0;
    if (length < MESSAGE_PREFIX_LENGTH) {
        return MESSAGE_INCOMPLETE;
    }
    frameLength = get32(data);
    if (frameLength > maxLength || frameLength > UINT32_MAX - MESSAGE_PREFIX_LENGTH) {
        return MESSAGE_ETOOLARGE;
    }
    *consumed = MESSAGE_PREFIX_LENGTH + frameLength;
    if (length < (uint64_t)*consumed) {
        return MESSAGE_INCOMPLETE;                  /* *consumed: size to wait for */
    }

    if (frameLength == 0) {
        frame->id = MESSAGE_KEEP_ALIVE;
        return MESSAGE_OK;
    }
    frame->id = data[MESSAGE_PREFIX_LENGTH];
    payload = data + MESSAGE_PREFIX_LENGTH + 1u;
    payloadLength = frameLength - 1u;

    rule = payloadRule(frame->id);
    if ((rule >= 0 && payloadLength != (uint32_t)rule) ||
        (rule == -2 && payloadLength < 8u)) {
        return MESSAGE_EPROTO;
    }

    switch (frame->id) {
    case MESSAGE_HAVE:
        frame->index = get32(payload);
        break;
    case MESSAGE_REQUEST:
    case MESSAGE_CANCEL:
        frame->index = get32(payload);
        frame->begin = get32(payload + 4);
        frame->length = get32(payload + 8);
        break;
    case MESSAGE_PIECE:
        frame->index = get32(payload);
        frame->begin = get32(payload + 4);
        frame->payload = payload + 8;
        frame->payloadLength = payloadLength - 8u;
        frame->length = frame->payloadLength;
        break;
    case MESSAGE_PORT:
        frame->port = (uint16_t)(((uint32_t)payload[0] << 8) | payload[1]);
        break;
    case MESSAGE_CHOKE:
    case MESSAGE_UNCHOKE:
    case MESSAGE_INTERESTED:
    case MESSAGE_NOT_INTERESTED:
        break;
    default:                                        /* bitfield, unknown ids */
        frame->payload = payload;
        frame->payloadLength = payloadLength;
        break;
    }
    return MESSAGE_OK;
}

uint32_t messageEncodedLength(const messageFrame *frame)
{
    int32_t  rule;
    uint64_t total;

    if (frame == NULL) {
        return 0;
    }
    if (frame->id == MESSAGE_KEEP_ALIVE) {
        return MESSAGE_PREFIX_LENGTH;
    }
    if (frame->id > 255u) {
        return 0;
    }
    rule = payloadRule(frame->id);
    if (rule >= 0) {
        total = (uint64_t)MESSAGE_PREFIX_LENGTH + 1u + (uint32_t)rule;
    } else {
        if (frame->payloadLength > 0 && frame->payload == NULL) {
            return 0;
        }
        total = (uint64_t)MESSAGE_PREFIX_LENGTH + 1u + frame->payloadLength +
                ((rule == -2) ? 8u : 0u);
    }
    return (total > UINT32_MAX) ? 0 : (uint32_t)total;
}

int32_t messageEncode(const messageFrame *frame, uint8_t *out, uint32_t capacity,
                      uint32_t *written)
{
    uint32_t total = messageEncodedLength(frame);
    uint8_t *payload;

    if (written != NULL) {
        *written = 0;
    }
    if (total == 0 || out == NULL || written == NULL || capacity < total) {
        return MESSAGE_EINVAL;
    }
    put32(out, total - MESSAGE_PREFIX_LENGTH);
    if (frame->id == MESSAGE_KEEP_ALIVE) {
        *written = total;
        return MESSAGE_OK;
    }
    out[MESSAGE_PREFIX_LENGTH] = (uint8_t)frame->id;
    payload = out + MESSAGE_PREFIX_LENGTH + 1u;

    switch (frame->id) {
    case MESSAGE_HAVE:
        put32(payload, frame->index);
        break;
    case MESSAGE_REQUEST:
    case MESSAGE_CANCEL:
        put32(payload, frame->index);
        put32(payload + 4, frame->begin);
        put32(payload + 8, frame->length);
        break;
    case MESSAGE_PIECE:
        put32(payload, frame->index);
        put32(payload + 4, frame->begin);
        if (frame->payloadLength > 0) {
            memcpy(payload + 8, frame->payload, frame->payloadLength);
        }
        break;
    case MESSAGE_PORT:
        payload[0] = (uint8_t)(frame->port >> 8);
        payload[1] = (uint8_t)frame->port;
        break;
    case MESSAGE_CHOKE:
    case MESSAGE_UNCHOKE:
    case MESSAGE_INTERESTED:
    case MESSAGE_NOT_INTERESTED:
        break;
    default:                                        /* bitfield, other ids */
        if (frame->payloadLength > 0) {
            memcpy(payload, frame->payload, frame->payloadLength);
        }
        break;
    }
    *written = total;
    return MESSAGE_OK;
}

uint32_t messageBitfieldBytes(uint32_t pieceCount)
{
    return pieceCount / 8u + ((pieceCount % 8u) != 0 ? 1u : 0u);
}

uint8_t messageBitfieldGet(const uint8_t *bitfield, uint32_t index)
{
    return (uint8_t)(((uint32_t)bitfield[index / 8u] >> (7u - index % 8u)) & 1u);
}

void messageBitfieldSet(uint8_t *bitfield, uint32_t index)
{
    bitfield[index / 8u] = (uint8_t)(bitfield[index / 8u] | (0x80u >> (index % 8u)));
}

uint32_t messageBitfieldCount(const uint8_t *bitfield, uint32_t pieceCount)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < pieceCount; i++) {
        count += messageBitfieldGet(bitfield, i);
    }
    return count;
}

uint8_t messageBitfieldValid(const uint8_t *bitfield, uint32_t length, uint32_t pieceCount)
{
    uint32_t spare;

    if (length != messageBitfieldBytes(pieceCount)) {
        return 0;
    }
    spare = length * 8u - pieceCount;               /* 0-7 unused low bits */
    if (spare > 0 && ((uint32_t)bitfield[length - 1u] & ((1u << spare) - 1u)) != 0u) {
        return 0;
    }
    return 1;
}

const char *messageName(uint32_t id)
{
    switch (id) {
    case MESSAGE_CHOKE:          return "choke";
    case MESSAGE_UNCHOKE:        return "unchoke";
    case MESSAGE_INTERESTED:     return "interested";
    case MESSAGE_NOT_INTERESTED: return "not interested";
    case MESSAGE_HAVE:           return "have";
    case MESSAGE_BITFIELD:       return "bitfield";
    case MESSAGE_REQUEST:        return "request";
    case MESSAGE_PIECE:          return "piece";
    case MESSAGE_CANCEL:         return "cancel";
    case MESSAGE_PORT:           return "port";
    case MESSAGE_KEEP_ALIVE:     return "keep-alive";
    default:                     return "unknown";
    }
}
