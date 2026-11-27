/*
 * check_session.c - sessions and torrent tasks end to end, on loopback.
 *
 * Two sessions in one process: a seed, listening on one port for all its
 * torrents, and a leech. They check:
 *   - a magnet link: metadata, keeping the torrent, then using it next time;
 *   - two torrents served on one port, with plain and with encrypted
 *     connections (routed by info hash, or by SHA1("req2", hash));
 *   - a session-wide download limit shared fairly by two torrents;
 *   - 100 small torrents at once;
 *   - torrents that end early, and the session's own errors.
 *
 * The files left in the data directory (resume files and kept torrents) are
 * named after random info hashes and removed at the end.
 */
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "check.h"
#include "bencode.h"
#include "datafile.h"
#include "peer.h"
#include "platform.h"
#include "session.h"

#define PIECE_LENGTH    16384u
#define SEED_PORT       17411u
#define GIVE_UP_MS      60000u
#define MAX_TORRENTS    100u

typedef struct item {
    uint8_t    *data;                   /* the file                            */
    uint32_t    size;
    uint8_t    *torrent;                /* its .torrent                        */
    uint64_t    length;
    uint8_t     infoHash[PARSER_SHA1_LENGTH];
    char        seedDir[256];
    char        leechDir[256];
    char        magnet[256];
} item;

typedef struct record {
    struct world *w;
    torrentTask  *task;
    uint32_t      states[8];
    uint32_t      stateCount;
    uint32_t      notices;              /* bit (1 << TORRENT_NOTICE_*)         */
    int           ready;
    int           complete;
    int           ended;
    int32_t       result;
    int32_t       detail;
    int           chosen;
    uint64_t      otherReceived;        /* the other torrent's bytes, at our end */
    uint32_t      encrypted;            /* encrypted peers when complete       */
} record;

typedef struct world {
    uv_loop_t      loop;
    parserContext  context;
    uv_timer_t     guard;
    session       *seed;
    session       *leech;
    int            seedEnded;
    int            leechEnded;
    uint16_t       seedPort;
    record         seeds[MAX_TORRENTS];
    record         leeches[MAX_TORRENTS];
    uint32_t       count;               /* torrents in this test               */
    uint32_t       seedsReady;
    uint32_t       leechesComplete;
    struct record *firstComplete;       /* the leech that completed first      */
    int            gaveUp;
    /* What the leech adds once every seed is ready (so that none of its
     * connections arrives while a seed is still checking its files). */
    struct item   *items;
    const char    *const *peer;
    const char    *magnet;              /* or a magnet link for items[0]       */
} world;

static void addLeeches(world *w);

/* ------------------------------------------------------------------------ */
/* Events                                                                   */
/* ------------------------------------------------------------------------ */

static void onState(void *privateArg, torrentTask *task, uint32_t state)
{
    record *r = privateArg;

    (void)task;
    if (r->stateCount < 8) {
        r->states[r->stateCount++] = state;
    }
}

static void onNotice(void *privateArg, torrentTask *task, const torrentNotice *notice)
{
    (void)task;
    ((record *)privateArg)->notices |= 1u << notice->code;
}

static void onFilesChosen(void *privateArg, torrentTask *task, int32_t status,
                          const selectionResult *result)
{
    (void)task;
    (void)status;
    (void)result;
    ((record *)privateArg)->chosen = 1;
}

static void onReady(void *privateArg, torrentTask *task)
{
    record *r = privateArg;
    world  *w = r->w;

    (void)task;
    r->ready = 1;
    if (r >= w->seeds && r < w->seeds + MAX_TORRENTS && ++w->seedsReady == w->count) {
        addLeeches(w);
    }
}

static void stopBoth(world *w)
{
    if (w->leech != NULL) {
        sessionStop(w->leech);
    }
    if (w->seed != NULL) {
        sessionStop(w->seed);
    }
}

