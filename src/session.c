/*
 * session.c - many torrents on one loop, sharing what can be shared. See
 * session.h.
 *
 * The session starts its shared parts from the loop (onKick): the
 * listeners, the tracker module, the port forwarding, the DHT node and the
 * tick. Torrent tasks added before that start after it (their own kicks are
 * queued later), so the services are there when they need them.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <string.h>

#include "datafile.h"
#include "dht.h"
#include "peer.h"
#include "session.h"
#include "tracker.h"

struct session {
    uv_loop_t      *loop;
    parserContext  *context;
    sessionOptions  options;
    sessionEvents   events;
    void           *privateArg;
    uv_timer_t      kick;                   /* starts the shared parts            */
    uv_timer_t      tick;                   /* every BANDWIDTH_TICK_MS            */
    uint8_t         started;

    /* Shared parts. */
    uint8_t         peerUp;
    uint8_t         trackerUp;
    peerListener   *listeners[2];           /* IPv4, IPv6                         */
    portmap        *mapper;
    dht            *dhtNode;
    httpConfig      http;
    downloadGate    gate;
    torrentHost     host;                   /* lent to every task                 */
    bandwidthBucket down;                   /* the session-wide limits            */
    bandwidthBucket up;

    /* Torrents, in the order added. */
    torrentTask   **tasks;
    uint32_t        taskCount;
    uint32_t        taskCapacity;
    uint32_t        ended;                  /* tasks that have ended              */
    bandwidthShare **downShares;            /* scratch for bandwidthDivide()      */
    bandwidthShare **upShares;

    uint8_t         stopping;
    uint8_t         closed;
};

static void closeShared(session *s);

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void notice(session *s, uint32_t code, uint64_t number, uint64_t number2,
                   int32_t status)
{
    sessionNotice n;

    if (s->events.onNotice == NULL) {
        return;
    }
    n.code = code;
    n.number = number;
    n.number2 = number2;
    n.status = status;
    s->events.onNotice(s->privateArg, s, &n);
}

