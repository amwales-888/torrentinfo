/*
 * check_bandwidth.c - token buckets and shares of a session limit,
 * simulated over time: one download takes the whole limit, two split it
 * evenly, an idle one costs the others nothing, and no limit means none.
 */
#include <string.h>

#include "check.h"
#include "bandwidth.h"

#define BLOCK 16384u

/* Downloads that always want more: each takes blocks while its share
 * allows. 'busy' says which ones want anything at all. Returns bytes per
 * second for each over 'seconds'. */
static void simulate(uint64_t rate, uint32_t count, const int *busy, uint32_t seconds,
                     double *perSecond)
{
    bandwidthBucket shared;
    bandwidthShare  shares[4];
    bandwidthShare *list[4];
    uint64_t        used[4] = { 0, 0, 0, 0 };
    uint64_t        now = 0;
    uint32_t        tick;
    uint32_t        i;

    memset(shares, 0, sizeof(shares));
    for (i = 0; i < count; i++) {
        list[i] = &shares[i];
    }
    bandwidthSet(&shared, rate, 0);
    for (tick = 0; tick < seconds * (1000u / BANDWIDTH_TICK_MS); tick++) {
        now += BANDWIDTH_TICK_MS;
        bandwidthDivide(&shared, list, count, now);
        for (i = 0; i < count; i++) {
            uint32_t guard = 0;

            while (busy[i] && bandwidthShareAvailable(&shares[i]) && guard++ < 10000u) {
                bandwidthShareCharge(&shares[i], BLOCK);
                used[i] += BLOCK;
            }
        }
    }
    for (i = 0; i < count; i++) {
        perSecond[i] = (double)used[i] / seconds;
    }
}

static int near(double value, double expected, double tolerance)
{
    return value >= expected * (1.0 - tolerance) && value <= expected * (1.0 + tolerance);
}

void checkBandwidth(void)
{
    static const int allBusy[4] = { 1, 1, 1, 1 };
    static const int oneIdle[2] = { 1, 0 };
    bandwidthBucket  bucket;
    bandwidthShare   share;
    bandwidthShare  *list[1];
    double           rates[4];
    uint64_t         rate = 1024u * 1024u;

    /* A bucket: full at first (half a second), empty after that, refilled
     * with time, never above the burst. */
    bandwidthSet(&bucket, 100000u, 0);
    CHECK(bucket.tokens == 50000);
    CHECK(bandwidthAvailable(&bucket, 0));
    bandwidthCharge(&bucket, 60000u);
    CHECK(!bandwidthAvailable(&bucket, 0));
    CHECK(bandwidthAvailable(&bucket, 200));        /* +20000 */
    CHECK(bucket.tokens == 10000);
    CHECK(bandwidthAvailable(&bucket, 100000) && bucket.tokens == 50000);
    bandwidthSet(&bucket, 0, 0);
    bandwidthCharge(&bucket, 1000000u);
    CHECK(bandwidthAvailable(&bucket, 0));          /* no limit */
    CHECK(bandwidthBurst(1000) == BANDWIDTH_MIN_BURST);

    /* One download: the whole limit (and the first burst). */
    simulate(rate, 1, allBusy, 20, rates);
    CHECK(near(rates[0], (double)rate, 0.06));

    /* Two, three: equal parts. */
    simulate(rate, 2, allBusy, 20, rates);
    CHECK(near(rates[0], (double)rate / 2, 0.06) && near(rates[1], (double)rate / 2, 0.06));
    simulate(rate, 3, allBusy, 20, rates);
    CHECK(near(rates[0], (double)rate / 3, 0.08) && near(rates[1], (double)rate / 3, 0.08) &&
          near(rates[2], (double)rate / 3, 0.08));

    /* One busy, one idle: the busy one is not held to half. */
    simulate(rate, 2, oneIdle, 20, rates);
    CHECK(near(rates[0], (double)rate, 0.06) && rates[1] == 0.0);

    /* No session limit: shares are unlimited. */
    memset(&share, 0, sizeof(share));
    list[0] = &share;
    bandwidthSet(&bucket, 0, 0);
    bandwidthDivide(&bucket, list, 1, 100);
    CHECK(!bandwidthShareLimited(&share) && bandwidthShareAvailable(&share));
    CHECK(bandwidthShareAvailable(NULL) && !bandwidthShareLimited(NULL));

    /* A limited share with nothing granted waits, and says it wants more. */
    memset(&share, 0, sizeof(share));
    share.limited = 1;
    CHECK(!bandwidthShareAvailable(&share) && share.hungry);

    /* An overdraft is paid back from the next grant. */
    bandwidthSet(&bucket, 100000u, 0);
    memset(&share, 0, sizeof(share));
    bandwidthDivide(&bucket, list, 1, 0);           /* the first burst: 50000 */
    CHECK(share.grant == 50000);
    bandwidthShareCharge(&share, 60000u);           /* 10000 over */
    bandwidthDivide(&bucket, list, 1, 100);         /* +10000 refilled */
    CHECK(share.grant == 0);
}
