/*
 * message.h - BitTorrent peer wire messages (BEP 3): encoding and decoding.
 *
 * Every message after the handshake is framed as
 *
 *     <4-byte big-endian length> <1-byte id> <payload>
 *
 * with a length of 0 meaning keep-alive. These functions are pure: they
 * neither allocate nor do I/O, so they can be used and tested on their own.
 * peer.c uses them for live connections.
 *
 *   id  name            payload
 *    0  choke           -
 *    1  unchoke         -
 *    2  interested      -
 *    3  not interested  -
 *    4  have            piece index (4)
 *    5  bitfield        one bit per piece, first piece = high bit of byte 0
 *    6  request         index (4), begin (4), length (4)
 *    7  piece           index (4), begin (4), block data
 *    8  cancel          index (4), begin (4), length (4)
 *    9  port            DHT port (2)
 *
 * Other ids (e.g. the fast extension's 13-17 and the extension protocol's
 * 20) are decoded generically: id plus raw payload.
 */
#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define MESSAGE_CHOKE               0u
#define MESSAGE_UNCHOKE             1u
#define MESSAGE_INTERESTED          2u
#define MESSAGE_NOT_INTERESTED      3u
#define MESSAGE_HAVE                4u
#define MESSAGE_BITFIELD            5u
#define MESSAGE_REQUEST             6u
#define MESSAGE_PIECE               7u
#define MESSAGE_CANCEL              8u
#define MESSAGE_PORT                9u
#define MESSAGE_KEEP_ALIVE          256u    /* not a wire id: a zero-length frame */

#define MESSAGE_PREFIX_LENGTH       4u      /* the length field                  */
#define MESSAGE_BLOCK_LENGTH        16384u  /* the usual request size (16 KiB)   */
#define MESSAGE_MAX_BLOCK_LENGTH    131072u /* largest request accepted (128 KiB) */

/* Return codes */
#define MESSAGE_OK                  0       /* a whole message was decoded       */
#define MESSAGE_INCOMPLETE          1       /* need more bytes                   */
#define MESSAGE_EINVAL              (-3)    /* bad argument / output too small   */
#define MESSAGE_EPROTO              (-8)    /* malformed message                 */
#define MESSAGE_ETOOLARGE           (-14)   /* length above the allowed maximum  */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* One decoded message, or one to encode. Fields not used by 'id' are 0. */
typedef struct messageFrame {
    uint32_t        id;             /* MESSAGE_* (0-255 on the wire, or
                                       MESSAGE_KEEP_ALIVE)                    */
    uint32_t        index;          /* have, request, piece, cancel          */
    uint32_t        begin;          /* request, piece, cancel: byte offset   */
    uint32_t        length;         /* request, cancel: block length         */
    uint16_t        port;           /* port                                  */
    const uint8_t  *payload;        /* bitfield bits, piece data, or the raw
                                       payload of any other id; when decoding
                                       it points into the input buffer       */
    uint32_t        payloadLength;
} messageFrame;

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Decode the message at the start of 'data'. On MESSAGE_OK, *frame holds it
 * and *consumed is its total size including the length prefix. Returns
 * MESSAGE_INCOMPLETE if more bytes are needed (if at least 4 bytes are
 * present, *consumed is then set to the size the whole message will have),
 * MESSAGE_ETOOLARGE if the length exceeds 'maxLength' (which excludes the
 * prefix), or MESSAGE_EPROTO if the payload has the wrong size for its id.
 */
int32_t messageDecode(const uint8_t *data, uint64_t length, uint32_t maxLength,
                      messageFrame *frame, uint32_t *consumed);

/* Size of the encoded message, including the length prefix. 0 if invalid. */
uint32_t messageEncodedLength(const messageFrame *frame);

/*
 * Encode a message, payload included, into 'out'. Returns MESSAGE_EINVAL if
 * 'capacity' is too small or the frame is invalid; *written gets the size.
 */
int32_t messageEncode(const messageFrame *frame, uint8_t *out, uint32_t capacity,
                      uint32_t *written);

/* Bitfield helpers. A bitfield for n pieces is (n + 7) / 8 bytes. */
uint32_t messageBitfieldBytes(uint32_t pieceCount);
uint8_t  messageBitfieldGet(const uint8_t *bitfield, uint32_t index);
void     messageBitfieldSet(uint8_t *bitfield, uint32_t index);
uint32_t messageBitfieldCount(const uint8_t *bitfield, uint32_t pieceCount);

/* Right size for pieceCount, and the spare bits after the last piece clear. */
uint8_t  messageBitfieldValid(const uint8_t *bitfield, uint32_t length, uint32_t pieceCount);

/* Name of a message id ("choke", "piece", "keep-alive", "unknown"). Never NULL. */
const char *messageName(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_H */