static void onComplete(void *privateArg, torrentTask *task)
{
    record          *r = privateArg;
    world           *w = r->w;
    downloadProgress progress;

    if (r < w->leeches || r >= w->leeches + MAX_TORRENTS) {
        return;                                     /* a seed, complete from the start */
    }
    r->complete = 1;
    downloadGetProgress(torrentDownload(task), &progress);
    r->encrypted = progress.peersEncrypted;
    if (w->count == 2) {                            /* how far the other one got */
        record *other = (r == &w->leeches[0]) ? &w->leeches[1] : &w->leeches[0];

        if (torrentDownload(other->task) != NULL) {
            downloadGetProgress(torrentDownload(other->task), &progress);
            r->otherReceived = progress.bytesReceived;
        }
    }
    if (w->firstComplete == NULL) {
        w->firstComplete = r;
    }
    if (++w->leechesComplete == w->count) {
        stopBoth(w);
    }
}

static void onEnded(void *privateArg, torrentTask *task, int32_t result, int32_t detail)
{
    record *r = privateArg;

    (void)task;
    r->ended = 1;
    r->result = result;
    r->detail = detail;
}

/* With CHECK_VERBOSE set, what the engine reports, for debugging. */
static void onEvent(void *privateArg, torrentTask *task, const eventInfo *event)
{
    record *r = privateArg;

    (void)task;
    if (getenv("CHECK_VERBOSE") != NULL) {
        fprintf(stderr, "    %6.2f %s %u: %s\n", (double)(uv_hrtime() % 1000000000000ull) / 1e9,
                (r >= r->w->leeches && r < r->w->leeches + MAX_TORRENTS) ? "leech" : "seed",
                (unsigned)((r >= r->w->leeches && r < r->w->leeches + MAX_TORRENTS)
                               ? (uint32_t)(r - r->w->leeches) : (uint32_t)(r - r->w->seeds)),
                event->message);
    }
}

static const torrentEvents events = {
    onState, onNotice, onEvent, NULL, NULL, onFilesChosen, onReady, onComplete, NULL, onEnded
};

static void onSessionNotice(void *privateArg, session *owner, const sessionNotice *notice)
{
    world *w = privateArg;

    if (owner == w->seed && notice->code == SESSION_NOTICE_LISTENING) {
        w->seedPort = (uint16_t)notice->number;
    }
}

static void onSessionEnded(void *privateArg, session *owner)
{
    world *w = privateArg;

    if (owner == w->seed) {
        w->seedEnded = 1;
    } else {
        w->leechEnded = 1;
    }
    if ((w->seed == NULL || w->seedEnded) && (w->leech == NULL || w->leechEnded)) {
        uv_timer_stop(&w->guard);
    }
}

static const sessionEvents sessionCallbacks = { onSessionNotice, NULL, onSessionEnded };

static void onGuard(uv_timer_t *timer)
{
    world *w = timer->data;

    w->gaveUp = 1;
    stopBoth(w);
    stopBoth(w);
}

/* ------------------------------------------------------------------------ */
/* Worlds, torrents and files                                               */
/* ------------------------------------------------------------------------ */

static void newWorld(world *w)
{
    memset(w, 0, sizeof(*w));
    CHECK(uv_loop_init(&w->loop) == 0);
    CHECK(parserInit(&w->context, &checkAllocator) == PARSER_OK);
    uv_timer_init(&w->loop, &w->guard);
    w->guard.data = w;
    uv_timer_start(&w->guard, onGuard, GIVE_UP_MS, 0);
}

static void sessionDefaults(sessionOptions *o, char id, uint16_t port)
{
    memset(o, 0, sizeof(*o));
    memcpy(o->peerId, "-CK0001-000000000000", PEER_ID_LENGTH);
    o->peerId[19] = (uint8_t)id;
    o->listenPort = port;
}

static void startSessions(world *w, uint64_t leechLimit, int withSeed)
{
    sessionOptions o;

    if (withSeed) {
        sessionDefaults(&o, 'S', SEED_PORT);
        CHECK(sessionBegin(&w->loop, &w->context, &o, &sessionCallbacks, w, &w->seed) ==
              SESSION_OK);
    }
    sessionDefaults(&o, 'L', 0);
    o.maxDownloadRate = leechLimit;
    CHECK(sessionBegin(&w->loop, &w->context, &o, &sessionCallbacks, w, &w->leech) ==
          SESSION_OK);
}

