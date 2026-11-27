/*
 * check_selection.c - file numbers, patterns and their errors, on a torrent
 * built in memory:
 *
 *     1  Show/a.bin        100 bytes
 *     2  Show/b.mkv        200
 *        (padding)          12
 *     3  Show/extras/e.mkv 300
 *     4  Show/c.TXT         50
 */
#include <string.h>

#include "check.h"
#include "selection.h"

#define FILES 5u

typedef struct fakeTorrent {
    parserTorrent       torrent;
    parserFile          files[FILES];
    parserPathComponent parts[8];
} fakeTorrent;

static void addFile(fakeTorrent *t, uint32_t index, uint64_t length, uint8_t isPadding,
                    const char *first, const char *second, uint32_t *part)
{
    parserFile *file = &t->files[index];

    file->length = length;
    file->isPadding = isPadding;
    TAILQ_INIT(&file->path);
    t->parts[*part].name = (char *)first;
    TAILQ_INSERT_TAIL(&file->path, &t->parts[*part], entries);
    (*part)++;
    if (second != NULL) {
        t->parts[*part].name = (char *)second;
        TAILQ_INSERT_TAIL(&file->path, &t->parts[*part], entries);
        (*part)++;
    }
    TAILQ_INSERT_TAIL(&t->torrent.files, file, entries);
    t->torrent.fileCount++;
}

static void build(fakeTorrent *t)
{
    uint32_t part = 0;

    memset(t, 0, sizeof(*t));
    TAILQ_INIT(&t->torrent.files);
    t->torrent.name = (char *)"Show";
    addFile(t, 0, 100, 0, "a.bin", NULL, &part);
    addFile(t, 1, 200, 0, "b.mkv", NULL, &part);
    addFile(t, 2, 12, 1, ".pad", "12", &part);
    addFile(t, 3, 300, 0, "extras", "e.mkv", &part);
    addFile(t, 4, 50, 0, "c.TXT", NULL, &part);
}

static int wantedIs(const uint8_t *wanted, const char *expected)
{
    uint32_t i;

    for (i = 0; i < FILES; i++) {
        if (wanted[i] != (uint8_t)(expected[i] == '1')) {
            return 0;
        }
    }
    return 1;
}

static int32_t applyList(const fakeTorrent *t, const char *list, uint8_t *wanted,
                         selectionResult *result)
{
    selection choice;
    int32_t   status;

    selectionInit(&choice);
    status = selectionSetList(&choice, list, result);
    if (status != SELECTION_OK) {
        return status;
    }
    return selectionApply(&choice, &t->torrent, wanted, result);
}

void checkSelection(void)
{
    fakeTorrent     t;
    selection       choice;
    selectionResult result;
    uint8_t         wanted[FILES];
    char            path[64];
    uint32_t        i;

    build(&t);

    /* Numbers skip the padding file. */
    CHECK(applyList(&t, "1,3", wanted, &result) == SELECTION_OK);
    CHECK(wantedIs(wanted, "10010"));
    CHECK(result.files == 4 && result.chosen == 2 && result.chosenBytes == 400 &&
          result.allBytes == 650);
    CHECK(applyList(&t, "2-4", wanted, &result) == SELECTION_OK);
    CHECK(wantedIs(wanted, "01011"));
    CHECK(applyList(&t, "4,4,1-1", wanted, &result) == SELECTION_OK);
    CHECK(wantedIs(wanted, "10001"));

    /* Syntax, backwards ranges and missing files. */
    CHECK(applyList(&t, "", wanted, &result) == SELECTION_ESYNTAX);
    CHECK(applyList(&t, "1,", wanted, &result) == SELECTION_ESYNTAX);
    CHECK(applyList(&t, "1-", wanted, &result) == SELECTION_ESYNTAX);
    CHECK(applyList(&t, "a", wanted, &result) == SELECTION_ESYNTAX);
    CHECK(applyList(&t, "1;2", wanted, &result) == SELECTION_ESYNTAX);
    CHECK(applyList(&t, "-3", wanted, &result) == SELECTION_ESYNTAX);
    CHECK(applyList(&t, "3-2", wanted, &result) == SELECTION_EBACKWARDS);
    CHECK(result.first == 3 && result.last == 2);
    CHECK(applyList(&t, "0", wanted, &result) == SELECTION_ERANGE && result.number == 0);
    CHECK(applyList(&t, "2-9", wanted, &result) == SELECTION_ERANGE && result.number == 9);
    CHECK(applyList(&t, "99999999999999999999999", wanted, &result) == SELECTION_ERANGE);

    /* Patterns: name only, with a folder, case, '?', none matching. */
    selectionInit(&choice);
    CHECK(selectionAddPattern(&choice, "*.MKV") == SELECTION_OK);
    CHECK(selectionApply(&choice, &t.torrent, wanted, &result) == SELECTION_OK);
    CHECK(wantedIs(wanted, "01010"));
    selectionInit(&choice);
    CHECK(selectionAddPattern(&choice, "extras/*") == SELECTION_OK);
    CHECK(selectionAddPattern(&choice, "?.txt") == SELECTION_OK);
    CHECK(selectionAddPattern(&choice, "*.iso") == SELECTION_OK);
    CHECK(selectionApply(&choice, &t.torrent, wanted, &result) == SELECTION_OK);
    CHECK(wantedIs(wanted, "00011"));
    CHECK(result.unmatched == 4u);                  /* only the third matched nothing */
    selectionInit(&choice);
    CHECK(selectionAddPattern(&choice, "e.mkv/x") == SELECTION_OK);  /* has '/': full path */
    CHECK(selectionApply(&choice, &t.torrent, wanted, &result) == SELECTION_ENONE);

    /* List and patterns together. */
    selectionInit(&choice);
    CHECK(selectionSetList(&choice, "1", NULL) == SELECTION_OK);
    CHECK(selectionAddPattern(&choice, "*.txt") == SELECTION_OK);
    CHECK(selectionActive(&choice));
    CHECK(selectionApply(&choice, &t.torrent, wanted, &result) == SELECTION_OK);
    CHECK(wantedIs(wanted, "10001"));

    /* Limits and arguments. */
    selectionInit(&choice);
    CHECK(!selectionActive(&choice));
    for (i = 0; i < SELECTION_MAX_PATTERNS; i++) {
        CHECK(selectionAddPattern(&choice, "*") == SELECTION_OK);
    }
    CHECK(selectionAddPattern(&choice, "*") == SELECTION_EFULL);
    CHECK(selectionApply(NULL, &t.torrent, wanted, &result) == SELECTION_EINVAL);

    /* The match itself. */
    CHECK(selectionMatch("*", ""));
    CHECK(selectionMatch("a*b*c", "aXXbYYc"));
    CHECK(!selectionMatch("a*b*c", "aXXbYY"));
    CHECK(selectionMatch("*/*.mkv", "extras/e.mkv"));
    CHECK(!selectionMatch("?", ""));

    /* Paths, and truncation. */
    selectionFilePath(&t.files[3], path, sizeof(path));
    CHECK(strcmp(path, "extras/e.mkv") == 0);
    selectionFilePath(&t.files[3], path, 8);       /* a terminated prefix */
    CHECK(strlen(path) < 8 && strncmp(path, "extras/e.mkv", strlen(path)) == 0);
}
