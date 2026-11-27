/*
 * dht.c - The BitTorrent DHT (BEP 5). See dht.h.
 *
 * Routing table: 160 buckets of up to 8 nodes, bucket i holding nodes whose
 * ids share exactly i leading bits with ours. A node is added when it
 * answers us or queries us; a full bucket takes a newcomer only in place of
 * a node that keeps failing (or that never answered, for one that did).
 *
 * Lookups: up to 64 candidates sorted by distance to the target; queries go
 * to the closest not yet asked, 4 at a time, among the 8 closest that have
 * not failed; the lookup ends when those 8 have all answered (or failed).
 * A get_peers lookup then announces us to the 8 closest that gave a token.
 *
 * Tokens for announce_peer are the first 8 bytes of SHA-1(secret | IP), the
 * secret changing every 5 minutes (the previous one is still accepted).
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dht.h"
#include "bencode.h"
#include "queue.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif

#define DHT_BUCKETS             160u
#define DHT_ALPHA               4u      /* queries in flight per lookup       */
#define DHT_CANDIDATES          64u     /* per lookup                         */
#define DHT_MAX_PENDING         256u
#define DHT_MAX_LOOKUPS         16u
#define DHT_MAX_BOOTSTRAP       16u
#define DHT_MAX_STORED          256u    /* info hashes we store peers for     */
#define DHT_STORED_PEERS        64u     /* ...and peers each                  */
#define DHT_STORE_SECONDS       1800u
#define DHT_TOKEN_SECONDS       300u
#define DHT_QUESTIONABLE_MS     (15u * 60u * 1000u)
#define DHT_MAX_FAILURES        3u
#define DHT_PACKET              1500u
#define DHT_TICK_MS             500u
#define DHT_PORT_ATTEMPTS       10u
#define DHT_SEEN_PEERS          256u    /* per lookup, to report each once    */
#define DHT_MAX_VALUES          50u     /* peers in one get_peers answer      */
#define DHT_VERSION             "TI08"

#define QUERY_PING              1u
#define QUERY_FIND_NODE         2u
#define QUERY_GET_PEERS         3u
#define QUERY_ANNOUNCE          4u

#define CANDIDATE_NEW           0u
#define CANDIDATE_ASKED         1u
#define CANDIDATE_REPLIED       2u
#define CANDIDATE_FAILED        3u

typedef struct dhtNode {
    uint8_t             id[DHT_ID_LENGTH];
    struct sockaddr_in  address;
    uint64_t            lastReply;          /* uv_now(); 0: never answered us */
    uint64_t            lastPing;
    uint8_t             failures;
    uint8_t             used;
} dhtNode;

typedef struct candidate {
    uint8_t             id[DHT_ID_LENGTH];
    uint8_t             idKnown;            /* bootstrap routers: not yet     */
    uint8_t             state;
    uint8_t             tokenLength;
    uint8_t             token[32];
    struct sockaddr_in  address;
} candidate;

struct dhtTorrent;

typedef struct lookup {
    uint8_t             target[DHT_ID_LENGTH];
    uint32_t            kind;               /* QUERY_FIND_NODE or _GET_PEERS  */
    struct dhtTorrent  *torrent;            /* for get_peers                  */
    candidate           candidates[DHT_CANDIDATES];
    uint32_t            count;
    uint32_t            inFlight;
    uint32_t            asked;
    uint32_t            peersFound;
    uint32_t            seen[DHT_SEEN_PEERS * 2u];  /* ip, port of peers reported */
    uint32_t            seenCount;
} lookup;

struct dhtTorrent {
    TAILQ_ENTRY(dhtTorrent) entries;
    uint8_t             infoHash[PARSER_SHA1_LENGTH];
    uint16_t            announcePort;
    dhtPeersCallback    callback;
    dhtSearchDoneCallback done;
    void               *privateArg;
    uint64_t            nextSearch;
    lookup             *running;
};

TAILQ_HEAD(dhtTorrentList, dhtTorrent);

typedef struct pendingQuery {
    uint8_t             used;
    uint8_t             kind;
    uint16_t            tid;
    struct sockaddr_in  to;
    uint64_t            sentAt;
    lookup             *owner;              /* NULL for ping and announce     */
} pendingQuery;

typedef struct storedPeer {
    uint8_t             ip[4];
    uint16_t            port;
    uint64_t            at;
} storedPeer;

typedef struct storedHash {
    uint8_t             infoHash[PARSER_SHA1_LENGTH];
    storedPeer          peers[DHT_STORED_PEERS];
    uint32_t            count;
    uint64_t            lastUsed;
} storedHash;

typedef struct resolveRequest {
    uv_getaddrinfo_t    request;
    struct dht         *owner;
    uint16_t            port;
} resolveRequest;

struct dht {
    uv_loop_t          *loop;
    parserContext      *context;
    netBinding          binding;
    uv_udp_t            udp;
    uv_timer_t          timer;
    uint32_t            openHandles;
    uint32_t            resolving;          /* DNS requests outstanding       */
    uint8_t             ended;
    const blocklist    *blocklist;
    uint64_t            blocked;            /* by the blocklist               */
    uint16_t            port;

    uint8_t             id[DHT_ID_LENGTH];
    dhtNode            *table;              /* DHT_BUCKETS * DHT_BUCKET_SIZE  */
    pendingQuery        pending[DHT_MAX_PENDING];
    uint16_t            nextTid;
    lookup             *lookups[DHT_MAX_LOOKUPS];
    struct dhtTorrentList torrents;
    struct sockaddr_in  bootstrap[DHT_MAX_BOOTSTRAP];
    uint32_t            bootstrapCount;
    uint64_t            lastBootstrap;
    uint64_t            lastMaintenance;

    uint8_t             secrets[2][16];     /* current, previous              */
    uint64_t            lastRotate;
    storedHash         *stored;             /* DHT_MAX_STORED, on first use   */

