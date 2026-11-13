/*
 * blocklist.h - IP address blocklists.
 *
 * A plain text file of addresses not to talk to is read into two sorted
 * arrays of non-overlapping [first, last] ranges, one of 32-bit IPv4
 * addresses and one of 128-bit IPv6 addresses, so that a lookup is a binary
 * search: about 18 comparisons for a list of 200,000 ranges.
 *
 * One entry per line; blank lines and lines starting with '#', ';' or "//"
 * are ignored. Each entry may be:
 *
 *   1.2.3.4                     one address (IPv4 or IPv6)
 *   10.0.0.0/8   2001:db8::/32  a CIDR block
 *   1.2.3.0 - 1.2.3.255         a range (spaces optional)
 *   Some name:1.2.3.0-1.2.3.255 PeerGuardian "P2P" format
 *   001.002.003.000 - 001.002.003.255 , 000 , name
 *                               eMule ipfilter.dat format: entries with an
 *                               access level of 128 or more allow, and are
 *                               skipped
 *
 * IPv4 addresses may have leading zeros (ipfilter.dat does); they are read
 * as decimal. IPv4-mapped IPv6 addresses (::ffff:1.2.3.4) are checked
 * against the IPv4 ranges. Lines that cannot be read are counted, not fatal.
 */
#ifndef BLOCKLIST_H
#define BLOCKLIST_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCKLIST_OK            0
#define BLOCKLIST_EINVAL        (-3)
#define BLOCKLIST_ENOMEM        (-4)
#define BLOCKLIST_EIO           (-5)    /* the file could not be read */
#define BLOCKLIST_MAX_FILE      (512u * 1024u * 1024u)

typedef struct blocklist blocklist;

typedef struct blocklistStats {
    uint32_t    lines;                  /* lines in the file                 */
    uint32_t    entries;                /* lines that named addresses        */
    uint32_t    allowed;                /* ipfilter.dat lines that allow     */
    uint32_t    rejected;               /* lines not understood              */
    uint32_t    firstRejected;          /* line number of the first, or 0    */
    uint32_t    ranges4;                /* IPv4 ranges after merging         */
    uint32_t    ranges6;                /* IPv6 ranges after merging         */
    uint64_t    addresses4;             /* IPv4 addresses covered            */
} blocklistStats;

/* Parse text (not necessarily zero-terminated) into a blocklist. */
int32_t blocklistParse(parserContext *context, const char *text, size_t length,
                       blocklist **list, blocklistStats *stats);

/* Read and parse a file. BLOCKLIST_EIO (with the libuv error in *systemError,
 * if not NULL) if it cannot be read; UV_ENOENT means it does not exist. */
int32_t blocklistLoad(parserContext *context, const char *path, blocklist **list,
                      blocklistStats *stats, int32_t *systemError);

/* Is the address (IPv4 or IPv6; the port does not matter) blocked? A NULL
 * list blocks nothing. */
int blocklistContains(const blocklist *list, const struct sockaddr *address);

void blocklistFree(parserContext *context, blocklist *list);

#ifdef __cplusplus
}
#endif

#endif /* BLOCKLIST_H */
