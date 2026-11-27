/*
 * check.h - a very small unit-test harness for libaTorrent.
 *
 * Each suite is a function that calls CHECK(); failures are counted and
 * reported with their file and line, and the run carries on.
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdint.h>
#include <stdio.h>

#include "parser.h"

extern uint32_t checkFailures;
extern uint32_t checkCount;

#define CHECK(condition)                                                        \
    do {                                                                        \
        checkCount++;                                                           \
        if (!(condition)) {                                                     \
            checkFailures++;                                                    \
            fprintf(stderr, "  FAIL  %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

/* An allocator that counts live blocks, and can be told to fail. */
extern const parserAllocator checkAllocator;
int64_t checkLiveAllocations(void);
void    checkFailAllocationsAfter(int64_t count);   /* -1: never fail */

/* A path under the test's scratch directory (created by main). */
const char *checkScratchPath(const char *name, char *out, size_t size);

/* Deterministic random numbers for the fuzz tests. */
uint32_t checkRandom(void);

/* The suites. */
void checkBencode(void);
void checkDht(void);
void checkSelection(void);
void checkSettings(void);
void checkDataFile(void);
void checkBandwidth(void);
void checkSession(void);

#endif /* CHECK_H */
