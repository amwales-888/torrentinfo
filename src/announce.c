/*
 * announce.c - Keeping a torrent's trackers up to date. See announce.h.
 *
 * Each tracker URL (duplicates removed) has its own schedule. A one-second
 * timer starts announces that are due; the tracker module's callback
 * records the answer and sets the next time. Stopping cancels whatever is
 * in flight and sends "stopped" to the trackers that know us.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <string.h>

#include "announce.h"

#define ANNOUNCE_TICK_MS        1000u
#define ANNOUNCE_STOP_TICK_MS   100u

typedef struct announceTracker {
    announce   *owner;
    const char *url;                /* in the torrent                    */
    uint32_t    id;                 /* tracker request in flight, or 0   */
    uint32_t    event;              /* of that request                   */
    uint8_t     started;            /* accepted our "started"            */
    uint8_t     completedDue;       /* owes a "completed"                */
    uint8_t     disabled;           /* unusable URL                      */
    uint32_t    failures;           /* in a row                          */
    uint64_t    nextAt;             /* uv_now() of the next announce     */
    uint64_t    lastAt;             /* uv_now() of the last one started  */
} announceTracker;

struct announce {
    uv_loop_t              *loop;
    parserContext          *context;
    uint8_t                 infoHash[PARSER_SHA1_LENGTH];
    announceEvents          events;
    void                   *privateArg;
    announceTracker        *trackers;
    uint32_t                trackerCount;
    uv_timer_t              timer;
    uint8_t                 stopping;
    uint8_t                 ended;
    uint64_t                stopDeadline;
    announceStoppedCallback stopCallback;
    void                   *stopArg;
};

