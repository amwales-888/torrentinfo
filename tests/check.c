/*
 * check.c - runs the libaTorrent unit tests.
 *
 *     check [scratch-directory]
 *
 * Exit status 0 when everything passes.
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "platform.h"

uint32_t checkFailures;
uint32_t checkCount;

static int64_t     liveAllocations;
static int64_t     failAfter = -1;
static const char *scratch = ".";
static uint32_t    randomState = 0x12345678u;

static void *countingRealloc(void *pointer, size_t size)
{
    void *result;

    if (failAfter == 0) {
        return NULL;
    }
    if (failAfter > 0) {
        failAfter--;
    }
    result = realloc(pointer, size);
    if (result != NULL && pointer == NULL) {
        liveAllocations++;
    }
    return result;
}

static void countingFree(void *pointer)
{
    if (pointer != NULL) {
        liveAllocations--;
        free(pointer);
    }
}

const parserAllocator checkAllocator = { countingRealloc, countingFree };

int64_t checkLiveAllocations(void)
{
    return liveAllocations;
}

void checkFailAllocationsAfter(int64_t count)
{
    failAfter = count;
}

const char *checkScratchPath(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", scratch, name);
    return out;
}

uint32_t checkRandom(void)
{
    /* xorshift32 */
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}

typedef struct suite {
    const char *name;
    void      (*run)(void);
} suite;

int main(int argc, char **argv)
{
    static const suite suites[] = {
        { "bencode",   checkBencode   },
        { "selection", checkSelection },
        { "settings",  checkSettings  },
        { "datafile",  checkDataFile  },
        { "dht",       checkDht       },
        { "bandwidth", checkBandwidth },
        { "session",   checkSession   },
    };
    size_t i;

    if (argc > 1) {
        scratch = argv[1];
    }
    for (i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        uint32_t failuresBefore = checkFailures;
        uint32_t countBefore = checkCount;
        int64_t  liveBefore = liveAllocations;

        suites[i].run();
        if (liveAllocations != liveBefore) {
            checkFailures++;
            fprintf(stderr, "  FAIL  %s leaked %lld allocation(s)\n", suites[i].name,
                    (long long)(liveAllocations - liveBefore));
        }
        printf("%-5s %-10s %u checks\n", (checkFailures == failuresBefore) ? "PASS" : "FAIL",
               suites[i].name, checkCount - countBefore);
    }
    printf("== %u failure(s) in %u checks\n", checkFailures, checkCount);
    return (checkFailures == 0) ? 0 : 1;
}
