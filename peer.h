/*
 * peer.h - BitTorrent peer connections using libuv (BEP 3 peer wire protocol).
 *
 * Step 1: connect to a peer and exchange handshakes.
 *
 *   handshake = <19> "BitTorrent protocol" <8 reserved bytes>
 *               <20-byte info hash> <20-byte peer id>          (68 bytes)
 *
 * peerConnect() opens a TCP connection, sends our handshake and reads the
 * peer's. The callback runs once with the outcome. After a successful
 * handshake the connection stays open, ready for the message exchange that
 * comes next; any bytes the peer sent straight after its handshake (often
 * its bitfield) are kept for that.
 *
 * Every connection handle stays valid until peerClose(), whatever the
 * outcome, in the same way as tracker ids.
 *
 * Memory comes from the allocator in the parserContext given to peerInit().
 * All functions must be called from the thread that runs the libuv loop.
 *
 * Usage:
 *     peerInit(loop, &context);
 *     peerConnect(address, infoHash, myPeerId, 10, onHandshake, me, &connection);
 *     uv_run(loop, UV_RUN_DEFAULT);
 *
 *     static void onHandshake(void *privateArg, peerConnection *connection,
 *                             const peerHandshakeResult *result) {
 *         if (result->status == PEER_OK) { ... result->peerId ... }
 *         peerClose(connection);           // or keep it for messages
 *     }
 */
#ifndef PEER_H
#define PEER_H

#include <stdint.h>
#include <uv.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define PEER_ID_LENGTH              20u
#define PEER_RESERVED_LENGTH        8u
#define PEER_HANDSHAKE_LENGTH       68u
#define PEER_ERROR_MESSAGE_LENGTH   160u

/* Reserved-bit flags in peerHandshakeResult.extensions */
#define PEER_EXTENSION_PROTOCOL     0x01u   /* BEP 10 extension protocol (reserved[5] & 0x10) */
#define PEER_EXTENSION_FAST         0x02u   /* BEP 6 fast extension      (reserved[7] & 0x04) */
#define PEER_EXTENSION_DHT          0x04u   /* BEP 5 DHT port message    (reserved[7] & 0x01) */

/* Status codes (shared values match the TRACKER_* codes) */
#define PEER_OK                     0       /* Handshake completed           */
#define PEER_ERROR                  (-1)    /* Network error: see message    */
#define PEER_EINVAL                 (-3)    /* Invalid argument              */
#define PEER_ENOMEM                 (-4)    /* Allocator returned NULL       */
#define PEER_ETIMEDOUT              (-5)    /* No handshake within timeout   */
#define PEER_EPROTO                 (-8)    /* Not the BitTorrent protocol   */
#define PEER_ENOTINIT               (-9)    /* peerInit() not called         */
#define PEER_EHASH                  (-11)   /* Peer is not serving this torrent */
#define PEER_ESELF                  (-12)   /* Connected to ourselves        */
#define PEER_ECLOSED                (-13)   /* Peer closed the connection    */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* Opaque connection handle. */
typedef struct peerConnection peerConnection;

/* The outcome of a handshake. Valid only during the callback. */
typedef struct peerHandshakeResult {
    int32_t         status;                         /* PEER_OK or a PEER_E* code  */
    uint8_t         peerId[PEER_ID_LENGTH];         /* the peer's id (if it got
                                                       that far)                 */
    uint8_t         reserved[PEER_RESERVED_LENGTH]; /* the peer's reserved bytes  */
    uint32_t        extensions;                     /* PEER_EXTENSION_* decoded
                                                       from 'reserved'           */
    uint32_t        extraBytes;                     /* bytes received after the
                                                       handshake, kept for later */
    const char     *errorMessage;                   /* NULL on success            */
} peerHandshakeResult;

/* Called once per connection, unless peerClose() is called first. */
typedef void (*peerCallback)(void *privateArg, peerConnection *connection,
                             const peerHandshakeResult *result);

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Set up the module. Calls are counted, so several users may each call
 * peerInit() and peerShutdown(). Returns PEER_EINVAL if a later call passes
 * a different loop or context.
 */
int32_t peerInit(uv_loop_t *loop, parserContext *context);

/*
 * Connect to 'address' (IPv4 or IPv6, port included) and exchange
 * handshakes for 'infoHash', identifying ourselves as 'peerId'.
 *
 * On PEER_OK the attempt is under way, *connection receives its handle, and
 * the callback will run once from the libuv loop. Any other return value
 * means nothing was started and no handle was created.
 *
 * timeoutSeconds covers the connection and the handshake; 0 means no
 * timeout. The address, info hash and peer id are copied.
 */
int32_t peerConnect(const struct sockaddr *address,
                    const uint8_t infoHash[PARSER_SHA1_LENGTH],
                    const uint8_t peerId[PEER_ID_LENGTH],
                    uint32_t timeoutSeconds, peerCallback callback,
                    void *privateArg, peerConnection **connection);

/*
 * Close a connection and release its handle. If the handshake is still in
 * progress it is abandoned and the callback will not run. Safe to call from
 * the callback.
 */
void peerClose(peerConnection *connection);

/*
 * Release one peerInit(). The last call closes every connection (pending
 * callbacks do not run). Keep running the loop until uv_run() returns so
 * that all memory is released.
 */
void peerShutdown(void);

/* Human-readable description of a PEER_* status. Never NULL. */
const char *peerStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* PEER_H */
