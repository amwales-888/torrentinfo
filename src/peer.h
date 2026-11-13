/*
 * peer.h - BitTorrent peer connections using libuv (BEP 3 peer wire protocol).
 *
 * A connection goes through:
 *
 *   connect -> handshake -> messages ... -> closed
 *
 * peerConnect() opens a TCP connection and exchanges 68-byte handshakes:
 *
 *   <19> "BitTorrent protocol" <8 reserved bytes> <info hash> <peer id>
 *
 * After a successful handshake the connection reads messages (message.h)
 * until it is closed. Each message is checked against the protocol (a
 * bitfield only as the first message and of the right size, piece indexes
 * in range, ...), applied to the connection's view of the peer (choke and
 * interest flags, which pieces it has) and then passed to the onMessage
 * callback. A protocol violation closes the connection.
 *
 * Keep-alives are sent automatically after 90 seconds without sending
 * anything, and a peer that sends nothing for 3 minutes is dropped.
 *
 * Every connection handle stays valid until peerClose(), whatever happens
 * to the network connection.
 *
 * Memory comes from the allocator in the parserContext given to peerInit().
 * All functions must be called from the thread that runs the libuv loop.
 *
 * On Linux and other POSIX systems the program should ignore SIGPIPE
 * (signal(SIGPIPE, SIG_IGN)): writing to a connection the peer has just
 * closed raises it, and by default it ends the process.
 *
 * Usage:
 *     peerConnectOptions options = { 0 };
 *     memcpy(options.infoHash, torrent.infoHash, 20);
 *     memcpy(options.peerId, myPeerId, 20);
 *     options.pieceCount = torrent.pieceCount;
 *     options.timeoutSeconds = 10;
 *     peerCallbacks callbacks = { onHandshake, onMessage, onClosed };
 *
 *     peerInit(loop, &context);
 *     peerConnect(address, &options, &callbacks, me, &connection);
 *     uv_run(loop, UV_RUN_DEFAULT);
 *
 *     static void onHandshake(void *me, peerConnection *c, const peerHandshakeResult *r) {
 *         if (r->status == PEER_OK) {
 *             messageFrame interested = { MESSAGE_INTERESTED };
 *             peerSend(c, &interested);
 *         } else {
 *             peerClose(c);
 *         }
 *     }
 */
#ifndef PEER_H
#define PEER_H

#include <stdint.h>
#include <uv.h>

#include "message.h"
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
/* Timing; may be overridden at compile time (the tests use short values). */
#ifndef PEER_KEEP_ALIVE_SECONDS
#define PEER_KEEP_ALIVE_SECONDS     90u     /* send a keep-alive after this quiet */
#endif
#ifndef PEER_IDLE_SECONDS
#define PEER_IDLE_SECONDS           180u    /* drop a peer silent for this long   */
#endif

/* Reserved-bit flags in peerHandshakeResult.extensions */
#define PEER_EXTENSION_PROTOCOL     0x01u   /* BEP 10 extension protocol (reserved[5] & 0x10) */
#define PEER_EXTENSION_FAST         0x02u   /* BEP 6 fast extension      (reserved[7] & 0x04) */
#define PEER_EXTENSION_DHT          0x04u   /* BEP 5 DHT port message    (reserved[7] & 0x01) */

/* Status codes (shared values match the TRACKER_* codes) */
#define PEER_OK                     0       /* Success                         */
#define PEER_ERROR                  (-1)    /* Network error: see message      */
#define PEER_EINVAL                 (-3)    /* Invalid argument or bad timing  */
#define PEER_ENOMEM                 (-4)    /* Allocator returned NULL         */
#define PEER_ETIMEDOUT              (-5)    /* No handshake in time, or peer idle */
#define PEER_EPROTO                 (-8)    /* Peer broke the protocol         */
#define PEER_ENOTINIT               (-9)    /* peerInit() not called           */
#define PEER_EHASH                  (-11)   /* Peer is not serving this torrent */
#define PEER_ESELF                  (-12)   /* Connected to ourselves          */
#define PEER_ECLOSED                (-13)   /* The connection has closed       */

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

/* Opaque connection handle. */
typedef struct peerConnection peerConnection;

