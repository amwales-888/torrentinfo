/*
 * event.h - what the engine reports while it works.
 *
 * Each report has a level (how much a user cares), a code (what happened,
 * for programs to act on without reading text) and a readable message, plus
 * the peer, piece or web seed it concerns when there is one. A program can
 * show what it likes: the torrentinfo CLI prints every message; a daemon
 * might log warnings and keep the rest for "watch".
 */
#ifndef EVENT_H
#define EVENT_H

#include <stdint.h>

#include <uv.h>     /* struct sockaddr */

#ifdef __cplusplus
extern "C" {
#endif

/* Levels: lower is more important. */
#define EVENT_LEVEL_ERROR           0u  /* the work cannot go on as asked        */
#define EVENT_LEVEL_WARNING         1u  /* something went wrong; work goes on    */
#define EVENT_LEVEL_INFO            2u  /* a milestone a user wants to see       */
#define EVENT_LEVEL_DEBUG           3u  /* detail: single peers coming and going */

/* Codes. New codes are only ever added at the end. */
#define EVENT_NONE                  0u
/* download */
#define EVENT_ENDGAME               1u  /* every remaining piece is in progress  */
#define EVENT_COMPLETE              2u  /* all wanted pieces on disk; seeding    */
#define EVENT_PIECE_FAILED          3u  /* a piece failed its hash check (piece) */
#define EVENT_PIECE_READ_FAILED     4u  /* could not read a piece to upload      */
#define EVENT_PEER_CONNECTED        5u  /* handshake done (peer)                 */
#define EVENT_PEER_CONNECT_FAILED   6u  /* (peer)                                */
#define EVENT_PEER_NO_HANDSHAKE     7u  /* connected, but no handshake (peer)    */
#define EVENT_PEER_DROPPED          8u  /* (peer)                                */
#define EVENT_PEER_EXCLUDED         9u  /* misbehaved: not tried again (peer)    */
#define EVENT_PEX_PEERS             10u /* peer exchange named new peers (peer)  */
#define EVENT_WEBSEED_STARTED       11u /* (url)                                 */
#define EVENT_WEBSEED_RETRY         12u /* a request failed; trying later (url)  */
#define EVENT_WEBSEED_CORRUPT       13u /* sent a corrupt piece (url, piece)     */
#define EVENT_WEBSEED_DISABLED      14u /* not used any more (url)               */
#define EVENT_WEBSEED_UNUSABLE      15u /* not http or https (url)               */
#define EVENT_WEBSEEDS_UNAVAILABLE  16u /* HTTP could not be started             */
#define EVENT_RESUME_LOADED         17u /* what the resume file vouched for      */
#define EVENT_RESUME_SAVE_FAILED    18u
/* metadata (magnet links) */
#define EVENT_METADATA_SIZE         19u /* a peer told us the metadata size      */
#define EVENT_METADATA_MISMATCH     20u /* got metadata with the wrong hash      */
#define EVENT_METADATA_REFUSED      21u /* peers have it but will not send it    */
#define EVENT_METADATA_PEER         22u /* a peer could not help (peer)          */
#define EVENT_CODE_COUNT            23u

#define EVENT_NO_PIECE              (-1)

typedef struct eventInfo {
    uint32_t               level;       /* EVENT_LEVEL_*                         */
    uint32_t               code;        /* EVENT_*                               */
    const char            *message;     /* readable; valid during the callback   */
    const struct sockaddr *peer;        /* the peer concerned, or NULL           */
    int64_t                piece;       /* the piece concerned, or EVENT_NO_PIECE */
    const char            *url;         /* the web seed concerned, or NULL       */
} eventInfo;

/* "error", "warning", "info", "debug". */
const char *eventLevelName(uint32_t level);

/* A short fixed name for a code, e.g. "peer-connected"; for logs. */
const char *eventCodeName(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