/* Run until both sessions have ended, then release everything. */
static void runWorld(world *w)
{
    uv_run(&w->loop, UV_RUN_DEFAULT);
    uv_close((uv_handle_t *)&w->guard, NULL);
    uv_run(&w->loop, UV_RUN_DEFAULT);
    sessionEnd(w->seed);
    sessionEnd(w->leech);
    CHECK(uv_loop_close(&w->loop) == 0);
}

static void baseTorrent(torrentOptions *o, const char *directory)
{
    memset(o, 0, sizeof(*o));
    o->directory = directory;
    o->handshakeTimeoutSeconds = 5;
    o->onlyPiece = -1;
    o->webSeeds = 1;
}

/* A single-file torrent for it->data. */
static int buildTorrent(item *it)
{
    bencodeWriter info;
    bencodeWriter whole;
    uint32_t      pieces = (it->size + PIECE_LENGTH - 1u) / PIECE_LENGTH;
    uint32_t      i;

    bencodeWriterInit(&info, &checkAllocator);
    bencodeDictBegin(&info);
    bencodeText(&info, "length");
    bencodeUnsigned(&info, it->size);
    bencodeText(&info, "name");
    bencodeText(&info, "data.bin");
    bencodeText(&info, "piece length");
    bencodeUnsigned(&info, PIECE_LENGTH);
    bencodeText(&info, "pieces");
    bencodeStringHeader(&info, (uint64_t)pieces * PARSER_SHA1_LENGTH);
    for (i = 0; i < pieces; i++) {
        uint8_t  digest[PARSER_SHA1_LENGTH];
        uint32_t start = i * PIECE_LENGTH;
        uint32_t piece = (it->size - start < PIECE_LENGTH) ? it->size - start : PIECE_LENGTH;

        parserSha1(it->data + start, piece, digest);
        bencodeRaw(&info, digest, sizeof(digest));
    }
    bencodeEnd(&info);
    if (info.error != BENCODE_OK) {
        bencodeWriterFree(&info);
        return 0;
    }
    parserSha1(info.data, info.length, it->infoHash);
    bencodeWriterInit(&whole, &checkAllocator);
    bencodeDictBegin(&whole);
    bencodeText(&whole, "info");
    bencodeRaw(&whole, info.data, info.length);
    bencodeEnd(&whole);
    bencodeWriterFree(&info);
    return bencodeWriterTake(&whole, &it->torrent, &it->length) == BENCODE_OK;
}

/* Random data, its torrent, a seed folder holding the file and an empty
 * leech folder: "<test>/<n>/seed" and "<test>/<n>/leech". */
static int makeItem(item *it, const char *test, uint32_t n, uint32_t size)
{
    char     base[200];
    char     path[300];
    uint32_t i;
    int      p;

    memset(it, 0, sizeof(*it));
    it->size = size;
    it->data = checkAllocator.realloc(NULL, size);
    if (it->data == NULL) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        it->data[i] = (uint8_t)checkRandom();
    }
    if (uv_random(NULL, NULL, it->data, (size < 16u) ? size : 16u, 0, NULL) != 0) {
        it->data[0] ^= (uint8_t)n;                  /* a new info hash each run */
    }
    if (!buildTorrent(it)) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s-%u", test, (unsigned)n);
    checkScratchPath(path, base, sizeof(base));
    (void)platformMakeDirectory(base);
    snprintf(it->seedDir, sizeof(it->seedDir), "%s%cseed", base, PLATFORM_SEPARATOR);
    snprintf(it->leechDir, sizeof(it->leechDir), "%s%cleech", base, PLATFORM_SEPARATOR);
    (void)platformMakeDirectory(it->seedDir);
    (void)platformMakeDirectory(it->leechDir);
    snprintf(path, sizeof(path), "%s%cdata.bin", it->seedDir, PLATFORM_SEPARATOR);
    if (datafileWrite(path, it->data, size, NULL) != DATAFILE_OK) {
        return 0;
    }
    p = snprintf(it->magnet, sizeof(it->magnet), "magnet:?xt=urn:btih:");
    for (i = 0; i < PARSER_SHA1_LENGTH; i++) {
        p += snprintf(it->magnet + p, sizeof(it->magnet) - (size_t)p, "%02x", it->infoHash[i]);
    }
    return 1;
}