    uint64_t            received;
    uint64_t            sent;
    uint64_t            randomState;
    char                receiveBuffer[DHT_PACKET + 1u];
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static void *allocate(dht *d, size_t size)
{
    void *memory = d->context->allocator.realloc(NULL, size);

    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static void release(dht *d, void *memory)
{
    if (memory != NULL) {
        d->context->allocator.free(memory);
    }
}

static uint64_t nextRandom(dht *d)
{
    uint64_t x = d->randomState;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    d->randomState = x;
    return x;
}

static void randomBytes(dht *d, uint8_t *out, size_t length)
{
    if (uv_random(NULL, NULL, out, length, 0, NULL) != 0) {
        size_t i;

        for (i = 0; i < length; i++) {
            out[i] = (uint8_t)nextRandom(d);
        }
    }
}

/* Leading bits two ids share (160 if equal). */
static uint32_t commonBits(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    uint32_t bits = 0;

    for (i = 0; i < DHT_ID_LENGTH; i++) {
        uint8_t x = (uint8_t)(a[i] ^ b[i]);

        if (x == 0) {
            bits += 8;
            continue;
        }
        while (!(x & 0x80u)) {
            bits++;
            x = (uint8_t)(x << 1);
        }
        return bits;
    }
    return bits;
}

/* <0 if a is closer to target than b, >0 if farther, 0 if equally close. */
static int compareDistance(const uint8_t *target, const uint8_t *a, const uint8_t *b)
{
    uint32_t i;

    for (i = 0; i < DHT_ID_LENGTH; i++) {
        uint8_t x = (uint8_t)(a[i] ^ target[i]);
        uint8_t y = (uint8_t)(b[i] ^ target[i]);

        if (x != y) {
            return (x < y) ? -1 : 1;
        }
    }
    return 0;
}

static int sameEndpoint(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* Addresses no one should send to or report. */
static int usableAddress(const struct sockaddr_in *address)
{
    uint32_t ip = ntohl(address->sin_addr.s_addr);

    return address->sin_port != 0 && ip != 0 && ip != 0xffffffffu && (ip >> 28) != 0xEu;
}

static void compactAddress(const struct sockaddr_in *address, uint8_t out[6])
{
    memcpy(out, &address->sin_addr, 4);
    memcpy(out + 4, &address->sin_port, 2);         /* both in network order */
}

static void fromCompact(const uint8_t in[6], struct sockaddr_in *address)
{
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    memcpy(&address->sin_addr, in, 4);
    memcpy(&address->sin_port, in + 4, 2);
}

/* ------------------------------------------------------------------------ */
/* Sending                                                                  */
/* ------------------------------------------------------------------------ */

static void sendPacket(dht *d, const struct sockaddr_in *to, const uint8_t *data, size_t length)
{
    uv_buf_t buffer = uv_buf_init((char *)data, (unsigned int)length);

    if (d->ended || !usableAddress(to)) {
        return;
    }
    if (uv_udp_try_send(&d->udp, &buffer, 1, (const struct sockaddr *)to) >= 0) {
        d->sent++;
    }
}

/* Start a query: returns 0 if sent (a pending slot is used). */
static int sendQuery(dht *d, const struct sockaddr_in *to, uint32_t kind, lookup *owner,
                     const uint8_t *target, const uint8_t *token, size_t tokenLength,
                     uint16_t port)
{
    uint8_t  packet[DHT_PACKET];
    bencodeWriter w;
    uint8_t  tid[2];
    uint32_t slot;

    bencodeWriterInitFixed(&w, packet, sizeof(packet));
    if (!usableAddress(to)) {
        return -1;
    }
    for (slot = 0; slot < DHT_MAX_PENDING && d->pending[slot].used; slot++) {
    }
    if (slot == DHT_MAX_PENDING) {
        return -1;                                  /* busy: try later */
    }
    d->nextTid++;
    tid[0] = (uint8_t)(d->nextTid >> 8);
    tid[1] = (uint8_t)(d->nextTid & 0xffu);

    /* Keys sorted: a, q, t, v, y; and within a. */
    bencodeDictBegin(&w);
    bencodeText(&w, "a");
    bencodeDictBegin(&w);
    bencodeText(&w, "id");
    bencodeString(&w, d->id, DHT_ID_LENGTH);
    if (kind == QUERY_ANNOUNCE) {
        bencodeText(&w, "implied_port");
        bencodeInteger(&w, 0);
        bencodeText(&w, "info_hash");
        bencodeString(&w, target, DHT_ID_LENGTH);
        bencodeText(&w, "port");
        bencodeInteger(&w, port);
        bencodeText(&w, "token");
        bencodeString(&w, token, tokenLength);
    } else if (kind == QUERY_GET_PEERS) {
        bencodeText(&w, "info_hash");
        bencodeString(&w, target, DHT_ID_LENGTH);
    } else if (kind == QUERY_FIND_NODE) {
        bencodeText(&w, "target");
        bencodeString(&w, target, DHT_ID_LENGTH);
    }
    bencodeEnd(&w);
    bencodeText(&w, "q");
    bencodeText(&w, (kind == QUERY_PING)        ? "ping"
                  : (kind == QUERY_FIND_NODE)   ? "find_node"
                  : (kind == QUERY_GET_PEERS)   ? "get_peers"
                                                : "announce_peer");
    bencodeText(&w, "t");
    bencodeString(&w, tid, 2);
    bencodeText(&w, "v");
    bencodeString(&w, DHT_VERSION, 4);
    bencodeText(&w, "y");
    bencodeText(&w, "q");
    bencodeEnd(&w);
    if (w.error != BENCODE_OK) {
        return -1;
    }
    d->pending[slot].used = 1;
    d->pending[slot].kind = (uint8_t)kind;
    d->pending[slot].tid = d->nextTid;
    d->pending[slot].to = *to;
    d->pending[slot].sentAt = uv_now(d->loop);
    d->pending[slot].owner = owner;
    sendPacket(d, to, packet, (size_t)w.length);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Routing table                                                            */
/* ------------------------------------------------------------------------ */

static dhtNode *bucketOf(dht *d, const uint8_t *id)
{
    uint32_t bits = commonBits(id, d->id);

    return &d->table[(size_t)((bits >= DHT_BUCKETS) ? DHT_BUCKETS - 1u : bits) * DHT_BUCKET_SIZE];
}

/* A node answered us (confirmed) or contacted us. */
static void addNode(dht *d, const uint8_t *id, const struct sockaddr_in *address, int confirmed)
{
    dhtNode *bucket;
    dhtNode *slot = NULL;
    uint64_t now = uv_now(d->loop);
    uint32_t i;

    if (memcmp(id, d->id, DHT_ID_LENGTH) == 0 || !usableAddress(address)) {
        return;
    }
    if (blocklistContains(d->blocklist, (const struct sockaddr *)address)) {
        d->blocked++;
        return;
    }
    bucket = bucketOf(d, id);
    for (i = 0; i < DHT_BUCKET_SIZE; i++) {
        if (bucket[i].used && memcmp(bucket[i].id, id, DHT_ID_LENGTH) == 0) {
            bucket[i].address = *address;
            if (confirmed) {
                bucket[i].lastReply = now;
                bucket[i].failures = 0;
            }
            return;
        }
    }
    for (i = 0; i < DHT_BUCKET_SIZE && slot == NULL; i++) {
        if (!bucket[i].used) {
            slot = &bucket[i];
        }
    }
    for (i = 0; i < DHT_BUCKET_SIZE && slot == NULL; i++) {
        if (bucket[i].failures >= 2u || (confirmed && bucket[i].lastReply == 0)) {
            slot = &bucket[i];                      /* a worse node makes way */
        }
    }
    if (slot == NULL) {
        return;                                     /* full of good nodes */
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    memcpy(slot->id, id, DHT_ID_LENGTH);
    slot->address = *address;
    slot->lastReply = confirmed ? now : 0u;
    if (!confirmed) {
        /* It contacted us (or came from a saved table): check it answers. */
        slot->lastPing = now;
        (void)sendQuery(d, address, QUERY_PING, NULL, NULL, NULL, 0, 0);
    }
}

static void nodeFailed(dht *d, const struct sockaddr_in *address)
{
    uint32_t i;

    for (i = 0; i < DHT_BUCKETS * DHT_BUCKET_SIZE; i++) {
        dhtNode *node = &d->table[i];

        if (node->used && sameEndpoint(&node->address, address)) {
            if (++node->failures >= DHT_MAX_FAILURES) {
                node->used = 0;                     /* gone */
            }
            return;
        }
    }
}

/* Up to 'max' table nodes closest to 'target' (not 'except'), closest first. */
static uint32_t closestNodes(dht *d, const uint8_t *target, const struct sockaddr_in *except,
                             dhtNode **out, uint32_t max)
{
    uint32_t count = 0;
    uint32_t i;
    uint32_t j;

    for (i = 0; i < DHT_BUCKETS * DHT_BUCKET_SIZE; i++) {
        dhtNode *node = &d->table[i];

        if (!node->used || node->failures >= 2u ||
            (except != NULL && sameEndpoint(&node->address, except))) {
            continue;
        }
        for (j = count; j > 0 && compareDistance(target, node->id, out[j - 1u]->id) < 0; j--) {
            if (j < max) {
                out[j] = out[j - 1u];
            }
        }
        if (j < max) {
            out[j] = node;
            if (count < max) {
                count++;
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------------------ */
/* Lookups                                                                  */
/* ------------------------------------------------------------------------ */

/* Add a candidate, keeping the list sorted (unknown ids last). */
static void addCandidate(lookup *lk, const uint8_t *id, const struct sockaddr_in *address)
{
    candidate entry;
    uint32_t  i;
    uint32_t  at;

    if (!usableAddress(address)) {
        return;
    }
    for (i = 0; i < lk->count; i++) {
        if (sameEndpoint(&lk->candidates[i].address, address) ||
            (id != NULL && lk->candidates[i].idKnown &&
             memcmp(lk->candidates[i].id, id, DHT_ID_LENGTH) == 0)) {
            return;
        }
    }
    memset(&entry, 0, sizeof(entry));
    if (id != NULL) {
        memcpy(entry.id, id, DHT_ID_LENGTH);
        entry.idKnown = 1;
    }
    entry.address = *address;
    for (at = 0; at < lk->count; at++) {
        const candidate *other = &lk->candidates[at];

        if (id != NULL && (!other->idKnown || compareDistance(lk->target, id, other->id) < 0)) {
            break;
        }
    }
    if (at >= DHT_CANDIDATES) {
        return;                                     /* farther than all 64 */
    }
    if (lk->count == DHT_CANDIDATES) {
        if (lk->candidates[DHT_CANDIDATES - 1u].state == CANDIDATE_ASKED) {
            return;                                 /* keep what is in flight */
        }
        lk->count--;
    }
    memmove(&lk->candidates[at + 1u], &lk->candidates[at],
            sizeof(candidate) * (lk->count - at));
    lk->candidates[at] = entry;
    lk->count++;
}

static void finishLookup(dht *d, lookup *lk);

/* Send queries; finish when the 8 closest have all answered or failed. */
static void stepLookup(dht *d, lookup *lk)
{
    uint32_t considered = 0;
    uint32_t i;
    int      waiting = 0;

    if (d->ended) {
        return;
    }
    for (i = 0; i < lk->count && considered < DHT_BUCKET_SIZE; i++) {
        candidate *c = &lk->candidates[i];

        if (c->state == CANDIDATE_FAILED) {
            continue;
        }
        considered++;
        if (c->state == CANDIDATE_NEW) {
            if (lk->inFlight < DHT_ALPHA &&
                sendQuery(d, &c->address, lk->kind, lk, lk->target, NULL, 0, 0) == 0) {
                c->state = CANDIDATE_ASKED;
                lk->inFlight++;
                lk->asked++;
            } else {
                waiting = 1;
            }
        }
    }
    if (lk->inFlight == 0 && !waiting) {
        finishLookup(d, lk);
    }
}

/* Start a lookup (it may finish at once). -1 if there is no room for one. */
static int startLookup(dht *d, const uint8_t *target, uint32_t kind, struct dhtTorrent *torrent)
{
    dhtNode *nodes[DHT_CANDIDATES];
    lookup  *lk;
    uint32_t count;
    uint32_t slot;
    uint32_t i;

    for (slot = 0; slot < DHT_MAX_LOOKUPS && d->lookups[slot] != NULL; slot++) {
    }
    if (slot == DHT_MAX_LOOKUPS) {
        return -1;
    }
    lk = allocate(d, sizeof(*lk));
    if (lk == NULL) {
        return -1;
    }
    memcpy(lk->target, target, DHT_ID_LENGTH);
    lk->kind = kind;
    lk->torrent = torrent;
    count = closestNodes(d, target, NULL, nodes, 32u);
    for (i = 0; i < count; i++) {
        addCandidate(lk, nodes[i]->id, &nodes[i]->address);
    }
    if (count < DHT_BUCKET_SIZE) {
        for (i = 0; i < d->bootstrapCount; i++) {   /* few nodes known: ask routers */
            addCandidate(lk, NULL, &d->bootstrap[i]);
        }
    }
    d->lookups[slot] = lk;
    if (torrent != NULL) {
        torrent->running = lk;
    }
    stepLookup(d, lk);                              /* may finish (and free) it */
    return 0;
}

static void freeLookup(dht *d, lookup *lk)
{
    uint32_t i;

    for (i = 0; i < DHT_MAX_LOOKUPS; i++) {
        if (d->lookups[i] == lk) {
            d->lookups[i] = NULL;
        }
    }
    for (i = 0; i < DHT_MAX_PENDING; i++) {
        if (d->pending[i].used && d->pending[i].owner == lk) {
            d->pending[i].owner = NULL;             /* late answers are ignored */
        }
    }
    release(d, lk);
}

static void finishLookup(dht *d, lookup *lk)
{
    struct dhtTorrent *torrent = lk->torrent;
    uint32_t           announced = 0;
    uint32_t           i;

    if (torrent != NULL) {
        /* Tell the closest nodes that answered (with a token) about us. */
        for (i = 0; i < lk->count && announced < DHT_BUCKET_SIZE && torrent->announcePort; i++) {
            candidate *c = &lk->candidates[i];

            if (c->state == CANDIDATE_REPLIED && c->tokenLength > 0) {
                (void)sendQuery(d, &c->address, QUERY_ANNOUNCE, NULL, lk->target, c->token,
                                c->tokenLength, torrent->announcePort);
                announced++;
            }
        }
        torrent->running = NULL;
        torrent->nextSearch = uv_now(d->loop) +
            (uint64_t)((lk->peersFound < DHT_BUCKET_SIZE) ? DHT_SEARCH_INTERVAL_FEW
                                                          : DHT_SEARCH_INTERVAL) * 1000u;
    }
    {
        dhtSearchDoneCallback done = (torrent != NULL) ? torrent->done : NULL;
        void                 *argument = (torrent != NULL) ? torrent->privateArg : NULL;
        uint32_t              peers = lk->peersFound;
        uint32_t              asked = lk->asked;

        freeLookup(d, lk);                          /* before the user can call dhtEnd() */
        if (done != NULL) {
            done(argument, peers, asked);
        }
    }
}

/* Peers from a get_peers answer: report each once per lookup. */
static void reportPeers(lookup *lk, const uint8_t *const *values, uint32_t count)
{
    struct sockaddr_in peers[DHT_MAX_VALUES];
    uint32_t           fresh = 0;
    uint32_t           i;
    uint32_t           j;

    for (i = 0; i < count && fresh < DHT_MAX_VALUES; i++) {
        uint32_t ip;
        uint32_t port;
        int      seen = 0;

        fromCompact(values[i], &peers[fresh]);
        if (!usableAddress(&peers[fresh])) {
            continue;
        }
        ip = peers[fresh].sin_addr.s_addr;
        port = peers[fresh].sin_port;
        for (j = 0; j < lk->seenCount && !seen; j++) {
            seen = (lk->seen[2u * j] == ip && lk->seen[2u * j + 1u] == port);
        }
        if (seen) {
            continue;
        }
        if (lk->seenCount < DHT_SEEN_PEERS) {
            lk->seen[2u * lk->seenCount] = ip;
            lk->seen[2u * lk->seenCount + 1u] = port;
            lk->seenCount++;
        }
        fresh++;
    }
    lk->peersFound += fresh;
    if (fresh > 0 && lk->torrent != NULL && lk->torrent->callback != NULL) {
        lk->torrent->callback(lk->torrent->privateArg, peers, fresh);
    }
}

/* ------------------------------------------------------------------------ */
/* Receiving                                                                */
/* ------------------------------------------------------------------------ */

typedef struct krpc {
    char            type;                   /* 'q', 'r' or 'e'                */
    const uint8_t  *tid;
    uint64_t        tidLength;
    char            query[16];
    const uint8_t  *id;
    const uint8_t  *target;
    const uint8_t  *infoHash;
    const uint8_t  *token;
    uint64_t        tokenLength;
    const uint8_t  *nodes;
    uint64_t        nodesLength;
    const uint8_t  *values[DHT_MAX_VALUES];
    uint32_t        valueCount;
    int64_t         port;
    int64_t         impliedPort;
} krpc;

static int keyIs(const uint8_t *key, uint64_t length, const char *name)
{
    return length == strlen(name) && memcmp(key, name, (size_t)length) == 0;
}

static int readBytes(parserCursor *cursor, const uint8_t **value, uint64_t *length)
{
    return parserCursorPeek(cursor) >= '0' && parserCursorPeek(cursor) <= '9' &&
           parserReadString(cursor, value, length) == PARSER_OK;
}

/* The "a" or "r" dictionary. */
static int decodeArguments(parserCursor *cursor, krpc *message)
{
    const uint8_t *key;
    const uint8_t *value;
    uint64_t       keyLength;
    uint64_t       length;
    uint8_t        more;

    if (parserContainerBegin(cursor, 'd') != PARSER_OK) {
        return -1;
    }
    for (;;) {
        if (parserContainerNext(cursor, &more) != PARSER_OK) {
            return -1;
        }
        if (!more) {
            return 0;
        }
        if (parserReadString(cursor, &key, &keyLength) != PARSER_OK) {
            return -1;
        }
        if ((keyIs(key, keyLength, "id") || keyIs(key, keyLength, "target") ||
             keyIs(key, keyLength, "info_hash")) && readBytes(cursor, &value, &length)) {
            if (length != DHT_ID_LENGTH) {
                return -1;
            }
            if (keyIs(key, keyLength, "id")) {
                message->id = value;
            } else if (keyIs(key, keyLength, "target")) {
                message->target = value;
            } else {
                message->infoHash = value;
            }
        } else if (keyIs(key, keyLength, "token") && readBytes(cursor, &value, &length)) {
            message->token = value;
            message->tokenLength = length;
        } else if (keyIs(key, keyLength, "nodes") && readBytes(cursor, &value, &length)) {
            message->nodes = value;
            message->nodesLength = length;
        } else if ((keyIs(key, keyLength, "port") || keyIs(key, keyLength, "implied_port")) &&
                   parserCursorPeek(cursor) == 'i') {
            int64_t number;

            if (parserReadInteger(cursor, &number) != PARSER_OK) {
                return -1;
            }
            if (keyIs(key, keyLength, "port")) {
                message->port = number;
            } else {
                message->impliedPort = number;
            }
        } else if (keyIs(key, keyLength, "values") && parserCursorPeek(cursor) == 'l') {
            if (parserContainerBegin(cursor, 'l') != PARSER_OK) {
                return -1;
            }
            for (;;) {
                if (parserContainerNext(cursor, &more) != PARSER_OK) {
                    return -1;
                }
                if (!more) {
                    break;
                }
                if (readBytes(cursor, &value, &length)) {
                    if (length == 6 && message->valueCount < DHT_MAX_VALUES) {
                        message->values[message->valueCount++] = value;
                    }
                } else if (parserSkipValue(cursor) != PARSER_OK) {
                    return -1;
                }
            }
        } else if (parserSkipValue(cursor) != PARSER_OK) {
            return -1;
        }
    }
}

static int decodeMessage(const uint8_t *data, size_t length, krpc *message)
{
    parserCursor   cursor;
    const uint8_t *key;
    const uint8_t *value;
    uint64_t       keyLength;
    uint64_t       valueLength;
    uint8_t        more;

    memset(message, 0, sizeof(*message));
    memset(&cursor, 0, sizeof(cursor));
    cursor.buffer = data;
    cursor.length = length;
    if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
        return -1;
    }
    for (;;) {
        if (parserContainerNext(&cursor, &more) != PARSER_OK) {
            return -1;
        }
        if (!more) {
            break;
        }
        if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK) {
            return -1;
        }
        if (keyIs(key, keyLength, "t") && readBytes(&cursor, &value, &valueLength)) {
            message->tid = value;
            message->tidLength = valueLength;
        } else if (keyIs(key, keyLength, "y") && readBytes(&cursor, &value, &valueLength)) {
            message->type = (valueLength == 1) ? (char)value[0] : '?';
        } else if (keyIs(key, keyLength, "q") && readBytes(&cursor, &value, &valueLength)) {
            if (valueLength < sizeof(message->query)) {
                memcpy(message->query, value, (size_t)valueLength);
            }
        } else if ((keyIs(key, keyLength, "a") || keyIs(key, keyLength, "r")) &&
                   parserCursorPeek(&cursor) == 'd') {
            if (decodeArguments(&cursor, message) != 0) {
                return -1;
            }
        } else if (parserSkipValue(&cursor) != PARSER_OK) {
            return -1;
        }
    }
    return (message->tid != NULL && message->tidLength <= 16u &&
            (message->type == 'q' || message->type == 'r' || message->type == 'e')) ? 0 : -1;
}

static void makeToken(const dht *d, uint32_t secret, const struct sockaddr_in *from, uint8_t out[8])
{
    uint8_t input[20];
    uint8_t digest[PARSER_SHA1_LENGTH];

    memcpy(input, d->secrets[secret], 16);
    memcpy(input + 16, &from->sin_addr, 4);
    parserSha1(input, sizeof(input), digest);
    memcpy(out, digest, 8);
}

static storedHash *findStored(dht *d, const uint8_t *infoHash, int create)
{
    storedHash *oldest = NULL;
    uint32_t    i;

    if (d->stored == NULL) {
        if (!create) {
            return NULL;
        }
        d->stored = allocate(d, sizeof(*d->stored) * DHT_MAX_STORED);
        if (d->stored == NULL) {
            return NULL;
        }
    }
    for (i = 0; i < DHT_MAX_STORED; i++) {
        storedHash *entry = &d->stored[i];

        if (entry->count > 0 && memcmp(entry->infoHash, infoHash, PARSER_SHA1_LENGTH) == 0) {
            return entry;
        }
        if (oldest == NULL || entry->count == 0 ||
            (oldest->count > 0 && entry->lastUsed < oldest->lastUsed)) {
            oldest = (oldest != NULL && oldest->count == 0) ? oldest : entry;
        }
    }
    if (!create || oldest == NULL) {
        return NULL;
    }
    memset(oldest, 0, sizeof(*oldest));
    memcpy(oldest->infoHash, infoHash, PARSER_SHA1_LENGTH);
    return oldest;
}

/* A KRPC error reply: d1:eli<code>e<message>e1:t<tid>1:y1:ee */
static void sendError(dht *d, const krpc *message, const struct sockaddr_in *from,
                      int64_t code, const char *text)
{
    uint8_t       packet[DHT_PACKET];
    bencodeWriter w;

    bencodeWriterInitFixed(&w, packet, sizeof(packet));
    bencodeDictBegin(&w);
    bencodeText(&w, "e");
    bencodeListBegin(&w);
    bencodeInteger(&w, code);
    bencodeText(&w, text);
    bencodeEnd(&w);
    bencodeText(&w, "t");
    bencodeString(&w, message->tid, (size_t)message->tidLength);
    bencodeText(&w, "y");
    bencodeText(&w, "e");
    bencodeEnd(&w);
    if (w.error == BENCODE_OK) {
        sendPacket(d, from, packet, (size_t)w.length);
    }
}

/* Answer a query. */
static void answerQuery(dht *d, const krpc *message, const struct sockaddr_in *from)
{
    uint8_t     packet[DHT_PACKET];
    bencodeWriter w;
    dhtNode    *nodes[DHT_BUCKET_SIZE];
    uint8_t     token[8];
    uint8_t     compact[26];
    uint64_t    now = uv_now(d->loop);
    uint32_t    count;
    uint32_t    i;
    int         isPing = strcmp(message->query, "ping") == 0;
    int         isFind = strcmp(message->query, "find_node") == 0;
    int         isGet = strcmp(message->query, "get_peers") == 0;
    int         isAnnounce = strcmp(message->query, "announce_peer") == 0;

    bencodeWriterInitFixed(&w, packet, sizeof(packet));

    if (isAnnounce) {
        storedHash *entry;
        uint8_t     current[8];
        uint8_t     previous[8];
        uint16_t    port;

        makeToken(d, 0, from, current);
        makeToken(d, 1, from, previous);
        if (message->infoHash == NULL || message->token == NULL || message->tokenLength != 8 ||
            (memcmp(message->token, current, 8) != 0 && memcmp(message->token, previous, 8) != 0)) {
            sendError(d, message, from, 203, "bad token");
            return;
        }
        port = (message->impliedPort != 0) ? ntohs(from->sin_port) : (uint16_t)message->port;
        entry = (port != 0) ? findStored(d, message->infoHash, 1) : NULL;
        if (entry != NULL) {
            storedPeer *slot = NULL;

            for (i = 0; i < entry->count; i++) {
                if (memcmp(entry->peers[i].ip, &from->sin_addr, 4) == 0 &&
                    entry->peers[i].port == port) {
                    slot = &entry->peers[i];
                }
            }
            if (slot == NULL) {
                slot = (entry->count < DHT_STORED_PEERS) ? &entry->peers[entry->count++]
                     : &entry->peers[nextRandom(d) % DHT_STORED_PEERS];
            }
            memcpy(slot->ip, &from->sin_addr, 4);
            slot->port = port;
            slot->at = now;
            entry->lastUsed = now;
        }
    } else if (!isPing && !isFind && !isGet) {
        sendError(d, message, from, 204, "Method Unknown");
        return;
    }
    if ((isFind && message->target == NULL) || (isGet && message->infoHash == NULL)) {
        return;
    }

    /* Response: keys sorted id, nodes, token, values. */
    bencodeDictBegin(&w);
    bencodeText(&w, "r");
    bencodeDictBegin(&w);
    bencodeText(&w, "id");
    bencodeString(&w, d->id, DHT_ID_LENGTH);
    if (isFind || isGet) {
        storedHash *entry = isGet ? findStored(d, message->infoHash, 0) : NULL;
        uint32_t    fresh = 0;

        if (entry != NULL) {
            for (i = 0; i < entry->count; i++) {
                fresh += (now - entry->peers[i].at < (uint64_t)DHT_STORE_SECONDS * 1000u) ? 1u : 0u;
            }
        }
        if (fresh == 0) {
            count = closestNodes(d, isFind ? message->target : message->infoHash, from, nodes,
                                 DHT_BUCKET_SIZE);
            bencodeText(&w, "nodes");
            bencodeStringHeader(&w, count * 26u);
            for (i = 0; i < count; i++) {
                memcpy(compact, nodes[i]->id, DHT_ID_LENGTH);
                compactAddress(&nodes[i]->address, compact + DHT_ID_LENGTH);
                bencodeRaw(&w, compact, 26);
            }
        }
        if (isGet) {
            makeToken(d, 0, from, token);
            bencodeText(&w, "token");
            bencodeString(&w, token, 8);
        }
        if (fresh > 0) {
            uint32_t sent = 0;

            bencodeText(&w, "values");
            bencodeListBegin(&w);
            for (i = 0; i < entry->count && sent < DHT_MAX_VALUES; i++) {
                if (now - entry->peers[i].at < (uint64_t)DHT_STORE_SECONDS * 1000u) {
                    memcpy(compact, entry->peers[i].ip, 4);
                    compact[4] = (uint8_t)(entry->peers[i].port >> 8);
                    compact[5] = (uint8_t)(entry->peers[i].port & 0xffu);
                    bencodeString(&w, compact, 6);
                    sent++;
                }
            }
            bencodeEnd(&w);
        }
    }
    bencodeEnd(&w);
    bencodeText(&w, "t");
    bencodeString(&w, message->tid, (size_t)message->tidLength);
    bencodeText(&w, "v");
    bencodeString(&w, DHT_VERSION, 4);
    bencodeText(&w, "y");
    bencodeText(&w, "r");
    bencodeEnd(&w);
    if (w.error == BENCODE_OK) {
        sendPacket(d, from, packet, (size_t)w.length);
    }
}

static void handleResponse(dht *d, const krpc *message, const struct sockaddr_in *from)
{
    pendingQuery *query = NULL;
    lookup       *lk;
    uint16_t      tid;
    uint32_t      i;

    if (message->tidLength != 2) {
        return;
    }
    tid = (uint16_t)((message->tid[0] << 8) | message->tid[1]);
    for (i = 0; i < DHT_MAX_PENDING; i++) {
        if (d->pending[i].used && d->pending[i].tid == tid && sameEndpoint(&d->pending[i].to, from)) {
            query = &d->pending[i];
            break;
        }
    }
    if (query == NULL) {
        return;                                     /* late, or not ours */
    }
    query->used = 0;
    lk = query->owner;
    if (message->type == 'e' || message->id == NULL) {
        nodeFailed(d, from);
        if (lk != NULL) {
            for (i = 0; i < lk->count; i++) {
                if (sameEndpoint(&lk->candidates[i].address, from)) {
                    lk->candidates[i].state = CANDIDATE_FAILED;
                }
            }
            lk->inFlight--;
            stepLookup(d, lk);
        }
        return;
    }
    addNode(d, message->id, from, 1);
    if (lk == NULL) {
        return;
    }
    lk->inFlight--;
    for (i = 0; i < lk->count; i++) {
        candidate *c = &lk->candidates[i];

        if (sameEndpoint(&c->address, from)) {
            c->state = CANDIDATE_REPLIED;
            if (!c->idKnown) {
                memcpy(c->id, message->id, DHT_ID_LENGTH);
                c->idKnown = 1;                     /* a router: now placed later */
            }
            if (message->token != NULL && message->tokenLength <= sizeof(c->token)) {
                memcpy(c->token, message->token, (size_t)message->tokenLength);
                c->tokenLength = (uint8_t)message->tokenLength;
            }
            break;
        }
    }
    if (message->valueCount > 0) {
        reportPeers(lk, message->values, message->valueCount);
        if (d->ended) {
            return;                                 /* the callback ended the dht */
        }
    }
    for (i = 0; message->nodes != NULL && i + 26u <= message->nodesLength; i += 26u) {
        struct sockaddr_in address;

        fromCompact(message->nodes + i + DHT_ID_LENGTH, &address);
        if (blocklistContains(d->blocklist, (const struct sockaddr *)&address)) {
            d->blocked++;
        } else if (memcmp(message->nodes + i, d->id, DHT_ID_LENGTH) != 0) {
            addCandidate(lk, message->nodes + i, &address);
        }
    }
    /* A router replaced by a real id may now sort elsewhere: re-sort. */
    for (i = 1; i < lk->count; i++) {
        candidate entry = lk->candidates[i];
        uint32_t  j = i;

        while (j > 0 && entry.idKnown &&
               (!lk->candidates[j - 1u].idKnown ||
                compareDistance(lk->target, entry.id, lk->candidates[j - 1u].id) < 0)) {
            lk->candidates[j] = lk->candidates[j - 1u];
            j--;
        }
        lk->candidates[j] = entry;
    }
    stepLookup(d, lk);
}

static void onAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    dht *d = handle->data;

    (void)suggested;
    *buffer = uv_buf_init(d->receiveBuffer, DHT_PACKET);
}

static void onReceive(uv_udp_t *udp, ssize_t count, const uv_buf_t *buffer,
                      const struct sockaddr *from, unsigned flags)
{
    dht *d = udp->data;
    krpc message;

    (void)flags;
    if (d->ended || count <= 0 || from == NULL || from->sa_family != AF_INET) {
        return;
    }
    if (blocklistContains(d->blocklist, from)) {
        d->blocked++;                               /* neither heard nor answered */
        return;
    }
    d->received++;
    if (decodeMessage((const uint8_t *)buffer->base, (size_t)count, &message) != 0) {
        return;
    }
    if (message.type == 'q') {
        if (message.id == NULL) {
            return;
        }
        addNode(d, message.id, (const struct sockaddr_in *)from, 0);
        answerQuery(d, &message, (const struct sockaddr_in *)from);
    } else {
        handleResponse(d, &message, (const struct sockaddr_in *)from);
    }
}

/* ------------------------------------------------------------------------ */
/* Bootstrap and upkeep                                                     */
/* ------------------------------------------------------------------------ */

static uint32_t nodeCount(const dht *d, uint32_t *good)
{
    uint64_t now = uv_now(d->loop);
    uint32_t count = 0;
    uint32_t i;

    if (good != NULL) {
        *good = 0;
    }
    for (i = 0; i < DHT_BUCKETS * DHT_BUCKET_SIZE; i++) {
        if (d->table[i].used) {
            count++;
            if (good != NULL && d->table[i].lastReply != 0 &&
                now - d->table[i].lastReply < DHT_QUESTIONABLE_MS) {
                (*good)++;
            }
        }
    }
    return count;
}

static void addBootstrap(dht *d, const struct sockaddr_in *address)
{
    uint32_t i;

    for (i = 0; i < d->bootstrapCount; i++) {
        if (sameEndpoint(&d->bootstrap[i], address)) {
            return;
        }
    }
    if (d->bootstrapCount < DHT_MAX_BOOTSTRAP) {
        d->bootstrap[d->bootstrapCount++] = *address;
    }
}

/* Find nodes near our own id: fills the table. */
static void refreshTable(dht *d)
{
    d->lastBootstrap = uv_now(d->loop);
    (void)startLookup(d, d->id, QUERY_FIND_NODE, NULL);
}

/* Everything is closed and answered: free the lot. */
static void finalRelease(dht *d)
{
    struct dhtTorrent *torrent;
    uint32_t           i;

    for (i = 0; i < DHT_MAX_LOOKUPS; i++) {
        release(d, d->lookups[i]);
    }
    while ((torrent = TAILQ_FIRST(&d->torrents)) != NULL) {
        TAILQ_REMOVE(&d->torrents, torrent, entries);
        release(d, torrent);
    }
    release(d, d->table);
    release(d, d->stored);
    release(d, d);
}

static void onResolved(uv_getaddrinfo_t *request, int status, struct addrinfo *result)
{
    resolveRequest  *resolve = request->data;
    dht             *d = resolve->owner;
    struct addrinfo *at;

    d->resolving--;
    if (status == 0 && !d->ended) {
        for (at = result; at != NULL; at = at->ai_next) {
            if (at->ai_family == AF_INET) {
                struct sockaddr_in address = *(const struct sockaddr_in *)at->ai_addr;

                address.sin_port = htons(resolve->port);
                addBootstrap(d, &address);
            }
        }
        if (nodeCount(d, NULL) < DHT_BUCKET_SIZE) {
            refreshTable(d);
        }
    }
    uv_freeaddrinfo(result);
    release(d, resolve);
    if (d->ended && d->resolving == 0 && d->openHandles == 0) {
        finalRelease(d);
    }
}

static void resolveBootstrap(dht *d, const char *spec)
{
    struct sockaddr_in address;
    struct addrinfo    hints;
    resolveRequest    *resolve;
    char               host[256];
    const char        *colon = strrchr(spec, ':');
    long               port;

    if (colon == NULL || (size_t)(colon - spec) >= sizeof(host)) {
        return;
    }
    port = strtol(colon + 1, NULL, 10);
    if (port <= 0 || port > 65535) {
        return;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    if (uv_ip4_addr(host, (int)port, &address) == 0) {
        addBootstrap(d, &address);                  /* a literal address */
        return;
    }
    resolve = allocate(d, sizeof(*resolve));
    if (resolve == NULL) {
        return;
    }
    resolve->owner = d;
    resolve->port = (uint16_t)port;
    resolve->request.data = resolve;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (uv_getaddrinfo(d->loop, &resolve->request, onResolved, host, NULL, &hints) == 0) {
        d->resolving++;
    } else {
        release(d, resolve);
    }
}

static void onTick(uv_timer_t *timer)
{
    dht               *d = timer->data;
    struct dhtTorrent *torrent;
    uint64_t           now = uv_now(d->loop);
    uint32_t           i;

    if (d->ended) {
        return;
    }
    /* Queries that went unanswered. */
    for (i = 0; i < DHT_MAX_PENDING; i++) {
        pendingQuery *query = &d->pending[i];

        if (query->used && now - query->sentAt >= DHT_QUERY_TIMEOUT_MS) {
            lookup  *lk = query->owner;
            uint32_t j;

            query->used = 0;
            nodeFailed(d, &query->to);
            if (lk != NULL) {
                for (j = 0; j < lk->count; j++) {
                    if (sameEndpoint(&lk->candidates[j].address, &query->to) &&
                        lk->candidates[j].state == CANDIDATE_ASKED) {
                        lk->candidates[j].state = CANDIDATE_FAILED;
                    }
                }
                lk->inFlight--;
                stepLookup(d, lk);                  /* may free lk */
            }
        }
    }
    /* Lookups that wait for a free pending slot. */
    for (i = 0; i < DHT_MAX_LOOKUPS; i++) {
        if (d->lookups[i] != NULL) {
            stepLookup(d, d->lookups[i]);
        }
    }
    /* Torrents due for a lookup. */
    TAILQ_FOREACH(torrent, &d->torrents, entries) {
        if (torrent->running == NULL && now >= torrent->nextSearch) {
            if (startLookup(d, torrent->infoHash, QUERY_GET_PEERS, torrent) != 0) {
                torrent->nextSearch = now + 5000u;  /* too many at once: soon */
            }
            if (d->ended) {
                return;
            }
        }
    }
    /* Few nodes: look for more, every minute. */
    if (now - d->lastBootstrap >= 60000u && nodeCount(d, NULL) < 4u * DHT_BUCKET_SIZE) {
        refreshTable(d);
    }
    /* Every 15 s, ping a few nodes we have not heard from for a while. */
    if (now - d->lastMaintenance >= 15000u) {
        uint32_t pinged = 0;

        d->lastMaintenance = now;
        for (i = 0; i < DHT_BUCKETS * DHT_BUCKET_SIZE && pinged < 4u; i++) {
            dhtNode *node = &d->table[i];

            if (node->used && (node->lastPing == 0 || now - node->lastPing > 300000u) &&
                (node->lastReply == 0 || now - node->lastReply >= DHT_QUESTIONABLE_MS)) {
                node->lastPing = now;
                if (sendQuery(d, &node->address, QUERY_PING, NULL, NULL, NULL, 0, 0) == 0) {
                    pinged++;
                }
            }
        }
    }
    /* New token secret every 5 minutes. */
    if (now - d->lastRotate >= (uint64_t)DHT_TOKEN_SECONDS * 1000u) {
        memcpy(d->secrets[1], d->secrets[0], sizeof(d->secrets[0]));
        randomBytes(d, d->secrets[0], sizeof(d->secrets[0]));
        d->lastRotate = now;
    }
}

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

/* d2:id20:<id>5:nodes<26 * n>e */
static void loadState(dht *d, const uint8_t *data, size_t length)
{
    parserCursor   cursor;
    const uint8_t *key;
    const uint8_t *value;
    uint64_t       keyLength;
    uint64_t       valueLength;
    uint8_t        more;
    uint64_t       i;

    memset(&cursor, 0, sizeof(cursor));
    cursor.buffer = data;
    cursor.length = length;
    if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
        return;
    }
    while (parserContainerNext(&cursor, &more) == PARSER_OK && more) {
        if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK) {
            return;
        }
        if (!readBytes(&cursor, &value, &valueLength)) {
            if (parserSkipValue(&cursor) != PARSER_OK) {
                return;
            }
            continue;
        }
        if (keyIs(key, keyLength, "id") && valueLength == DHT_ID_LENGTH) {
            memcpy(d->id, value, DHT_ID_LENGTH);
        } else if (keyIs(key, keyLength, "nodes")) {
            for (i = 0; i + 26u <= valueLength; i += 26u) {
                struct sockaddr_in address;

                fromCompact(value + i + DHT_ID_LENGTH, &address);
                addNode(d, value + i, &address, 0);  /* to be confirmed */
            }
        }
    }
}

int32_t dhtSaveState(const dht *d, uint8_t *out, size_t size, size_t *length)
{
    bencodeWriter w;
    uint8_t  compact[26];
    uint32_t count = 0;
    uint32_t i;

    if (d == NULL || out == NULL || length == NULL) {
        return DHT_EINVAL;
    }
    bencodeWriterInitFixed(&w, out, size);
    for (i = 0; i < DHT_BUCKETS * DHT_BUCKET_SIZE; i++) {
        count += (d->table[i].used && d->table[i].lastReply != 0) ? 1u : 0u;
    }
    if (count > 256u) {
        count = 256u;
    }
    bencodeDictBegin(&w);
    bencodeText(&w, "id");
    bencodeString(&w, d->id, DHT_ID_LENGTH);
    bencodeText(&w, "nodes");
    bencodeStringHeader(&w, count * 26u);
    for (i = 0; i < DHT_BUCKETS * DHT_BUCKET_SIZE && count > 0; i++) {
        if (d->table[i].used && d->table[i].lastReply != 0) {
            memcpy(compact, d->table[i].id, DHT_ID_LENGTH);
            compactAddress(&d->table[i].address, compact + DHT_ID_LENGTH);
            bencodeRaw(&w, compact, 26);
            count--;
        }
    }
    bencodeEnd(&w);
    if (w.error != BENCODE_OK) {
        return DHT_EINVAL;
    }
    *length = (size_t)w.length;
    return DHT_OK;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

static const char *const defaultBootstrap[] = {
    "router.bittorrent.com:6881",
    "dht.transmissionbt.com:6881",
    "router.utorrent.com:6881",
    "dht.libtorrent.org:25401",
    NULL
};

static void onHandleClosed(uv_handle_t *handle)
{
    dht *d = handle->data;

    if (--d->openHandles == 0 && d->resolving == 0) {
        finalRelease(d);
    }
}

int32_t dhtBegin(uv_loop_t *loop, parserContext *context, const dhtOptions *options,
                 dht **handle)
{
    const char *const *bootstrap;
    dht               *d;
    uint16_t           base;
    uint32_t           attempt;
    int                result = UV_EADDRINUSE;

    if (handle != NULL) {
        *handle = NULL;
    }
    if (loop == NULL || context == NULL || options == NULL || handle == NULL) {
        return DHT_EINVAL;
    }
    d = context->allocator.realloc(NULL, sizeof(*d));
    if (d == NULL) {
        return DHT_ENOMEM;
    }
    memset(d, 0, sizeof(*d));
    d->loop = loop;
    d->context = context;
    d->blocklist = options->blocklist;
    if (options->binding != NULL) {
        d->binding = *options->binding;
    }
    d->randomState = uv_hrtime() | 1u;
    TAILQ_INIT(&d->torrents);
    d->table = allocate(d, sizeof(*d->table) * DHT_BUCKETS * DHT_BUCKET_SIZE);
    if (d->table == NULL) {
        context->allocator.free(d);
        return DHT_ENOMEM;
    }
    randomBytes(d, d->id, sizeof(d->id));
    randomBytes(d, d->secrets[0], sizeof(d->secrets[0]));
    randomBytes(d, d->secrets[1], sizeof(d->secrets[1]));
    d->lastRotate = uv_now(loop);
    if (options->state != NULL && options->stateLength > 0) {
        loadState(d, options->state, options->stateLength);
    }

    if (uv_udp_init_ex(loop, &d->udp, AF_INET) != 0) {   /* the socket exists now */
        release(d, d->table);
        context->allocator.free(d);
        return DHT_ERROR;
    }
    d->udp.data = d;
    d->openHandles++;
    base = (options->port != 0) ? options->port : 6881u;
    for (attempt = 0; attempt < DHT_PORT_ATTEMPTS && (uint32_t)base + attempt <= 65535u;
         attempt++) {
        result = netUdpBindPort(&d->udp, &d->binding, AF_INET, (uint16_t)(base + attempt));
        if (result != UV_EADDRINUSE) {
            break;
        }
    }
    if (result == 0) {
        d->port = (uint16_t)(base + attempt);
        result = uv_udp_recv_start(&d->udp, onAlloc, onReceive);
    }
    uv_timer_init(loop, &d->timer);
    d->timer.data = d;
    d->openHandles++;
    if (result != 0) {
        d->ended = 1;
        uv_close((uv_handle_t *)&d->udp, onHandleClosed);
        uv_close((uv_handle_t *)&d->timer, onHandleClosed);
        return (result == UV_EADDRINUSE) ? DHT_EADDRINUSE : DHT_ERROR;
    }
    uv_timer_start(&d->timer, onTick, DHT_TICK_MS, DHT_TICK_MS);

    for (bootstrap = (options->bootstrap != NULL) ? options->bootstrap : defaultBootstrap;
         *bootstrap != NULL; bootstrap++) {
        resolveBootstrap(d, *bootstrap);
    }
    refreshTable(d);                                /* saved nodes and literal routers */
    *handle = d;
    return DHT_OK;
}

int32_t dhtAddTorrent(dht *d, const uint8_t infoHash[PARSER_SHA1_LENGTH], uint16_t announcePort,
                      dhtPeersCallback callback, dhtSearchDoneCallback done, void *privateArg,
                      dhtTorrent **torrent)
{
    struct dhtTorrent *entry;

    if (torrent != NULL) {
        *torrent = NULL;
    }
    if (d == NULL || d->ended || infoHash == NULL || torrent == NULL) {
        return DHT_EINVAL;
    }
    entry = allocate(d, sizeof(*entry));
    if (entry == NULL) {
        return DHT_ENOMEM;
    }
    memcpy(entry->infoHash, infoHash, PARSER_SHA1_LENGTH);
    entry->announcePort = announcePort;
    entry->callback = callback;
    entry->done = done;
    entry->privateArg = privateArg;
    entry->nextSearch = 0;                          /* at the next tick */
    TAILQ_INSERT_TAIL(&d->torrents, entry, entries);
    *torrent = entry;
    return DHT_OK;
}

void dhtRemoveTorrent(dht *d, dhtTorrent *torrent)
{
    if (d == NULL || torrent == NULL) {
        return;
    }
    if (torrent->running != NULL) {
        torrent->running->torrent = NULL;           /* finishes quietly */
    }
    TAILQ_REMOVE(&d->torrents, torrent, entries);
    release(d, torrent);
}

void dhtAddNode(dht *d, const struct sockaddr_in *address)
{
    if (d != NULL && !d->ended && address != NULL) {
        (void)sendQuery(d, address, QUERY_PING, NULL, NULL, NULL, 0, 0);
    }
}

void dhtGetStatus(const dht *d, dhtStatus *status)
{
    uint32_t i;
    uint32_t j;

    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (d == NULL) {
        return;
    }
    status->port = d->port;
    status->nodes = nodeCount(d, &status->goodNodes);
    for (i = 0; i < DHT_MAX_LOOKUPS; i++) {
        status->lookups += (d->lookups[i] != NULL) ? 1u : 0u;
    }
    for (i = 0; d->stored != NULL && i < DHT_MAX_STORED; i++) {
        for (j = 0; j < d->stored[i].count; j++) {
            status->storedPeers++;
        }
    }
    status->received = d->received;
    status->blocked = d->blocked;
    status->sent = d->sent;
}

void dhtEnd(dht *d)
{
    if (d == NULL || d->ended) {
        return;
    }
    d->ended = 1;                                   /* freed once all is closed */
    uv_udp_recv_stop(&d->udp);
    uv_close((uv_handle_t *)&d->udp, onHandleClosed);
    uv_timer_stop(&d->timer);
    uv_close((uv_handle_t *)&d->timer, onHandleClosed);
}
