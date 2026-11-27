/*
 * torrent.c - one torrent, from "start" to "stopped". See torrent.h.
 *
 * Both phases of a magnet link (the metadata, then the download) use the
 * session's services (torrentHost). Stopping always goes through
 * closeAll(), once, which ends every part in turn and then reports
 * onEnded.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <string.h>

#include "announce.h"
#include "datafile.h"
#include "dht.h"
#include "magnet.h"
#include "torrent.h"

#define TORRENT_MAX_SIZE    (64u * 1024u * 1024u)  /* a cached .torrent at most      */
#define TORRENT_STOP_WAIT   5u                      /* seconds for "stopped" announces */

struct torrentTask {
    uv_loop_t              *loop;
    parserContext          *context;
    const torrentHost      *host;
    torrentOptions          options;                /* pointers borrowed (torrent.h) */
    torrentEvents           events;
    void                   *privateArg;
    uint32_t                state;
    uv_timer_t              kick;                   /* starts the work in the loop   */

    /* The torrent: its info hash (and SHA1("req2", it)), its bytes and the
     * parsed form, once known. */
    uint8_t                 infoHash[PARSER_SHA1_LENGTH];
    uint8_t                 req2[PARSER_SHA1_LENGTH];
    uint8_t                *data;
    uint64_t                length;
    parserTorrent           metainfo;
    uint8_t                 parsed;
    magnetLink              link;
    uint8_t                 hasLink;
    char                    cachePath[TORRENT_PATH_LENGTH];

    /* Services: the session's DHT node, our lookup in it, our announces,
     * and our shares of the session's speed limits. */
    dhtTorrent             *dhtEntry;
    announce               *trackers;
    bandwidthShare          downShare;
    bandwidthShare          upShare;

    /* The metadata phase. */
    metadataFetch          *fetch;
    struct sockaddr_storage known[TORRENT_KNOWN_PEERS];  /* peers met, for later */
    uint32_t                knownCount;

    /* The download phase. */
    download               *transfer;
    char                    resumePath[TORRENT_PATH_LENGTH];

    /* Ending. */
    uint8_t                 stopping;               /* leaving politely             */
    uint8_t                 closed;                 /* closeAll() has run           */
    uint8_t                 complete;
    int32_t                 result;
    int32_t                 detail;
};

static void closeAll(torrentTask *task);
static void startDownload(torrentTask *task);

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static void setState(torrentTask *task, uint32_t state)
{
    task->state = state;
    if (task->events.onState != NULL) {
        task->events.onState(task->privateArg, task, state);
    }
}

static void notice(torrentTask *task, uint32_t code, const char *path, uint64_t number,
                   uint64_t number2, int32_t status)
{
    torrentNotice n;

    if (task->events.onNotice == NULL) {
        return;
    }
    n.code = code;
    n.path = path;
    n.number = number;
    n.number2 = number2;
    n.status = status;
    task->events.onNotice(task->privateArg, task, &n);
}

/* End with this result (the first one counts). */
static void fail(torrentTask *task, int32_t result, int32_t detail)
{
    if (task->result == TORRENT_OK) {
        task->result = result;
        task->detail = detail;
    }
    closeAll(task);
}

static void rememberPeer(torrentTask *task, const struct sockaddr *address)
{
    if (task->knownCount < TORRENT_KNOWN_PEERS) {
        memcpy(&task->known[task->knownCount++], address,
               (address->sa_family == AF_INET) ? sizeof(struct sockaddr_in)
                                               : sizeof(struct sockaddr_in6));
    }
}

/* "1.2.3.4:6881" or "[::1]:6881" (a magnet link's x.pe); host names are not
 * looked up. */
static int parsePeerAddress(const char *text, struct sockaddr_storage *address)
{
    char        host[64];
    const char *colon = strrchr(text, ':');
    const char *start = text;
    size_t      length;
    long        port = 0;
    const char *p;

    if (colon == NULL) {
        return 0;
    }
    for (p = colon + 1; *p >= '0' && *p <= '9' && port <= 65535; p++) {
        port = port * 10 + (*p - '0');
    }
    if (*start == '[') {
        start++;
    }
    length = (size_t)(colon - start);
    if (length > 0 && start[length - 1u] == ']') {
        length--;
    }
    if (port <= 0 || port > 65535 || length == 0 || length >= sizeof(host)) {
        return 0;
    }
    memcpy(host, start, length);
    host[length] = '\0';
    memset(address, 0, sizeof(*address));
    return uv_ip4_addr(host, (int)port, (struct sockaddr_in *)address) == 0 ||
           uv_ip6_addr(host, (int)port, (struct sockaddr_in6 *)address) == 0;
}

