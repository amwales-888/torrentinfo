/*
 * check_dht.c - three DHT nodes on loopback. B announces a torrent to A;
 * C then looks the torrent up through A and must be given B's port. This
 * exercises every query and reply the node writes (ping, find_node,
 * get_peers with nodes, tokens and values, announce_peer), and the saved
 * state is read back with parserCursor.
 */
#include <string.h>

#include <uv.h>

#include "check.h"
#include "dht.h"

#define BASE_PORT       17301u
#define ANNOUNCED_PORT  5555u

typedef struct dhtCheck {
    dht       *a;
    dht       *b;
    dht       *c;
    uv_timer_t timer;
    uint32_t   ticks;
    int        found;                   /* C was given 127.0.0.1:5555 */
    int        lookupStarted;
    dhtTorrent *lookup;
    uint8_t    infoHash[PARSER_SHA1_LENGTH];
} dhtCheck;

static void onPeers(void *privateArg, const struct sockaddr_in *peers, uint32_t count)
{
    dhtCheck *t = privateArg;
    uint32_t  i;

    for (i = 0; i < count; i++) {
        if (ntohs(peers[i].sin_port) == ANNOUNCED_PORT &&
            ntohl(peers[i].sin_addr.s_addr) == 0x7f000001u) {
            t->found = 1;
        }
    }
}

static void ignorePeers(void *privateArg, const struct sockaddr_in *peers, uint32_t count)
{
    (void)privateArg;
    (void)peers;
    (void)count;
}

static void onTick(uv_timer_t *timer)
{
    dhtCheck *t = timer->data;

    t->ticks++;
    if (!t->lookupStarted && t->ticks >= 15) {          /* 1.5 s: B has announced */
        t->lookupStarted = 1;
        (void)dhtAddTorrent(t->c, t->infoHash, 0, onPeers, NULL, t, &t->lookup);
    }
    if (t->found || t->ticks >= 150) {
        uv_timer_stop(timer);
        uv_stop(timer->loop);
    }
}

static void closeTimer(uv_handle_t *handle)
{
    (void)handle;
}

static int readState(const uint8_t *state, size_t length, uint32_t *nodes)
{
    parserCursor   cursor;
    uint8_t        more;
    const uint8_t *key;
    const uint8_t *value;
    uint64_t       keyLength;
    uint64_t       valueLength;
    int            sawId = 0;

    memset(&cursor, 0, sizeof(cursor));
    cursor.buffer = state;
    cursor.length = length;
    *nodes = 0;
    if (parserContainerBegin(&cursor, 'd') != PARSER_OK) {
        return 0;
    }
    while (parserContainerNext(&cursor, &more) == PARSER_OK && more) {
        if (parserReadString(&cursor, &key, &keyLength) != PARSER_OK ||
            parserReadString(&cursor, &value, &valueLength) != PARSER_OK) {
            return 0;
        }
        if (keyLength == 2 && memcmp(key, "id", 2) == 0) {
            sawId = (valueLength == 20);
        } else if (keyLength == 5 && memcmp(key, "nodes", 5) == 0) {
            if (valueLength % 26u != 0) {
                return 0;
            }
            *nodes = (uint32_t)(valueLength / 26u);
        }
    }
    return sawId && cursor.position == length;
}

void checkDht(void)
{
    static const char *const none[] = { NULL };
    static const char *const viaA[] = { "127.0.0.1:17301", NULL };
    uv_loop_t     loop;
    parserContext context;
    dhtOptions    options;
    dhtCheck      t;
    dhtTorrent   *announced = NULL;
    uint8_t       state[DHT_MAX_STATE];
    size_t        stateLength = 0;
    uint32_t      nodes = 0;
    uint32_t      i;

    memset(&t, 0, sizeof(t));
    for (i = 0; i < PARSER_SHA1_LENGTH; i++) {
        t.infoHash[i] = (uint8_t)(0xa0u + i);
    }
    CHECK(uv_loop_init(&loop) == 0);
    CHECK(parserInit(&context, &checkAllocator) == PARSER_OK);

    memset(&options, 0, sizeof(options));
    options.bootstrap = none;
    options.port = BASE_PORT;
    CHECK(dhtBegin(&loop, &context, &options, &t.a) == DHT_OK);
    options.bootstrap = viaA;
    options.port = BASE_PORT + 20u;
    CHECK(dhtBegin(&loop, &context, &options, &t.b) == DHT_OK);
    options.port = BASE_PORT + 40u;
    CHECK(dhtBegin(&loop, &context, &options, &t.c) == DHT_OK);
    if (t.a == NULL || t.b == NULL || t.c == NULL) {
        return;
    }
    CHECK(dhtAddTorrent(t.b, t.infoHash, ANNOUNCED_PORT, ignorePeers, NULL, &t, &announced) ==
          DHT_OK);

    uv_timer_init(&loop, &t.timer);
    t.timer.data = &t;
    uv_timer_start(&t.timer, onTick, 100, 100);
    uv_run(&loop, UV_RUN_DEFAULT);

    CHECK(t.found);                                     /* get_peers values from A */
    CHECK(dhtSaveState(t.c, state, sizeof(state), &stateLength) == DHT_OK);
    CHECK(readState(state, stateLength, &nodes));
    CHECK(nodes >= 1);                                  /* A answered C */
    CHECK(dhtSaveState(t.c, state, 10, &stateLength) == DHT_EINVAL);  /* too small */

    uv_close((uv_handle_t *)&t.timer, closeTimer);
    dhtEnd(t.a);
    dhtEnd(t.b);
    dhtEnd(t.c);
    uv_run(&loop, UV_RUN_DEFAULT);
    CHECK(uv_loop_close(&loop) == 0);
}
