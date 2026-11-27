/*
 * bandwidth.h - speed limits.
 *
 * A bucket is a token bucket of bytes, refilled continuously at its rate
 * and holding at most half a second's worth (and at least one block), so
 * up to that much may go at once. A download keeps two of its own (its
 * per-torrent limits).
 *
 * A session limit is shared by many downloads. Each download gets a share;
 * every tick (100 ms) bandwidthDivide() refills the session's bucket, takes
 * back what the shares did not use, and divides it equally among the
 * downloads that sent something since the last tick or wanted to and could
 * not. So one busy download can use the whole limit, several split it
 * evenly, and an idle one costs the others nothing. A block passes only if
 * both its download's own bucket and its share allow it.
 *
 * Nothing here allocates; nothing is thread-safe (one loop thread).
 */
#ifndef BANDWIDTH_H
#define BANDWIDTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BANDWIDTH_MIN_BURST     16384u  /* one block, whatever the rate         */
#define BANDWIDTH_TICK_MS       100u    /* how often shares are divided         */

typedef struct bandwidthBucket {
    uint64_t rate;                      /* bytes per second; 0: no limit        */
    int64_t  tokens;                    /* may dip one block below zero         */
    uint64_t updated;                   /* ms (uv_now) of the last refill       */
} bandwidthBucket;

/* A download's part of a session limit. */
typedef struct bandwidthShare {
    int64_t  grant;                     /* bytes it may still send this tick    */
    uint64_t used;                      /* bytes sent since the last division   */
    uint8_t  limited;                   /* a session limit applies              */
    uint8_t  hungry;                    /* wanted to send but had no grant      */
} bandwidthShare;

/* Set a bucket's rate (0: no limit) and fill it. */
void bandwidthSet(bandwidthBucket *bucket, uint64_t rate, uint64_t now);

/* Refill for the time since the last call; may a block go? */
int bandwidthAvailable(bandwidthBucket *bucket, uint64_t now);

/* Take bytes from a limited bucket. */
void bandwidthCharge(bandwidthBucket *bucket, uint32_t bytes);

/* The largest burst a rate allows: half a second's worth, at least a block. */
uint64_t bandwidthBurst(uint64_t rate);

/* For a download: does a share (NULL: none) let a block go? Marks the share
 * hungry when it does not. */
int bandwidthShareAvailable(bandwidthShare *share);
void bandwidthShareCharge(bandwidthShare *share, uint32_t bytes);
int bandwidthShareLimited(const bandwidthShare *share);

/*
 * For the session, every tick: refill 'shared' and divide it among the
 * shares (see above). With no limit (rate 0) the shares are simply marked
 * unlimited.
 */
void bandwidthDivide(bandwidthBucket *shared, bandwidthShare *const *shares, uint32_t count,
                     uint64_t now);

#ifdef __cplusplus
}
#endif

#endif /* BANDWIDTH_H */