/* ------------------------------------------------------------------------ */
/* Shared services: the tracker module and the DHT                          */
/* ------------------------------------------------------------------------ */

/* Stop our DHT lookup (the node is the session's). */
static void leaveDht(torrentTask *task)
{
    if (task->dhtEntry != NULL && task->host->dhtNode != NULL) {
        dhtRemoveTorrent(task->host->dhtNode, task->dhtEntry);
    }
    task->dhtEntry = NULL;
}

static void onDhtDone(void *privateArg, uint32_t peers, uint32_t nodes)
{
    torrentTask *task = privateArg;
    dhtStatus    status;

    dhtGetStatus(task->host->dhtNode, &status);
    if (task->events.onDht != NULL) {
        task->events.onDht(task->privateArg, task, peers, nodes, status.nodes);
    }
}

static void onTrackerResult(void *privateArg, const char *url, uint32_t event, int32_t status,
                            const trackerResult *result)
{
    torrentTask *task = privateArg;

    if (task->events.onTracker != NULL) {
        task->events.onTracker(task->privateArg, task, url, event, status, result);
    }
}

/* ------------------------------------------------------------------------ */
/* The metadata phase (magnet links)                                        */
/* ------------------------------------------------------------------------ */

static void endFetch(torrentTask *task)
{
    if (task->state == TORRENT_STATE_METADATA && task->trackers != NULL) {
        announceEnd(task->trackers);                /* no "stopped": we stay */
        task->trackers = NULL;
    }
    if (task->fetch != NULL) {
        metadataEnd(task->fetch);
        task->fetch = NULL;
    }
    if (task->transfer == NULL) {
        leaveDht(task);
    }
}

static void getFetchStats(void *privateArg, announceStats *stats)
{
    torrentTask *task = privateArg;

    stats->uploaded = 0;
    stats->downloaded = 0;
    stats->left = 16384u;                           /* not known yet: "a leecher" */
    stats->port = task->host->announcePort;
}

static void onFetchPeers(void *privateArg, const trackerPeer *peers, uint32_t count)
{
    torrentTask            *task = privateArg;
    struct sockaddr_storage address;
    uint32_t                i;

    for (i = 0; i < count && task->fetch != NULL; i++) {
        trackerPeerAddress(&peers[i], &address);
        metadataAddPeer(task->fetch, (const struct sockaddr *)&address);
        rememberPeer(task, (const struct sockaddr *)&address);
    }
}

static void onFetchDhtPeers(void *privateArg, const struct sockaddr_in *peers, uint32_t count)
{
    torrentTask            *task = privateArg;
    struct sockaddr_storage address;
    uint32_t                i;

    for (i = 0; i < count && task->fetch != NULL; i++) {
        memset(&address, 0, sizeof(address));
        memcpy(&address, &peers[i], sizeof(peers[i]));
        metadataAddPeer(task->fetch, (const struct sockaddr *)&address);
        rememberPeer(task, (const struct sockaddr *)&address);
    }
}

static void onFetchEvent(void *privateArg, metadataFetch *fetch, const eventInfo *event)
{
    torrentTask *task = privateArg;

    (void)fetch;
    if (task->events.onEvent != NULL) {
        task->events.onEvent(task->privateArg, task, event);
    }
}

/* Take the torrent's bytes and parse them; 0 if they do not parse. */
static int adoptTorrent(torrentTask *task, uint8_t *data, uint64_t length)
{
    int32_t status;

    task->data = data;
    task->length = length;
    status = parserParse(task->context, data, length, &task->metainfo);
    if (status != PARSER_OK) {
        task->detail = status;
        return 0;
    }
    task->parsed = 1;
    return 1;
}