static torrentTask *findTask(const session *s, const uint8_t infoHash[PARSER_SHA1_LENGTH])
{
    uint32_t i;

    for (i = 0; i < s->taskCount; i++) {
        if (memcmp(torrentInfoHash(s->tasks[i]), infoHash, PARSER_SHA1_LENGTH) == 0 &&
            torrentState(s->tasks[i]) != TORRENT_STATE_ENDED) {
            return s->tasks[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Incoming connections and connection caps                                 */
/* ------------------------------------------------------------------------ */

/* Encrypted connections reveal only SHA1("req2", info hash). */
static int lookupReq2(void *privateArg, const uint8_t req2[PARSER_SHA1_LENGTH],
                      uint8_t infoHash[PARSER_SHA1_LENGTH])
{
    session *s = privateArg;
    uint32_t i;

    for (i = 0; i < s->taskCount; i++) {
        if (memcmp(torrentReq2Hash(s->tasks[i]), req2, PARSER_SHA1_LENGTH) == 0 &&
            torrentState(s->tasks[i]) != TORRENT_STATE_ENDED) {
            memcpy(infoHash, torrentInfoHash(s->tasks[i]), PARSER_SHA1_LENGTH);
            return 1;
        }
    }
    return 0;
}

static int onIncoming(void *listenerArg, peerConnection *connection, const struct sockaddr *address,
                      const uint8_t infoHash[PARSER_SHA1_LENGTH], peerConnectOptions *options,
                      peerCallbacks *callbacks, void **privateArg)
{
    session     *s = listenerArg;
    torrentTask *task;

    if (s->stopping || s->closed) {
        return 0;
    }
    task = findTask(s, infoHash);
    return (task != NULL) && torrentAcceptIncoming(task, connection, address, infoHash, options,
                                                   callbacks, privateArg);
}

/* The caps on connections, counted over every download. */
static int mayConnect(void *privateArg, int incoming)
{
    session *s = privateArg;
    uint32_t open = 0;
    uint32_t connecting = 0;
    uint32_t i;

    for (i = 0; i < s->taskCount; i++) {
        uint32_t taskOpen;
        uint32_t taskConnecting;

        downloadConnections(torrentDownload(s->tasks[i]), &taskOpen, &taskConnecting);
        open += taskOpen;
        connecting += taskConnecting;
    }
    if (open + connecting >= s->options.maxConnections) {
        return 0;
    }
    return incoming || connecting < s->options.maxHalfOpen;
}

/* ------------------------------------------------------------------------ */
/* The tick                                                                 */
/* ------------------------------------------------------------------------ */

static void onTick(uv_timer_t *timer)
{
    session *s = timer->data;
    uint64_t now = uv_now(s->loop);
    uint32_t count = 0;
    uint32_t i;

    /* Divide the session's limits among the downloads, then let each one
     * work; the start rotates so nobody is always first. */
    for (i = 0; i < s->taskCount; i++) {
        if (torrentHasDownload(s->tasks[i])) {
            torrentShares(s->tasks[i], &s->downShares[count], &s->upShares[count]);
            count++;
        }
    }
    bandwidthDivide(&s->down, s->downShares, count, now);
    bandwidthDivide(&s->up, s->upShares, count, now);
    if (s->taskCount > 0) {
        uint32_t first = (uint32_t)((now / BANDWIDTH_TICK_MS) % s->taskCount);

        for (i = 0; i < s->taskCount && !s->closed; i++) {
            torrentTick(s->tasks[(first + i) % s->taskCount]);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Shared parts                                                             */
/* ------------------------------------------------------------------------ */

/* Listen on the port asked for, or the next free one of the following 9.
 * Either family may be missing (no IPv6, or an interface with one family). */
static void startListening(session *s)
{
    uint32_t attempt;

    if (s->options.listenPort == 0) {
        return;
    }
    for (attempt = 0; attempt < SESSION_LISTEN_ATTEMPTS &&
                      (uint32_t)s->options.listenPort + attempt <= 65535u; attempt++) {
        uint16_t port = (uint16_t)(s->options.listenPort + attempt);
        int32_t  v4 = peerListen(AF_INET, port, onIncoming, s, &s->listeners[0]);
        int32_t  v6;

        if (v4 == PEER_EADDRINUSE) {
            s->listeners[0] = NULL;
            continue;
        }
        v6 = peerListen(AF_INET6, port, onIncoming, s, &s->listeners[1]);
        if (v4 != PEER_OK) {
            s->listeners[0] = NULL;
        }
        if (v6 != PEER_OK) {
            s->listeners[1] = NULL;
        }
        if (v4 == PEER_OK || v6 == PEER_OK) {
            s->host.port = port;
        }
        peerListenerSetHashLookup(s->listeners[0], lookupReq2, s);
        peerListenerSetHashLookup(s->listeners[1], lookupReq2, s);
        break;
    }
    if (s->host.port != 0) {
        notice(s, SESSION_NOTICE_LISTENING, s->host.port, 0, 0);
        s->host.announcePort = s->host.port;
    } else {
        notice(s, SESSION_NOTICE_LISTEN_FAILED, s->options.listenPort,
               (uint64_t)s->options.listenPort + SESSION_LISTEN_ATTEMPTS - 1u, 0);
    }
}

static void onPortmap(void *privateArg, portmap *mapper, const portmapResult *result)
{
    session *s = privateArg;

    (void)mapper;
    if (s->events.onPortmap != NULL) {
        s->events.onPortmap(s->privateArg, s, result);
    }
}

/* The DHT node, loading the routing table saved last time. */
static void startDht(session *s)
{
    dhtOptions options;
    char       path[TORRENT_PATH_LENGTH];
    uint8_t   *state = NULL;
    uint64_t   length = 0;

    memset(&options, 0, sizeof(options));
    options.port = s->host.announcePort;
    options.binding = s->options.binding;
    options.blocklist = s->options.blocklist;
    options.bootstrap = s->options.dhtBootstrap;
    if (datafilePath(NULL, DATAFILE_DHT_STATE, path, sizeof(path)) == DATAFILE_OK &&
        datafileRead(&s->context->allocator, path, DHT_MAX_STATE, &state, &length, NULL) ==
            DATAFILE_OK) {
        options.state = state;
        options.stateLength = (size_t)length;
    }
    if (dhtBegin(s->loop, s->context, &options, &s->dhtNode) != DHT_OK) {
        s->dhtNode = NULL;
        notice(s, SESSION_NOTICE_DHT_FAILED, 0, 0, 0);
    }
    if (state != NULL) {
        s->context->allocator.free(state);
    }
    s->host.dhtNode = s->dhtNode;
}

/* Save the routing table for next time, then close the node. */
static void stopDht(session *s)
{
    uint8_t state[DHT_MAX_STATE];
    size_t  length;
    char    path[TORRENT_PATH_LENGTH];

    if (s->dhtNode == NULL) {
        return;
    }
    if (dhtSaveState(s->dhtNode, state, sizeof(state), &length) == DHT_OK &&
        datafilePath(NULL, DATAFILE_DHT_STATE, path, sizeof(path)) == DATAFILE_OK) {
        (void)datafileWriteAtomic(path, state, length, NULL);
    }
    dhtEnd(s->dhtNode);
    s->dhtNode = NULL;
    s->host.dhtNode = NULL;
}

static void onKick(uv_timer_t *timer)
{
    session      *s = timer->data;
    trackerConfig trackers;
    int32_t       status;

    if (s->closed || s->stopping) {
        return;
    }
    s->started = 1;
    startListening(s);

    memset(&trackers, 0, sizeof(trackers));
    memcpy(trackers.peerId, s->options.peerId, PEER_ID_LENGTH);
    trackers.port = s->host.announcePort;
    trackers.caFile = s->options.caFile;
    trackers.binding = s->options.binding;
    status = trackerInit(s->loop, s->context, &trackers);
    if (status == TRACKER_OK) {
        s->trackerUp = 1;
        s->host.trackers = 1;
    } else {
        notice(s, SESSION_NOTICE_TRACKERS_FAILED, 0, 0, status);
    }

    if (s->host.port != 0 && s->options.portForwarding) {
        portmapOptions mapping;

        memset(&mapping, 0, sizeof(mapping));
        mapping.port = s->host.port;
        mapping.binding = s->options.binding;
        if (portmapBegin(s->loop, s->context, &mapping, onPortmap, s, &s->mapper) !=
            PORTMAP_OK) {
            s->mapper = NULL;
        }
    }
    if (s->options.dht) {
        startDht(s);
    }
    uv_timer_start(&s->tick, onTick, BANDWIDTH_TICK_MS, BANDWIDTH_TICK_MS);
}

/* Every torrent has ended: close the shared parts and report. */
static void closeShared(session *s)
{
    uint32_t i;

    if (s->closed) {
        return;
    }
    s->closed = 1;
    uv_close((uv_handle_t *)&s->tick, NULL);
    uv_close((uv_handle_t *)&s->kick, NULL);
    for (i = 0; i < 2; i++) {
        peerListenerClose(s->listeners[i]);
        s->listeners[i] = NULL;
    }
    if (s->mapper != NULL) {
        portmapEnd(s->mapper);                      /* removes the forwarding */
        s->mapper = NULL;
    }
    stopDht(s);
    if (s->trackerUp) {
        trackerShutdown();
        s->trackerUp = 0;
        s->host.trackers = 0;
    }
    if (s->peerUp) {
        peerShutdown();
        s->peerUp = 0;
    }
    if (s->events.onEnded != NULL) {
        s->events.onEnded(s->privateArg, s);
    }
}

static int allEnded(const session *s)
{
    return s->ended == s->taskCount;
}

static void onCloseNow(uv_timer_t *timer)
{
    closeShared(timer->data);
}

/* Close once the session is stopping and every task has ended; from the
 * loop, so that the last task's own onEnded comes first. */
static void closeWhenDone(session *s)
{
    if (s->stopping && !s->closed && allEnded(s)) {
        uv_timer_start(&s->kick, onCloseNow, 0, 0);
    }
}

static void onTaskEnded(void *hostArg, torrentTask *task)
{
    session *s = hostArg;

    (void)task;
    s->ended++;
    closeWhenDone(s);
}

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

int32_t sessionBegin(uv_loop_t *loop, parserContext *context, const sessionOptions *options,
                     const sessionEvents *events, void *privateArg, session **owner)
{
    session *s;

    if (owner != NULL) {
        *owner = NULL;
    }
    if (loop == NULL || context == NULL || options == NULL || events == NULL || owner == NULL) {
        return SESSION_EINVAL;
    }
    s = context->allocator.realloc(NULL, sizeof(*s));
    if (s == NULL) {
        return SESSION_ENOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->loop = loop;
    s->context = context;
    s->options = *options;
    if (s->options.maxConnections == 0) {
        s->options.maxConnections = SESSION_DEFAULT_MAX_CONNECTIONS;
    }
    if (s->options.maxHalfOpen == 0) {
        s->options.maxHalfOpen = SESSION_DEFAULT_MAX_HALF_OPEN;
    }
    s->events = *events;
    s->privateArg = privateArg;
    if (peerInit(loop, context, options->binding) != PEER_OK) {
        context->allocator.free(s);
        return SESSION_EPEER;
    }
    s->peerUp = 1;

    s->http.caFile = options->caFile;
    s->http.binding = options->binding;
    s->gate.mayConnect = mayConnect;
    s->gate.privateArg = s;
    memcpy(s->host.peerId, options->peerId, PEER_ID_LENGTH);
    s->host.announcePort = (uint16_t)SESSION_DEFAULT_PORT;
    s->host.blocklist = options->blocklist;
    s->host.http = &s->http;
    s->host.gate = &s->gate;
    s->host.down = &s->down;
    s->host.up = &s->up;
    s->host.onTaskEnded = onTaskEnded;
    s->host.hostArg = s;
    bandwidthSet(&s->down, options->maxDownloadRate, uv_now(loop));
    bandwidthSet(&s->up, options->maxUploadRate, uv_now(loop));

    uv_timer_init(loop, &s->kick);
    s->kick.data = s;
    uv_timer_init(loop, &s->tick);
    s->tick.data = s;
    uv_timer_start(&s->kick, onKick, 0, 0);
    *owner = s;
    return SESSION_OK;
}

/* Room for one more task, and for its shares in the scratch arrays. */
static int grow(session *s)
{
    uint32_t        capacity;
    torrentTask   **tasks;
    bandwidthShare **downs;
    bandwidthShare **ups;

    if (s->taskCount < s->taskCapacity) {
        return 1;
    }
    capacity = (s->taskCapacity == 0) ? 8u : s->taskCapacity * 2u;
    tasks = s->context->allocator.realloc(s->tasks, capacity * sizeof(*tasks));
    if (tasks == NULL) {
        return 0;
    }
    s->tasks = tasks;
    downs = s->context->allocator.realloc(s->downShares, capacity * sizeof(*downs));
    if (downs == NULL) {
        return 0;
    }
    s->downShares = downs;
    ups = s->context->allocator.realloc(s->upShares, capacity * sizeof(*ups));
    if (ups == NULL) {
        return 0;
    }
    s->upShares = ups;
    s->taskCapacity = capacity;
    return 1;
}

int32_t sessionAddTorrent(session *s, const torrentOptions *options, const torrentEvents *events,
                          void *privateArg, torrentTask **task)
{
    int32_t status;

    if (task != NULL) {
        *task = NULL;
    }
    if (s == NULL || task == NULL) {
        return SESSION_EINVAL;
    }
    if (s->stopping || s->closed) {
        return SESSION_ESTOPPING;
    }
    if (!grow(s)) {
        return SESSION_ENOMEM;
    }
    status = torrentBegin(s->loop, s->context, &s->host, options, events, privateArg, task);
    if (status != TORRENT_OK) {
        return status;
    }
    s->tasks[s->taskCount++] = *task;
    return SESSION_OK;
}

void sessionSetRateLimits(session *s, uint64_t maxDownloadRate, uint64_t maxUploadRate)
{
    if (s == NULL || s->closed) {
        return;
    }
    bandwidthSet(&s->down, maxDownloadRate, uv_now(s->loop));
    bandwidthSet(&s->up, maxUploadRate, uv_now(s->loop));
}

void sessionGetStatus(const session *s, sessionStatus *status)
{
    uint32_t i;

    memset(status, 0, sizeof(*status));
    status->port = s->host.port;
    status->torrents = s->taskCount;
    status->running = s->taskCount - s->ended;
    status->stopping = s->stopping;
    if (s->dhtNode != NULL) {
        dhtStatus table;

        dhtGetStatus(s->dhtNode, &table);
        status->dhtRunning = 1;
        status->dhtNodes = table.nodes;
    }
    for (i = 0; i < s->taskCount; i++) {
        uint32_t open;
        uint32_t connecting;

        downloadConnections(torrentDownload(s->tasks[i]), &open, &connecting);
        status->connectionsOpen += open;
        status->connectionsConnecting += connecting;
    }
}

void sessionStop(session *s)
{
    uint32_t i;

    if (s == NULL || s->closed) {
        return;
    }
    /* First call: every torrent leaves politely; second: at once. Tasks
     * ending inside torrentStop() are counted by onTaskEnded. */
    s->stopping = 1;
    for (i = 0; i < s->taskCount && !s->closed; i++) {
        if (torrentState(s->tasks[i]) != TORRENT_STATE_ENDED) {
            torrentStop(s->tasks[i]);
        }
    }
    closeWhenDone(s);
}

void sessionEnd(session *s)
{
    parserContext *context;
    uint32_t       i;

    if (s == NULL) {
        return;
    }
    context = s->context;
    for (i = 0; i < s->taskCount; i++) {
        torrentEnd(s->tasks[i]);
    }
    context->allocator.free(s->tasks);
    context->allocator.free(s->downShares);
    context->allocator.free(s->upShares);
    context->allocator.free(s);
}

const char *sessionStatusString(int32_t status)
{
    switch (status) {
    case SESSION_OK:            return "success";
    case SESSION_EINVAL:        return "invalid argument";
    case SESSION_ENOMEM:        return "out of memory";
    case SESSION_EPEER:         return "the peer module could not start";
    case SESSION_ESTOPPING:     return "the session is stopping";
    default:                    return torrentStatusString(status);
    }
}
