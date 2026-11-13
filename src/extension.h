/*
 * extension.h - The BEP 10 extension protocol and BEP 11 peer exchange.
 *
 * Peers that both set the extension bit in their handshakes (reserved byte
 * 5, 0x10) may send "extended" messages: message id 20, then one byte that
 * says which extension, then the extension's data. Extension 0 is the
 * extended handshake, a bencoded dictionary in which each side lists the
 * extensions it supports and the byte it wants them sent with, e.g.
 *
 *   d1:md6:ut_pexi1ee1:pi6881e4:reqqi256e1:v15:torrentinfo 0.6e
 *
 * Peer exchange ("ut_pex") then lets connected peers tell each other about
 * the other peers they are connected to: every minute or so, a dictionary
 * of the peers "added" and "dropped" since the last message, as 6-byte
 * (IPv4) or 18-byte (IPv6) compact addresses.
 *
 * This module only encodes and decodes; the download module decides when.
 * Nothing here allocates.
 */
#ifndef EXTENSION_H
#define EXTENSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXTENSION_MESSAGE_ID        20u     /* the peer-wire message id          */
#define EXTENSION_HANDSHAKE         0u      /* extended id of the handshake      */
#define EXTENSION_PEX_ID            1u      /* the id we ask for ut_pex with     */
#define EXTENSION_CLIENT_LENGTH     64u
#define EXTENSION_MAX_PEX_PEERS     50u     /* added (or dropped) per message    */
#define EXTENSION_PEX_BUFFER        2048u   /* enough for any message we encode  */

/* PEX flags ("added.f"), one byte per added peer */
#define EXTENSION_PEX_ENCRYPTION    0x01u   /* prefers encryption                */
#define EXTENSION_PEX_SEED          0x02u   /* is a seed                         */
#define EXTENSION_PEX_UTP           0x04u   /* supports uTP                      */
#define EXTENSION_PEX_HOLEPUNCH     0x08u
#define EXTENSION_PEX_CONNECTABLE   0x10u   /* we reached it (it accepts)        */

#define EXTENSION_OK                0
#define EXTENSION_EINVAL            (-3)    /* bad argument or output too small  */
#define EXTENSION_EPROTO            (-8)    /* malformed message                 */

/* The extended handshake: what we send, or what a peer told us. */
typedef struct extensionHandshake {
    uint8_t     pexId;              /* ut_pex message id; 0: not supported   */
    uint16_t    port;               /* "p": the port it accepts on; 0 unknown */
    uint32_t    requestQueue;       /* "reqq": requests it queues; 0 unknown */
    char        client[EXTENSION_CLIENT_LENGTH];    /* "v", printable ASCII  */
} extensionHandshake;

/* A peer address in a PEX message. */
typedef struct extensionPeer {
    uint8_t     family;             /* 4 or 6                                */
    uint8_t     address[16];        /* network byte order; IPv4 in the first 4 */
    uint16_t    port;               /* host byte order                       */
    uint8_t     flags;              /* EXTENSION_PEX_* (added peers only)    */
} extensionPeer;

/*
 * Encode a handshake's dictionary (without the leading id bytes) into
 * 'out'. *length receives its size.
 */
int32_t extensionEncodeHandshake(const extensionHandshake *handshake, uint8_t *out, size_t size,
                                 size_t *length);

/* Decode a peer's handshake dictionary. Unknown keys are ignored. */
int32_t extensionDecodeHandshake(const uint8_t *data, size_t length,
                                 extensionHandshake *handshake);

/*
 * Encode a PEX dictionary: up to EXTENSION_MAX_PEX_PEERS of each list
 * (more are left out). An EXTENSION_PEX_BUFFER-byte buffer is always enough.
 */
int32_t extensionEncodePex(const extensionPeer *added, uint32_t addedCount,
                           const extensionPeer *dropped, uint32_t droppedCount, uint8_t *out,
                           size_t size, size_t *length);

/*
 * Decode a PEX dictionary, calling 'callback' for each peer (at most
 * EXTENSION_MAX_PEX_PEERS added and as many dropped): 'added' is 1 for
 * peers it connected to, 0 for ones it dropped.
 */
typedef void (*extensionPeerCallback)(void *privateArg, const extensionPeer *peer, int added);
int32_t extensionDecodePex(const uint8_t *data, size_t length, extensionPeerCallback callback,
                           void *privateArg);

#ifdef __cplusplus
}
#endif

#endif /* EXTENSION_H */
