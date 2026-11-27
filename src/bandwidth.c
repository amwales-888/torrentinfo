/*
 * bandwidth.c - token buckets and shares of a session limit. See
 * bandwidth.h.
 */
#include <stddef.h>

#include "bandwidth.h"

uint64_t bandwidthBurst(uint64_t rate)
{
    uint64_t burst = rate / 2u;

    return (burst < BANDWIDTH_MIN_BURST) ? BANDWIDTH_MIN_BURST : burst;
}

void bandwidthSet(bandwidthBucket *bucket, uint64_t rate, uint64_t now)
{
    bucket->rate = rate;
    bucket->tokens = (int64_t)bandwidthBurst(rate);
    bucket->updated = now;
}

static void refill(bandwidthBucket *bucket, uint64_t now)
{
    uint64_t burst;

    if (now <= bucket->updated) {
        return;
    }
    burst = bandwidthBurst(bucket->rate);
    bucket->tokens += (int64_t)(bucket->rate * (now - bucket->updated) / 1000u);
    if (bucket->tokens > (int64_t)burst) {
        bucket->tokens = (int64_t)burst;
    }
    bucket->updated = now;
}

int bandwidthAvailable(bandwidthBucket *bucket, uint64_t now)
{
    if (bucket->rate == 0) {
        return 1;
    }
    refill(bucket, now);
    return bucket->tokens > 0;
}

void bandwidthCharge(bandwidthBucket *bucket, uint32_t bytes)
{
    if (bucket->rate != 0) {
        bucket->tokens -= (int64_t)bytes;
    }
}

int bandwidthShareAvailable(bandwidthShare *share)
{
    if (share == NULL || !share->limited) {
        return 1;
    }
    if (share->grant > 0) {
        return 1;
    }
    share->hungry = 1;
    return 0;
}

void bandwidthShareCharge(bandwidthShare *share, uint32_t bytes)
{
    if (share == NULL) {
        return;
    }
    share->used += bytes;
    if (share->limited) {
        share->grant -= (int64_t)bytes;
    }
}

int bandwidthShareLimited(const bandwidthShare *share)
{
    return share != NULL && share->limited;
}

void bandwidthDivide(bandwidthBucket *shared, bandwidthShare *const *shares, uint32_t count,
                     uint64_t now)
{
    int64_t  pool;
    int64_t  each;
    uint32_t active = 0;
    uint32_t i;

    if (shared->rate == 0) {
        for (i = 0; i < count; i++) {
            shares[i]->limited = 0;
            shares[i]->grant = 0;
            shares[i]->used = 0;
            shares[i]->hungry = 0;
        }
        return;
    }
    /* What the bucket has now, plus what the shares did not use. */
    refill(shared, now);
    pool = shared->tokens;
    for (i = 0; i < count; i++) {
        if (shares[i]->limited && shares[i]->grant > 0) {
            pool += shares[i]->grant;
        }
        if (shares[i]->used > 0 || shares[i]->hungry) {
            active++;
        }
    }
    if (pool > (int64_t)bandwidthBurst(shared->rate)) {
        pool = (int64_t)bandwidthBurst(shared->rate);
    }
    if (pool < 0) {
        pool = 0;
    }
    /* Equal parts for those that are sending (or everyone, if none is). */
    each = (count == 0) ? 0 : pool / (int64_t)((active > 0) ? active : count);
    shared->tokens = pool;
    for (i = 0; i < count; i++) {
        bandwidthShare *share = shares[i];
        int64_t         debt = (share->limited && share->grant < 0) ? share->grant : 0;
        int             gets = (active == 0) || share->used > 0 || share->hungry;

        share->limited = 1;
        share->grant = debt + (gets ? each : 0);
        shared->tokens -= gets ? each : 0;
        share->used = 0;
        share->hungry = 0;
    }
}