static void onFetchDone(void *privateArg, metadataFetch *fetch, const uint8_t *info,
                        uint32_t length)
{
    torrentTask *task = privateArg;
    uint8_t     *built = NULL;
    uint64_t     builtLength = 0;

    (void)fetch;
    if (magnetBuildTorrent(task->context, &task->link, info, length, &built, &builtLength) !=
        MAGNET_OK) {
        endFetch(task);
        fail(task, TORRENT_EMETADATA, 0);
        return;
    }
    endFetch(task);                                 /* 'info' is not used after this */
    notice(task, TORRENT_NOTICE_METADATA, NULL, length, 0, 0);
    if (task->cachePath[0] != '\0' &&
        datafileWriteAtomic(task->cachePath, built, builtLength, NULL) == DATAFILE_OK) {
        notice(task, TORRENT_NOTICE_SAVED, task->cachePath, 0, 0, 0);
    }
    if (!adoptTorrent(task, built, builtLength)) {
        fail(task, TORRENT_EPARSE, task->detail);
        return;
    }
    startDownload(task);
}

/* A magnet link: the torrent fetched before, or fetch it now. */
static void startMagnet(torrentTask *task)
{
    static const announceEvents callbacks = { getFetchStats, onFetchPeers, onTrackerResult };
    metadataEvents              events = { onFetchEvent, onFetchDone };
    metadataOptions             options;
    struct sockaddr_storage     address;
    uint8_t                    *cached = NULL;
    uint64_t                    cachedLength = 0;
    uint32_t                    i;

    if (datafileTorrentPath(task->link.infoHash, task->cachePath, sizeof(task->cachePath)) !=
        DATAFILE_OK) {
        task->cachePath[0] = '\0';                  /* no data directory: no cache */
    }
    if (task->cachePath[0] != '\0' &&
        datafileRead(&task->context->allocator, task->cachePath, TORRENT_MAX_SIZE, &cached,
                     &cachedLength, NULL) == DATAFILE_OK) {
        if (adoptTorrent(task, cached, cachedLength) &&
            memcmp(task->metainfo.infoHash, task->link.infoHash, PARSER_SHA1_LENGTH) == 0) {
            notice(task, TORRENT_NOTICE_CACHED, task->cachePath, 0, 0, 0);
            startDownload(task);
            return;
        }
        if (task->parsed) {                         /* another torrent: fetch again */
            parserFree(task->context, &task->metainfo);
            task->parsed = 0;
        }
        task->context->allocator.free(cached);
        task->data = NULL;
        task->length = 0;
        task->detail = 0;
    }
    if (task->link.trackerCount == 0 && task->link.peerCount == 0 &&
        task->options.peerCount == 0 && !(task->options.dht && task->host->dhtNode != NULL)) {
        fail(task, TORRENT_ENOSOURCES, 0);
        return;
    }

    setState(task, TORRENT_STATE_METADATA);
    memset(&options, 0, sizeof(options));
    memcpy(options.infoHash, task->link.infoHash, PARSER_SHA1_LENGTH);
    memcpy(options.peerId, task->host->peerId, PEER_ID_LENGTH);
    options.handshakeTimeoutSeconds = task->options.handshakeTimeoutSeconds;
    options.listenPort = task->host->port;
    options.blocklist = task->host->blocklist;
    options.maxPeers = task->options.maxPeers;
    if (metadataBegin(task->loop, task->context, &options, &events, task, &task->fetch) !=
        METADATA_OK) {
        task->fetch = NULL;
        fail(task, TORRENT_EMETADATA, 0);
        return;
    }
    for (i = 0; i < task->link.peerCount; i++) {
        if (parsePeerAddress(task->link.peers[i], &address)) {
            metadataAddPeer(task->fetch, (const struct sockaddr *)&address);
            rememberPeer(task, (const struct sockaddr *)&address);
        }
    }
    for (i = 0; i < task->options.peerCount; i++) {
        if (parsePeerAddress(task->options.peers[i], &address)) {
            metadataAddPeer(task->fetch, (const struct sockaddr *)&address);
            rememberPeer(task, (const struct sockaddr *)&address);
        }
    }
    if (task->options.dht && task->host->dhtNode != NULL &&
        dhtAddTorrent(task->host->dhtNode, task->link.infoHash, 0, onFetchDhtPeers, onDhtDone,
                      task, &task->dhtEntry) != DHT_OK) {
        task->dhtEntry = NULL;
        notice(task, TORRENT_NOTICE_DHT_FAILED, NULL, 0, 0, 0);
    }
    if (task->link.trackerCount > 0 && task->host->trackers) {
        if (announceBeginUrls(task->loop, task->context, task->link.infoHash,
                              task->link.trackers, task->link.trackerCount, &callbacks, task,
                              &task->trackers) != ANNOUNCE_OK) {
            task->trackers = NULL;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The download phase                                                       */
/* ------------------------------------------------------------------------ */

static void getAnnounceStats(void *privateArg, announceStats *stats)
{
    torrentTask     *task = privateArg;
    downloadProgress progress;

    downloadGetProgress(task->transfer, &progress);
    stats->uploaded = progress.bytesUploaded;
    stats->downloaded = progress.bytesReceived;
    stats->left = progress.left;
    stats->port = task->host->announcePort;
}

static void onAnnouncePeers(void *privateArg, const trackerPeer *peers, uint32_t count)
{
    torrentTask            *task = privateArg;
    struct sockaddr_storage address;
    uint32_t                i;

    for (i = 0; i < count && task->transfer != NULL; i++) {
        trackerPeerAddress(&peers[i], &address);
        downloadAddPeer(task->transfer, (const struct sockaddr *)&address);
    }
}

static void onDhtPeers(void *privateArg, const struct sockaddr_in *peers, uint32_t count)
{
    torrentTask            *task = privateArg;
    struct sockaddr_storage address;
    uint32_t                i;

    for (i = 0; i < count && task->transfer != NULL; i++) {
        memset(&address, 0, sizeof(address));
        memcpy(&address, &peers[i], sizeof(peers[i]));
        downloadAddPeer(task->transfer, (const struct sockaddr *)&address);
    }
}

static void onDownloadReady(void *privateArg, download *transfer, int32_t status,
                            const char *message)
{
    static const announceEvents callbacks = { getAnnounceStats, onAnnouncePeers,
                                              onTrackerResult };
    torrentTask     *task = privateArg;
    downloadProgress progress;

    (void)message;
    if (status != DOWNLOAD_OK) {
        return;                                     /* onDownloadFinished follows */
    }
    downloadGetProgress(transfer, &progress);
    setState(task, TORRENT_STATE_RUNNING);
    if (task->events.onReady != NULL) {
        task->events.onReady(task->privateArg, task);
    }
    if (task->closed) {
        return;                                     /* stopped from the callback */
    }
    if (progress.done >= progress.wanted && task->options.seed) {
        task->complete = 1;                         /* complete already: seeding only */
    }

    /* Announce now that "left" is known (unless there is nothing to do). */
    if (task->host->trackers && task->trackers == NULL &&
        (progress.done < progress.wanted || task->options.seed)) {
        status = announceBegin(task->loop, task->context, &task->metainfo, &callbacks, task,
                               &task->trackers);
        if (status == ANNOUNCE_ENOTRACKERS) {
            task->trackers = NULL;
            notice(task, TORRENT_NOTICE_NO_TRACKERS, NULL, 0, 0, 0);
        } else if (status != ANNOUNCE_OK) {
            task->trackers = NULL;
            notice(task, TORRENT_NOTICE_ANNOUNCE_FAILED, NULL, 0, 0, status);
        }
    }
}

static void onDownloadEvent(void *privateArg, download *transfer, const eventInfo *event)
{
    torrentTask *task = privateArg;

    (void)transfer;
    if (task->events.onEvent != NULL) {
        task->events.onEvent(task->privateArg, task, event);
    }
}

static void onDownloadComplete(void *privateArg, download *transfer)
{
    torrentTask *task = privateArg;

    (void)transfer;
    task->complete = 1;
    if (task->events.onComplete != NULL) {
        task->events.onComplete(task->privateArg, task);
    }
    if (task->trackers != NULL) {
        announceCompleted(task->trackers);
    }
}

static void onTrackersStopped(void *privateArg)
{
    closeAll(privateArg);
}

/* Leave the swarm: tell the trackers (a few seconds at most), then stop. */
static void leave(torrentTask *task)
{
    if (task->stopping || task->closed) {
        return;
    }
    task->stopping = 1;
    setState(task, TORRENT_STATE_STOPPING);
    if (task->trackers != NULL) {
        notice(task, TORRENT_NOTICE_LEAVING, NULL, 0, 0, 0);
        announceStop(task->trackers, TORRENT_STOP_WAIT, onTrackersStopped, task);
    } else {
        closeAll(task);
    }
}

static void onDownloadFinished(void *privateArg, download *transfer, int32_t status,
                               const char *message)
{
    torrentTask *task = privateArg;

    (void)transfer;
    if (status == DOWNLOAD_OK) {
        task->complete = 1;
    } else if (task->result == TORRENT_OK) {
        task->result = TORRENT_EDOWNLOAD;
        task->detail = status;
    }
    if (task->events.onFinished != NULL) {
        task->events.onFinished(task->privateArg, task, status, message);
    }
    leave(task);
}

/* Choose the files, open them and start everything that finds peers. */
static void startDownload(torrentTask *task)
{
    static const downloadEvents events = { onDownloadReady, onDownloadEvent,
                                           onDownloadFinished, onDownloadComplete };
    downloadOptions         options;
    uint8_t                *wanted = NULL;
    int32_t                 status;
    uint32_t                i;
    struct sockaddr_storage address;

    if (task->options.onlyPiece >= (int64_t)task->metainfo.pieceCount) {
        fail(task, TORRENT_ENOPIECE, 0);
        return;
    }
    if (selectionActive(task->options.choice)) {
        selectionResult result;

        wanted = task->context->allocator.realloc(NULL, task->metainfo.fileCount + 1u);
        if (wanted == NULL) {
            fail(task, TORRENT_ENOMEM, 0);
            return;
        }
        status = selectionApply(task->options.choice, &task->metainfo, wanted, &result);
        if (task->events.onFilesChosen != NULL) {
            task->events.onFilesChosen(task->privateArg, task, status, &result);
        }
        if (status != SELECTION_OK) {
            task->context->allocator.free(wanted);
            fail(task, TORRENT_ESELECTION, status);
            return;
        }
    }

    memset(&options, 0, sizeof(options));
    memcpy(options.peerId, task->host->peerId, PEER_ID_LENGTH);
    options.directory = task->options.directory;
    options.handshakeTimeoutSeconds = task->options.handshakeTimeoutSeconds;
    options.onlyPiece = task->options.onlyPiece;
    options.listenPort = 0;                         /* the session listens for us */
    options.seed = task->options.seed;
    options.infoDictionary = task->data + task->metainfo.infoOffset;
    options.infoDictionaryLength = (uint32_t)task->metainfo.infoLength;
    options.blocklist = task->host->blocklist;
    options.maxPeers = task->options.maxPeers;
    options.uploadSlots = task->options.uploadSlots;
    options.maxDownloadRate = task->options.maxDownloadRate;
    options.maxUploadRate = task->options.maxUploadRate;
    options.noWebSeeds = (uint8_t)!task->options.webSeeds;
    options.fileWanted = wanted;
    if (datafileResumePath(task->metainfo.infoHash, task->resumePath,
                           sizeof(task->resumePath)) == DATAFILE_OK) {
        options.resumePath = task->resumePath;     /* fast resume */
        options.resumeRecheck = task->options.recheck;
    }
    options.http = task->host->http;
    /* Under a session limit, nothing goes until the next tick grants a share. */
    memset(&task->downShare, 0, sizeof(task->downShare));
    memset(&task->upShare, 0, sizeof(task->upShare));
    task->downShare.limited = (uint8_t)(task->host->down != NULL && task->host->down->rate != 0);
    task->upShare.limited = (uint8_t)(task->host->up != NULL && task->host->up->rate != 0);
    options.downShare = &task->downShare;
    options.upShare = &task->upShare;
    options.externalTick = 1;                       /* the session ticks every download */
    options.gate = task->host->gate;

    setState(task, TORRENT_STATE_CHECKING);
    status = downloadBegin(task->loop, task->context, &task->metainfo, &options, &events, task,
                           &task->transfer);
    if (wanted != NULL) {
        task->context->allocator.free(wanted);      /* only read by downloadBegin() */
    }
    if (status != DOWNLOAD_OK) {
        task->transfer = NULL;
        fail(task, TORRENT_EDOWNLOAD, status);
        return;
    }
    for (i = 0; i < task->knownCount; i++) {        /* met while fetching the metadata */
        downloadAddPeer(task->transfer, (const struct sockaddr *)&task->known[i]);
    }
    if (!task->hasLink) {                           /* (a magnet link's were added) */
        for (i = 0; i < task->options.peerCount; i++) {
            if (parsePeerAddress(task->options.peers[i], &address)) {
                downloadAddPeer(task->transfer, (const struct sockaddr *)&address);
            }
        }
    }
    if (task->options.dht && !task->metainfo.isPrivate && task->host->dhtNode != NULL &&
        task->dhtEntry == NULL) {
        if (dhtAddTorrent(task->host->dhtNode, task->metainfo.infoHash, task->host->port,
                          onDhtPeers, onDhtDone, task, &task->dhtEntry) != DHT_OK) {
            task->dhtEntry = NULL;
            notice(task, TORRENT_NOTICE_DHT_FAILED, NULL, 0, 0, 0);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Starting and stopping                                                    */
/* ------------------------------------------------------------------------ */

static void onKick(uv_timer_t *timer)
{
    torrentTask *task = timer->data;

    if (task->closed) {
        return;
    }
    if (task->hasLink && !task->parsed) {
        startMagnet(task);
    } else {
        startDownload(task);
    }
}

/* End every part, once, and report onEnded. */
static void closeAll(torrentTask *task)
{
    if (task->closed) {
        return;
    }
    task->closed = 1;
    endFetch(task);
    leaveDht(task);
    if (task->trackers != NULL) {
        announceEnd(task->trackers);
        task->trackers = NULL;
    }
    if (task->transfer != NULL) {
        downloadEnd(task->transfer);
        task->transfer = NULL;
    }
    if (!uv_is_closing((uv_handle_t *)&task->kick)) {
        uv_close((uv_handle_t *)&task->kick, NULL); /* done before uv_run() returns */
    }
    if (task->result == TORRENT_OK && !task->complete) {
        task->result = TORRENT_ESTOPPED;
    }
    setState(task, TORRENT_STATE_ENDED);
    if (task->host->onTaskEnded != NULL) {
        task->host->onTaskEnded(task->host->hostArg, task);
    }
    if (task->events.onEnded != NULL) {
        task->events.onEnded(task->privateArg, task, task->result, task->detail);
    }
}

int32_t torrentBegin(uv_loop_t *loop, parserContext *context, const torrentHost *host,
                     const torrentOptions *options, const torrentEvents *events,
                     void *privateArg, torrentTask **handle)
{
    torrentTask *task;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (loop == NULL || context == NULL || host == NULL || options == NULL || events == NULL ||
        handle == NULL || (options->data == NULL) == (options->magnet == NULL) ||
        (options->peerCount > 0 && options->peers == NULL)) {
        return TORRENT_EINVAL;
    }
    task = context->allocator.realloc(NULL, sizeof(*task));
    if (task == NULL) {
        return TORRENT_ENOMEM;
    }
    memset(task, 0, sizeof(*task));
    task->loop = loop;
    task->context = context;
    task->host = host;
    task->options = *options;
    task->options.data = NULL;
    task->options.magnet = NULL;
    task->events = *events;
    task->privateArg = privateArg;
    task->state = TORRENT_STATE_STARTING;

    if (options->magnet != NULL) {
        if (magnetParse(context, options->magnet, &task->link) != MAGNET_OK) {
            context->allocator.free(task);
            return TORRENT_EMAGNET;
        }
        task->hasLink = 1;
        memcpy(task->infoHash, task->link.infoHash, PARSER_SHA1_LENGTH);
    } else {
        uint8_t *copy;

        if (options->length == 0 || options->length > (uint64_t)(SIZE_MAX / 2u)) {
            context->allocator.free(task);
            return TORRENT_EPARSE;
        }
        copy = context->allocator.realloc(NULL, (size_t)options->length);
        if (copy == NULL) {
            context->allocator.free(task);
            return TORRENT_ENOMEM;
        }
        memcpy(copy, options->data, (size_t)options->length);
        if (!adoptTorrent(task, copy, options->length)) {
            context->allocator.free(copy);
            context->allocator.free(task);
            return TORRENT_EPARSE;
        }
        memcpy(task->infoHash, task->metainfo.infoHash, PARSER_SHA1_LENGTH);
    }
    peerReq2Hash(task->infoHash, task->req2);

    uv_timer_init(loop, &task->kick);
    task->kick.data = task;
    uv_timer_start(&task->kick, onKick, 0, 0);
    *handle = task;
    return TORRENT_OK;
}

void torrentStop(torrentTask *task)
{
    if (task == NULL || task->closed) {
        return;
    }
    if (task->stopping || task->transfer == NULL) {
        closeAll(task);                             /* at once */
    } else {
        leave(task);
    }
}

uint32_t torrentState(const torrentTask *task)
{
    return task->state;
}

void torrentGetStatus(const torrentTask *task, torrentStatus *status)
{
    memset(status, 0, sizeof(*status));
    status->state = task->state;
    if (task->fetch != NULL) {
        status->hasMetadataProgress = 1;
        metadataGetProgress(task->fetch, &status->metadata);
    }
    if (task->transfer != NULL) {
        status->hasDownloadProgress = 1;
        downloadGetProgress(task->transfer, &status->download);
    }
    if (task->host->dhtNode != NULL) {
        dhtStatus table;

        dhtGetStatus(task->host->dhtNode, &table);
        status->dhtRunning = 1;
        status->dhtNodes = table.nodes;
    }
}

const parserTorrent *torrentMetainfo(const torrentTask *task)
{
    return task->parsed ? &task->metainfo : NULL;
}

download *torrentDownload(const torrentTask *task)
{
    return task->transfer;
}

const uint8_t *torrentInfoHash(const torrentTask *task)
{
    return task->infoHash;
}

const uint8_t *torrentReq2Hash(const torrentTask *task)
{
    return task->req2;
}

void torrentShares(torrentTask *task, bandwidthShare **down, bandwidthShare **up)
{
    *down = &task->downShare;
    *up = &task->upShare;
}

int torrentHasDownload(const torrentTask *task)
{
    return task->transfer != NULL;
}

void torrentTick(torrentTask *task)
{
    if (task->transfer != NULL && !task->closed) {
        downloadTick(task->transfer);
    }
}

int torrentAcceptIncoming(torrentTask *task, peerConnection *connection,
                          const struct sockaddr *address,
                          const uint8_t infoHash[PARSER_SHA1_LENGTH],
                          peerConnectOptions *options, peerCallbacks *callbacks,
                          void **privateArg)
{
    if (task->transfer == NULL || task->closed || task->stopping) {
        return 0;                                   /* not downloading (yet, or any more) */
    }
    return downloadAcceptIncoming(task->transfer, connection, address, infoHash, options,
                                  callbacks, privateArg);
}

void torrentEnd(torrentTask *task)
{
    parserContext *context;

    if (task == NULL) {
        return;
    }
    context = task->context;
    if (task->parsed) {
        parserFree(context, &task->metainfo);
        task->parsed = 0;
    }
    if (task->data != NULL) {
        context->allocator.free(task->data);
        task->data = NULL;
    }
    if (task->hasLink) {
        magnetFree(context, &task->link);
        task->hasLink = 0;
    }
    context->allocator.free(task);
}

const char *torrentStatusString(int32_t status)
{
    switch (status) {
    case TORRENT_OK:            return "complete";
    case TORRENT_EINVAL:        return "invalid argument";
    case TORRENT_ENOMEM:        return "out of memory";
    case TORRENT_EPARSE:        return "the torrent does not parse";
    case TORRENT_EMAGNET:       return "not a usable magnet link";
    case TORRENT_ENOSOURCES:    return "no way to find peers";
    case TORRENT_EMETADATA:     return "could not get the metadata";
    case TORRENT_ENOPIECE:      return "no such piece";
    case TORRENT_ESELECTION:    return "the file choice is wrong";
    case TORRENT_EDOWNLOAD:     return "the download failed";
    case TORRENT_ESTOPPED:      return "stopped";
    default:                    return "unknown torrent status";
    }
}