/* What to connect for. Copied by peerConnect(). */
typedef struct peerConnectOptions {
    uint8_t     infoHash[PARSER_SHA1_LENGTH];
    uint8_t     peerId[PEER_ID_LENGTH];     /* our peer id                       */
    uint32_t    pieceCount;                 /* the torrent's piece count: sizes
                                               the peer's bitfield and bounds
                                               piece indexes                    */
    uint32_t    timeoutSeconds;             /* for connecting and the handshake;
                                               0 means no limit                 */
} peerConnectOptions;

/* The outcome of a handshake. Valid only during the callback. */
typedef struct peerHandshakeResult {
    int32_t         status;                         /* PEER_OK or a PEER_E* code  */
    uint8_t         peerId[PEER_ID_LENGTH];         /* the peer's id (if it got
                                                       that far)                 */
    uint8_t         reserved[PEER_RESERVED_LENGTH]; /* the peer's reserved bytes  */
    uint32_t        extensions;                     /* PEER_EXTENSION_* decoded
                                                       from 'reserved'           */
    const char     *errorMessage;                   /* NULL on success            */
} peerHandshakeResult;

/* Our view of a connected peer, from peerGetState(). */
typedef struct peerRemoteState {
    uint8_t         connected;      /* 1 between the handshake and closing   */
    uint8_t         amChoking;      /* we are choking the peer (initially 1) */
    uint8_t         amInterested;   /* we told the peer we are interested    */
    uint8_t         peerChoking;    /* the peer is choking us (initially 1)  */
    uint8_t         peerInterested; /* the peer is interested in us          */
    uint32_t        pieceCount;
    uint32_t        haveCount;      /* pieces the peer has told us it has    */
    const uint8_t  *have;           /* those pieces as a bitfield
                                       (messageBitfieldGet); valid until the
                                       next callback or peerClose()         */
    uint32_t        messagesReceived;   /* excluding keep-alives            */
    uint64_t        bytesReceived;      /* after the handshake              */
    uint64_t        bytesSent;
    uint8_t         peerId[PEER_ID_LENGTH];
    uint32_t        extensions;     /* PEER_EXTENSION_*                      */
} peerRemoteState;

/* Called once with the handshake's outcome. */
typedef void (*peerHandshakeCallback)(void *privateArg, peerConnection *connection,
                                      const peerHandshakeResult *result);

/*
 * Called for each message after the handshake, once it has been checked
 * and applied to the connection's state. The frame, including any payload,
 * is valid only during the callback.
 */
typedef void (*peerMessageCallback)(void *privateArg, peerConnection *connection,
                                    const messageFrame *message);

/*
 * Called once if a connection that completed its handshake later closes:
 * the peer disconnected, a network error, a protocol violation by the peer,
 * or the idle timeout. Not called after peerClose().
 */
typedef void (*peerClosedCallback)(void *privateArg, peerConnection *connection,
                                   int32_t status, const char *errorMessage);

typedef struct peerCallbacks {
    peerHandshakeCallback   onHandshake;    /* may be NULL */
    peerMessageCallback     onMessage;      /* may be NULL */
    peerClosedCallback      onClosed;       /* may be NULL */
} peerCallbacks;

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
 * handshakes. On PEER_OK the attempt is under way, *connection receives its
 * handle, and onHandshake will run once from the libuv loop. Any other
 * return value means nothing was started and no handle was created.
 * The address, options and callbacks are copied.
 */
int32_t peerConnect(const struct sockaddr *address, const peerConnectOptions *options,
                    const peerCallbacks *callbacks, void *privateArg,
                    peerConnection **connection);

/*
 * Send a message (any id, including MESSAGE_KEEP_ALIVE). Sending choke,
 * unchoke, interested or not interested updates our side of the state.
 * The message is copied, so the frame and its payload may be reused at
 * once. Returns PEER_EINVAL before the handshake has completed or for an
 * invalid message, and PEER_ECLOSED once the connection has closed.
 */
int32_t peerSend(peerConnection *connection, const messageFrame *message);

/* Copy out our view of the peer. Returns PEER_EINVAL for a closed handle. */
int32_t peerGetState(peerConnection *connection, peerRemoteState *state);

/*
 * Close a connection and release its handle. No callbacks run for it
 * afterwards. Safe to call from any of its callbacks.
 */
void peerClose(peerConnection *connection);

/*
 * Release one peerInit(). The last call closes every connection (no further
 * callbacks run). Keep running the loop until uv_run() returns so that all
 * memory is released.
 */
void peerShutdown(void);

/* Human-readable description of a PEER_* status. Never NULL. */
const char *peerStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* PEER_H */