static void freeItem(item *it)
{
    char path[1024];

    if (datafileTorrentPath(it->infoHash, path, sizeof(path)) == DATAFILE_OK) {
        (void)platformFileRemove(path);
    }
    if (datafileResumePath(it->infoHash, path, sizeof(path)) == DATAFILE_OK) {
        (void)platformFileRemove(path);
    }
    checkAllocator.free(it->data);
    checkAllocator.free(it->torrent);
}

/* Remove what earlier tests downloaded, so every test starts empty. */
static void resetLeeches(const item *items, uint32_t count)
{
    char     path[300];
    uint32_t i;

    for (i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s%cdata.bin", items[i].leechDir, PLATFORM_SEPARATOR);
        (void)platformFileRemove(path);
    }
}

static int downloaded(const item *it)
{
    char     path[300];
    uint8_t *read = NULL;
    uint64_t length = 0;
    int      same;

    snprintf(path, sizeof(path), "%s%cdata.bin", it->leechDir, PLATFORM_SEPARATOR);
    if (datafileRead(&checkAllocator, path, it->size, &read, &length, NULL) != DATAFILE_OK) {
        return 0;
    }
    same = length == it->size && memcmp(read, it->data, it->size) == 0;
    checkAllocator.free(read);
    return same;
}

/* Seed every item from the seed session; once all are ready (addLeeches),
 * download each from the leech session, told the seed's address. */
static void addAll(world *w, item *items, uint32_t count, const char *const *peer)
{
    torrentOptions o;
    uint32_t       i;

    w->count = count;
    w->items = items;
    w->peer = peer;
    for (i = 0; i < count; i++) {
        w->seeds[i].w = w;
        w->leeches[i].w = w;
        baseTorrent(&o, items[i].seedDir);
        o.data = items[i].torrent;
        o.length = items[i].length;
        o.seed = 1;
        CHECK(sessionAddTorrent(w->seed, &o, &events, &w->seeds[i], &w->seeds[i].task) ==
              SESSION_OK);
    }
}

static void addLeeches(world *w)
{
    torrentOptions o;
    uint32_t       i;

    for (i = 0; i < w->count; i++) {
        baseTorrent(&o, w->items[i].leechDir);
        if (w->magnet != NULL) {
            o.magnet = w->magnet;
        } else {
            o.data = w->items[i].torrent;
            o.length = w->items[i].length;
            o.peers = w->peer;
            o.peerCount = 1;
        }
        CHECK(sessionAddTorrent(w->leech, &o, &events, &w->leeches[i], &w->leeches[i].task) ==
              SESSION_OK);
    }
}

/* ------------------------------------------------------------------------ */
/* The tests                                                                */
/* ------------------------------------------------------------------------ */

static const char *const seedPeer[] = { "127.0.0.1:17411" };

/* A magnet link naming the seed: metadata, then the download; then again,
 * using the torrent kept the first time. */
static void magnetLink(item *it)
{
    world          w;
    torrentOptions o;
    char           magnet[300];

    resetLeeches(it, 1);
    newWorld(&w);
    startSessions(&w, 0, 1);
    snprintf(magnet, sizeof(magnet), "%s&x.pe=127.0.0.1:%u", it->magnet, SEED_PORT);
    w.magnet = magnet;
    addAll(&w, it, 1, NULL);                        /* the leech: from the link */
    runWorld(&w);
    CHECK(!w.gaveUp && w.seedPort == SEED_PORT);
    CHECK(w.leeches[0].result == TORRENT_OK && downloaded(it));
    CHECK(w.leeches[0].stateCount >= 4 && w.leeches[0].states[0] == TORRENT_STATE_METADATA &&
          w.leeches[0].states[1] == TORRENT_STATE_CHECKING &&
          w.leeches[0].states[2] == TORRENT_STATE_RUNNING);
    CHECK(w.leeches[0].notices & (1u << TORRENT_NOTICE_METADATA));
    CHECK(w.leeches[0].notices & (1u << TORRENT_NOTICE_SAVED));
    CHECK(w.seeds[0].result == TORRENT_OK);         /* complete, then stopped */

    /* Again: the kept torrent, no metadata phase, nothing left to do. */
    newWorld(&w);
    startSessions(&w, 0, 0);
    w.count = 1;
    w.leeches[0].w = &w;
    baseTorrent(&o, it->leechDir);
    o.magnet = magnet;
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[0], &w.leeches[0].task) ==
          SESSION_OK);
    uv_run(&w.loop, UV_RUN_NOWAIT);
    uv_run(&w.loop, UV_RUN_NOWAIT);
    while (!w.leeches[0].ended) {
        uv_run(&w.loop, UV_RUN_ONCE);
    }
    sessionStop(w.leech);
    runWorld(&w);
    CHECK(w.leeches[0].notices & (1u << TORRENT_NOTICE_CACHED));
    CHECK(w.leeches[0].states[0] == TORRENT_STATE_CHECKING);
    CHECK(w.leeches[0].result == TORRENT_OK);
}

