/*
 * magnet.h - Magnet links.
 *
 * A magnet link names a torrent by its info hash instead of carrying the
 * .torrent file:
 *
 *   magnet:?xt=urn:btih:<info hash>&dn=<name>&tr=<tracker>&tr=...
 *
 * magnetParse() reads one. The torrent's info dictionary ("metadata") is
 * then fetched from peers (see metadata.h), and magnetBuildTorrent() turns
 * it, with the link's trackers, into an ordinary .torrent file that the
 * parser reads and that can be saved for next time.
 *
 * Understood parameters: xt (urn:btih: with 40 hex or 32 base32 digits),
 * dn, tr, x.pe (peer addresses, "host:port"), ws (web seeds). Others are
 * ignored. Memory comes from the parserContext's allocator.
 */
#ifndef MAGNET_H
#define MAGNET_H

#include <stdint.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAGNET_OK           0
#define MAGNET_EINVAL       (-3)    /* not a magnet link, or no BitTorrent info hash */
#define MAGNET_ENOMEM       (-4)
#define MAGNET_MAX_ITEMS    64u     /* trackers, peers or web seeds kept, each */

typedef struct magnetLink {
    uint8_t     infoHash[PARSER_SHA1_LENGTH];
    char       *name;               /* dn, or NULL                           */
    char      **trackers;           /* tr                                    */
    uint32_t    trackerCount;
    char      **peers;              /* x.pe, "host:port" or "[v6]:port"      */
    uint32_t    peerCount;
    char      **webSeeds;           /* ws                                    */
    uint32_t    webSeedCount;
} magnetLink;

/* Is 'text' a magnet link (does it start with "magnet:")? */
int magnetIs(const char *text);

int32_t magnetParse(parserContext *context, const char *uri, magnetLink *link);
void magnetFree(parserContext *context, magnetLink *link);

/*
 * Build a .torrent file: the info dictionary 'info' (whose SHA-1 the caller
 * has checked), with the link's trackers as announce/announce-list and its
 * web seeds as url-list. *torrent is allocated from the context; release it
 * with the allocator's free.
 */
int32_t magnetBuildTorrent(parserContext *context, const magnetLink *link, const uint8_t *info,
                           uint64_t infoLength, uint8_t **torrent, uint64_t *length);

/*
 * The magnet link for a torrent: its info hash (xt), name (dn), trackers
 * (tr: the announce-list, or else announce) and web seeds (ws), each
 * percent-encoded. *uri is allocated from the context; release it with the
 * allocator's free.
 */
int32_t magnetFromTorrent(parserContext *context, const parserTorrent *torrent, char **uri);

#ifdef __cplusplus
}
#endif

#endif /* MAGNET_H */
