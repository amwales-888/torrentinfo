/*
 * selection.h - choosing which files of a torrent to download.
 *
 * A selection is a list of file numbers ("1,3,5-7", numbered from 1 as a
 * torrent listing shows them, padding files not counted) and/or shell-style
 * patterns such as *.mkv (see selectionAddPattern). selectionApply turns it
 * into one byte per file (padding files included), as
 * downloadOptions.fileWanted takes.
 *
 * Nothing is allocated and nothing is printed: errors come back as codes,
 * with the numbers involved in selectionResult so a caller can explain them.
 */
#ifndef SELECTION_H
#define SELECTION_H

#include <stddef.h>
#include <stdint.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SELECTION_MAX_PATTERNS      32u

#define SELECTION_OK                0
#define SELECTION_EINVAL            (-1)    /* bad arguments                        */
#define SELECTION_ESYNTAX           (-2)    /* list not like 1,3,5-7                */
#define SELECTION_EBACKWARDS        (-3)    /* a range like 7-5 (first, last set)   */
#define SELECTION_ERANGE            (-4)    /* no such file (number set)            */
#define SELECTION_EFULL             (-5)    /* more than SELECTION_MAX_PATTERNS     */
#define SELECTION_ENONE             (-6)    /* nothing chosen                       */

/* The strings are borrowed: they must outlive the selection. */
typedef struct selection {
    const char *list;                                   /* "1,3,5-7" or NULL       */
    const char *patterns[SELECTION_MAX_PATTERNS];
    uint32_t    patternCount;
} selection;

typedef struct selectionResult {
    uint32_t files;             /* numbered files in the torrent                  */
    uint32_t chosen;            /* ...of which chosen                              */
    uint64_t chosenBytes;
    uint64_t allBytes;
    uint32_t unmatched;         /* bit i set: patterns[i] matched no file          */
    uint64_t first;             /* SELECTION_EBACKWARDS: the range as written      */
    uint64_t last;
    uint64_t number;            /* SELECTION_ERANGE: the file that does not exist  */
} selectionResult;

void selectionInit(selection *choice);

/* Set the number list; checks its syntax (SELECTION_ESYNTAX,
 * SELECTION_EBACKWARDS with result->first/last; result may be NULL). */
int32_t selectionSetList(selection *choice, const char *list, selectionResult *result);

/* Add a pattern: '*' is any text (including '/'), '?' any one character,
 * ASCII case ignored. A pattern without '/' may also match just the file
 * name. SELECTION_EFULL when there are already SELECTION_MAX_PATTERNS. */
int32_t selectionAddPattern(selection *choice, const char *pattern);

/* Is anything to be chosen? (Otherwise every file is wanted.) */
int selectionActive(const selection *choice);

/* Fill wanted[0..torrent->fileCount) and *result. Files chosen by the list
 * or by any pattern are wanted. SELECTION_ERANGE if the list names a file
 * the torrent does not have; SELECTION_ENONE if nothing was chosen. */
int32_t selectionApply(const selection *choice, const parserTorrent *torrent, uint8_t *wanted,
                       selectionResult *result);

/* The file's path inside the torrent, '/'-separated, without the torrent
 * name; truncated if it does not fit. */
void selectionFilePath(const parserFile *file, char *text, size_t size);

/* The pattern match selectionApply uses. */
int selectionMatch(const char *pattern, const char *text);

const char *selectionErrorString(int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* SELECTION_H */