/* Two torrents served on one port, plain or encrypted. */
static void onePort(item *items, uint8_t encryption)
{
    world w;

    peerSetEncryption(encryption);
    resetLeeches(items, 2);
    newWorld(&w);
    startSessions(&w, 0, 1);
    addAll(&w, items, 2, seedPeer);
    runWorld(&w);
    CHECK(!w.gaveUp);
    CHECK(w.leeches[0].result == TORRENT_OK && w.leeches[1].result == TORRENT_OK);
    CHECK(downloaded(&items[0]) && downloaded(&items[1]));
    if (encryption == PEER_ENCRYPTION_REQUIRE) {
        CHECK(w.leeches[0].encrypted > 0 && w.leeches[1].encrypted > 0);
    } else {
        CHECK(w.leeches[0].encrypted == 0 && w.leeches[1].encrypted == 0);
    }
    peerSetEncryption(PEER_ENCRYPTION_PREFER);
}

/* A session-wide limit, shared by two torrents of the same size: when the
 * first is complete the other must be most of the way, and the whole must
 * take about as long as the limit says. */
static void fairLimit(item *items)
{
    world    w;
    uint64_t started;
    double   seconds;
    uint64_t limit = 1024u * 1024u;

    resetLeeches(items, 2);
    newWorld(&w);
    startSessions(&w, limit, 1);
    addAll(&w, items, 2, seedPeer);
    started = uv_hrtime();
    runWorld(&w);
    seconds = (double)(uv_hrtime() - started) / 1e9;
    CHECK(!w.gaveUp && w.leechesComplete == 2);
    CHECK(downloaded(&items[0]) && downloaded(&items[1]));
    /* 4 MiB at 1 MiB/s, less the half-second burst. */
    CHECK(seconds >= 3.0);
    /* The first to finish: the other had at least 3/4 of its data. */
    {
        record *first = (w.firstComplete != NULL) ? w.firstComplete : &w.leeches[0];

        CHECK((double)first->otherReceived >= 0.75 * items[0].size);
    }
}

/* 100 small torrents on one port, all at once. */
static void manyTorrents(item *items, uint32_t count)
{
    world    w;
    uint32_t i;
    uint32_t good = 0;

    resetLeeches(items, count);
    newWorld(&w);
    startSessions(&w, 0, 1);
    addAll(&w, items, count, seedPeer);
    runWorld(&w);
    CHECK(!w.gaveUp);
    for (i = 0; i < count; i++) {
        good += (w.leeches[i].result == TORRENT_OK && downloaded(&items[i])) ? 1u : 0u;
    }
    CHECK(good == count);
}

/* Torrents that end before downloading anything, and the session's own
 * errors. */