static void *allocate(announce *a, size_t size)
{
    void *memory = a->context->allocator.realloc(NULL, size);

    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static void addTracker(announce *a, const char *url, uint32_t capacity)
{
    uint32_t i;

    if (url == NULL || url[0] == '\0' || a->trackerCount >= capacity) {
        return;
    }
    for (i = 0; i < a->trackerCount; i++) {
        if (strcmp(a->trackers[i].url, url) == 0) {
            return;
        }
    }
    a->trackers[a->trackerCount].owner = a;
    a->trackers[a->trackerCount].url = url;
    a->trackerCount++;
}

static uint32_t retryDelay(uint32_t failures)
{
    uint64_t delay = ANNOUNCE_RETRY_SECONDS;

    while (failures-- > 1u && delay < ANNOUNCE_MAX_RETRY_SECONDS) {
        delay *= 2u;
    }
    return (delay > ANNOUNCE_MAX_RETRY_SECONDS) ? ANNOUNCE_MAX_RETRY_SECONDS : (uint32_t)delay;
}

static void onTrackerDone(void *privateArg, uint32_t uniqueId);

/* Send one announce. */
static void startAnnounce(announce *a, announceTracker *tracker, uint32_t event, uint32_t timeout)
{
    trackerAnnounceParams params;
    announceStats         stats;
    int32_t               status;

    memset(&stats, 0, sizeof(stats));
    if (a->events.getStats != NULL) {
        a->events.getStats(a->privateArg, &stats);
    }
    memset(&params, 0, sizeof(params));
    params.event = event;
    params.uploaded = stats.uploaded;
    params.downloaded = stats.downloaded;
    params.left = stats.left;
    params.port = stats.port;
    tracker->lastAt = uv_now(a->loop);
    status = trackerAnnounce(a->infoHash, tracker->url, &params, &tracker->id, timeout,
                             onTrackerDone, tracker);
    if (status == TRACKER_OK) {
        tracker->event = event;
        return;
    }
    tracker->id = 0;
    if (status == TRACKER_EINVAL || status == TRACKER_ENOTSUP) {
        tracker->disabled = 1;                      /* this URL will never work */
    } else {
        tracker->failures++;
        tracker->nextAt = tracker->lastAt + (uint64_t)retryDelay(tracker->failures) * 1000u;
    }
    if (a->events.onResult != NULL && !a->stopping) {
        a->events.onResult(a->privateArg, tracker->url, event, status, NULL);
    }
}

static int stopFinished(const announce *a)
{
    uint32_t i;

    for (i = 0; i < a->trackerCount; i++) {
        if (a->trackers[i].id != 0) {
            return 0;
        }
    }
    return 1;
}

static void stopDone(announce *a)
{
    announceStoppedCallback callback = a->stopCallback;

    if (callback == NULL) {
        return;
    }
    a->stopCallback = NULL;
    uv_timer_stop(&a->timer);
    callback(a->stopArg);
}

static void onTrackerDone(void *privateArg, uint32_t uniqueId)
{
    announceTracker *tracker = privateArg;
    announce        *a = tracker->owner;
    trackerResult    result;
    uint64_t         now = uv_now(a->loop);
    uint32_t         interval;
    int32_t          status;

    if (a->ended || tracker->id != uniqueId) {
        return;
    }
    status = trackerInfo(tracker, uniqueId, &result);
    if (a->stopping) {
        trackerClose(uniqueId);
        tracker->id = 0;
        if (stopFinished(a)) {
            stopDone(a);
        }
        return;
    }
    if (status == TRACKER_OK) {
        if (tracker->event == TRACKER_EVENT_STARTED) {
            tracker->started = 1;
        } else if (tracker->event == TRACKER_EVENT_COMPLETED) {
            tracker->completedDue = 0;
        }
        tracker->failures = 0;
        interval = (result.interval != 0) ? result.interval : ANNOUNCE_DEFAULT_INTERVAL;
        if (interval < result.minInterval) {
            interval = result.minInterval;
        }
        if (interval < ANNOUNCE_MIN_INTERVAL) {
            interval = ANNOUNCE_MIN_INTERVAL;
        }
        if (interval > ANNOUNCE_MAX_INTERVAL) {
            interval = ANNOUNCE_MAX_INTERVAL;
        }
        tracker->nextAt = now + (uint64_t)interval * 1000u;
    } else {
        tracker->failures++;
        tracker->nextAt = now + (uint64_t)retryDelay(tracker->failures) * 1000u;
    }
    if (a->events.onResult != NULL) {
        a->events.onResult(a->privateArg, tracker->url, tracker->event, status, &result);
    }
    if (!a->ended && status == TRACKER_OK && result.peerCount > 0 && a->events.onPeers != NULL) {
        a->events.onPeers(a->privateArg, result.peers, result.peerCount);
    }
    if (!a->ended) {
        trackerClose(uniqueId);                     /* frees 'result' */
        tracker->id = 0;
    }
}

static void onTick(uv_timer_t *timer)
{
    announce *a = timer->data;
    uint64_t  now = uv_now(a->loop);
    uint32_t  i;

    if (a->ended) {
        return;
    }
    if (a->stopping) {
        if (now >= a->stopDeadline || stopFinished(a)) {
            for (i = 0; i < a->trackerCount; i++) {
                if (a->trackers[i].id != 0) {
                    trackerClose(a->trackers[i].id);
                    a->trackers[i].id = 0;
                }
            }
            stopDone(a);
        }
        return;
    }
    for (i = 0; i < a->trackerCount && !a->ended && !a->stopping; i++) {
        announceTracker *tracker = &a->trackers[i];
        uint32_t         event;

        if (tracker->disabled || tracker->id != 0 || now < tracker->nextAt) {
            continue;
        }
        event = !tracker->started      ? TRACKER_EVENT_STARTED
              : tracker->completedDue  ? TRACKER_EVENT_COMPLETED
                                       : TRACKER_EVENT_NONE;
        startAnnounce(a, tracker, event, ANNOUNCE_TIMEOUT_SECONDS);
    }
}

static void onTimerClosed(uv_handle_t *handle)
{
    announce *a = handle->data;

    a->context->allocator.free(a->trackers);
    a->context->allocator.free(a);
}

/* Allocate an announce for up to 'capacity' URLs (added by the caller). */
static announce *create(uv_loop_t *loop, parserContext *context,
                        const uint8_t infoHash[PARSER_SHA1_LENGTH], const announceEvents *events,
                        void *privateArg, uint32_t capacity)
{
    announce *a = context->allocator.realloc(NULL, sizeof(*a));

    if (a == NULL) {
        return NULL;
    }
    memset(a, 0, sizeof(*a));
    a->loop = loop;
    a->context = context;
    memcpy(a->infoHash, infoHash, PARSER_SHA1_LENGTH);
    a->events = *events;
    a->privateArg = privateArg;
    a->trackers = allocate(a, sizeof(*a->trackers) * (capacity > 0 ? capacity : 1u));
    if (a->trackers == NULL) {
        context->allocator.free(a);
        return NULL;
    }
    return a;
}

/* Start announcing to the trackers added, or fail if there are none. */
static int32_t start(announce *a, announce **handle)
{
    if (a->trackerCount == 0) {
        a->context->allocator.free(a->trackers);
        a->context->allocator.free(a);
        return ANNOUNCE_ENOTRACKERS;
    }
    uv_timer_init(a->loop, &a->timer);
    a->timer.data = a;
    uv_timer_start(&a->timer, onTick, 0, ANNOUNCE_TICK_MS);   /* first round now */
    *handle = a;
    return ANNOUNCE_OK;
}

int32_t announceBegin(uv_loop_t *loop, parserContext *context, const parserTorrent *torrent,
                      const announceEvents *events, void *privateArg, announce **handle)
{
    const parserTier *tier;
    const parserUrl  *url;
    announce         *a;
    uint32_t          capacity = 1;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (loop == NULL || context == NULL || torrent == NULL || events == NULL || handle == NULL) {
        return ANNOUNCE_EINVAL;
    }
    TAILQ_FOREACH(tier, &torrent->announceList, entries) {
        capacity += tier->urlCount;
    }
    a = create(loop, context, torrent->infoHash, events, privateArg, capacity);
    if (a == NULL) {
        return ANNOUNCE_ENOMEM;
    }
    /* BEP 12: announce-list replaces announce when present. */
    TAILQ_FOREACH(tier, &torrent->announceList, entries) {
        TAILQ_FOREACH(url, &tier->urls, entries) {
            addTracker(a, url->url, capacity);
        }
    }
    if (a->trackerCount == 0) {
        addTracker(a, torrent->announce, capacity);
    }
    return start(a, handle);
}

int32_t announceBeginUrls(uv_loop_t *loop, parserContext *context,
                          const uint8_t infoHash[PARSER_SHA1_LENGTH], char *const *urls,
                          uint32_t count, const announceEvents *events, void *privateArg,
                          announce **handle)
{
    announce *a;
    uint32_t  i;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (loop == NULL || context == NULL || infoHash == NULL || events == NULL || handle == NULL ||
        (count > 0 && urls == NULL)) {
        return ANNOUNCE_EINVAL;
    }
    a = create(loop, context, infoHash, events, privateArg, count);
    if (a == NULL) {
        return ANNOUNCE_ENOMEM;
    }
    for (i = 0; i < count; i++) {
        addTracker(a, urls[i], count);
    }
    return start(a, handle);
}

void announceCompleted(announce *a)
{
    uint32_t i;

    if (a == NULL || a->ended || a->stopping) {
        return;
    }
    for (i = 0; i < a->trackerCount; i++) {
        if (a->trackers[i].started) {
            a->trackers[i].completedDue = 1;
            a->trackers[i].nextAt = 0;              /* now, or after the one in flight */
        }
    }
}

void announceNow(announce *a)
{
    uint32_t i;

    if (a == NULL || a->ended || a->stopping) {
        return;
    }
    for (i = 0; i < a->trackerCount; i++) {
        announceTracker *tracker = &a->trackers[i];
        uint64_t         earliest = tracker->lastAt + (uint64_t)ANNOUNCE_MIN_INTERVAL * 1000u;

        if (tracker->failures == 0 && tracker->nextAt > earliest) {
            tracker->nextAt = earliest;
        }
    }
}

void announceStop(announce *a, uint32_t timeoutSeconds, announceStoppedCallback callback,
                  void *privateArg)
{
    uint32_t i;

    if (a == NULL || a->ended || a->stopping) {
        return;
    }
    a->stopping = 1;
    a->stopCallback = callback;
    a->stopArg = privateArg;
    a->stopDeadline = uv_now(a->loop) + (uint64_t)timeoutSeconds * 1000u;
    for (i = 0; i < a->trackerCount; i++) {
        announceTracker *tracker = &a->trackers[i];

        if (tracker->id != 0) {
            trackerClose(tracker->id);              /* whatever was in flight */
            tracker->id = 0;
        }
        if (tracker->started && !tracker->disabled && timeoutSeconds > 0) {
            startAnnounce(a, tracker, TRACKER_EVENT_STOPPED, timeoutSeconds);
        }
    }
    /* Check from the loop, so the callback never runs inside this call. */
    uv_timer_stop(&a->timer);
    uv_timer_start(&a->timer, onTick, 0, ANNOUNCE_STOP_TICK_MS);
}

void announceEnd(announce *a)
{
    uint32_t i;

    if (a == NULL || a->ended) {
        return;
    }
    a->ended = 1;
    for (i = 0; i < a->trackerCount; i++) {
        if (a->trackers[i].id != 0) {
            trackerClose(a->trackers[i].id);
            a->trackers[i].id = 0;
        }
    }
    uv_timer_stop(&a->timer);
    uv_close((uv_handle_t *)&a->timer, onTimerClosed);
}