static void endsEarly(item *it)
{
    world          w;
    torrentOptions o;
    selection      choice;
    char           dir[256];
    torrentTask   *task = NULL;

    checkScratchPath("early", dir, sizeof(dir));
    newWorld(&w);
    startSessions(&w, 0, 0);
    w.count = 4;

    /* No trackers, no peers, no DHT. */
    w.leeches[0].w = &w;
    baseTorrent(&o, dir);
    o.magnet = "magnet:?xt=urn:btih:00000000000000000000000000000000000000ee";
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[0], &w.leeches[0].task) ==
          SESSION_OK);
    /* A peer that is not there: stopped while fetching the metadata. */
    w.leeches[1].w = &w;
    o.magnet = "magnet:?xt=urn:btih:00000000000000000000000000000000000000ef"
               "&x.pe=127.0.0.1:9";
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[1], &w.leeches[1].task) ==
          SESSION_OK);
    /* A piece the torrent does not have. */
    w.leeches[2].w = &w;
    o.magnet = NULL;
    o.data = it->torrent;
    o.length = it->length;
    o.onlyPiece = 99999;
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[2], &w.leeches[2].task) ==
          SESSION_OK);
    /* A file choice that chooses nothing. */
    w.leeches[3].w = &w;
    o.onlyPiece = -1;
    selectionInit(&choice);
    CHECK(selectionAddPattern(&choice, "*.none") == SELECTION_OK);
    o.choice = &choice;
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[3], &w.leeches[3].task) ==
          SESSION_OK);
    /* Refused at once. */
    o.choice = NULL;
    o.data = (const uint8_t *)"not bencode";
    o.length = 11;
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[4], &task) == TORRENT_EPARSE &&
          task == NULL);
    o.data = NULL;
    o.magnet = "magnet:?dn=no-hash";
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[4], &task) == TORRENT_EMAGNET);
    o.data = it->torrent;
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[4], &task) == TORRENT_EINVAL);

    while (!(w.leeches[0].ended && w.leeches[2].ended && w.leeches[3].ended &&
             torrentState(w.leeches[1].task) == TORRENT_STATE_METADATA)) {
        uv_run(&w.loop, UV_RUN_ONCE);
    }
    sessionStop(w.leech);                           /* the metadata fetch stops at once */
    o.magnet = NULL;
    CHECK(sessionAddTorrent(w.leech, &o, &events, &w.leeches[4], &task) == SESSION_ESTOPPING);
    runWorld(&w);
    CHECK(w.leeches[0].result == TORRENT_ENOSOURCES);
    CHECK(w.leeches[1].result == TORRENT_ESTOPPED);
    CHECK(w.leeches[2].result == TORRENT_ENOPIECE);
    CHECK(w.leeches[3].chosen && w.leeches[3].result == TORRENT_ESELECTION &&
          w.leeches[3].detail == SELECTION_ENONE);

    /* A session with no torrents stops at once (from the loop). */
    newWorld(&w);
    startSessions(&w, 0, 0);
    sessionStop(w.leech);
    CHECK(!w.leechEnded);                           /* reported from the loop */
    runWorld(&w);
    CHECK(w.leechEnded);
}

void checkSession(void)
{
    static item items[MAX_TORRENTS];
    uint32_t    i;
    uint32_t    made = 0;

    /* 0, 1: 2 MiB each (one port, the limit); 2..: 20 KiB each (many). */
    for (i = 0; i < MAX_TORRENTS; i++) {
        uint32_t size = (i < 2) ? 2u * 1024u * 1024u + 4321u * i : 20u * 1024u + i;

        if (!makeItem(&items[i], "session", i, size)) {
            break;
        }
        made++;
    }
    CHECK(made == MAX_TORRENTS);
    if (made == MAX_TORRENTS) {
        uint64_t at = uv_hrtime();

#define TIMED(call)                                                                   \
        do {                                                                          \
            call;                                                                     \
            if (getenv("CHECK_VERBOSE") != NULL) {                                    \
                fprintf(stderr, "  %-45s %.1f s\n", #call, (double)(uv_hrtime() - at) / 1e9); \
            }                                                                         \
            at = uv_hrtime();                                                         \
        } while (0)
        TIMED(magnetLink(&items[2]));
        TIMED(onePort(items, PEER_ENCRYPTION_OFF));
        TIMED(onePort(items, PEER_ENCRYPTION_REQUIRE));
        TIMED(fairLimit(items));
        TIMED(manyTorrents(items + 2, MAX_TORRENTS - 2u));
        TIMED(endsEarly(&items[3]));
#undef TIMED
    }
    for (i = 0; i < made; i++) {
        freeItem(&items[i]);
    }
}
