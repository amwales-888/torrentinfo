/*
 * selection.c - choosing which files of a torrent to download.
 * See selection.h.
 */
#include <stdio.h>
#include <string.h>

#include "selection.h"

void selectionInit(selection *choice)
{
    memset(choice, 0, sizeof(*choice));
}

static int isDigit(char c)
{
    return c >= '0' && c <= '9';
}

/* A decimal number; saturates rather than overflowing (any huge number is
 * then simply "no such file"). */
static uint64_t readNumber(const char **at)
{
    uint64_t value = 0;

    while (isDigit(**at)) {
        uint64_t digit = (uint64_t)(**at - '0');

        value = (value > (UINT64_MAX - digit) / 10u) ? UINT64_MAX : value * 10u + digit;
        (*at)++;
    }
    return value;
}

/*
 * Walk "1,3,5-7", calling mark() for each range when torrent is given.
 * Returns SELECTION_OK or the first error.
 */
typedef void (*rangeCallback)(void *privateArg, uint64_t first, uint64_t last);

static int32_t walkList(const char *list, rangeCallback mark, void *privateArg,
                        selectionResult *result)
{
    const char *at = list;

    if (*at == '\0') {
        return SELECTION_ESYNTAX;
    }
    while (*at != '\0') {
        uint64_t first;
        uint64_t last;

        if (!isDigit(*at)) {
            return SELECTION_ESYNTAX;
        }
        first = readNumber(&at);
        last = first;
        if (*at == '-') {
            at++;
            if (!isDigit(*at)) {
                return SELECTION_ESYNTAX;
            }
            last = readNumber(&at);
        }
        if (*at != '\0' && *at != ',') {
            return SELECTION_ESYNTAX;
        }
        if (last < first) {
            if (result != NULL) {
                result->first = first;
                result->last = last;
            }
            return SELECTION_EBACKWARDS;
        }
        if (mark != NULL) {
            mark(privateArg, first, last);
        }
        if (*at == ',') {
            at++;
            if (*at == '\0') {
                return SELECTION_ESYNTAX;           /* trailing comma */
            }
        }
    }
    return SELECTION_OK;
}

int32_t selectionSetList(selection *choice, const char *list, selectionResult *result)
{
    int32_t error;

    if (choice == NULL || list == NULL) {
        return SELECTION_EINVAL;
    }
    error = walkList(list, NULL, NULL, result);
    if (error == SELECTION_OK) {
        choice->list = list;
    }
    return error;
}

int32_t selectionAddPattern(selection *choice, const char *pattern)
{
    if (choice == NULL || pattern == NULL) {
        return SELECTION_EINVAL;
    }
    if (choice->patternCount == SELECTION_MAX_PATTERNS) {
        return SELECTION_EFULL;
    }
    choice->patterns[choice->patternCount++] = pattern;
    return SELECTION_OK;
}

int selectionActive(const selection *choice)
{
    return choice != NULL && (choice->list != NULL || choice->patternCount > 0);
}

void selectionFilePath(const parserFile *file, char *text, size_t size)
{
    const parserPathComponent *component;
    size_t                     used = 0;

    if (size == 0) {
        return;
    }
    text[0] = '\0';
    TAILQ_FOREACH(component, &file->path, entries) {
        int written = snprintf(text + used, size - used, "%s%s",
                               (component != TAILQ_FIRST(&file->path)) ? "/" : "",
                               component->name);

        if (written < 0 || (size_t)written >= size - used) {
            return;                                 /* truncated: good enough */
        }
        used += (size_t)written;
    }
}

static char lowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int selectionMatch(const char *pattern, const char *text)
{
    const char *starPattern = NULL;
    const char *starText = NULL;

    while (*text != '\0') {
        if (*pattern == '*') {
            starPattern = ++pattern;
            starText = text;
        } else if (*pattern == '?' ||
                   (*pattern != '\0' && lowerAscii(*pattern) == lowerAscii(*text))) {
            pattern++;
            text++;
        } else if (starPattern != NULL) {
            pattern = starPattern;
            text = ++starText;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') {
        pattern++;
    }
    return *pattern == '\0';
}

typedef struct marking {
    const parserTorrent *torrent;
    uint8_t             *wanted;
    uint32_t             files;
    uint64_t             missing;               /* first number with no file, or 0 */
    int                  hasMissing;
} marking;

static void markRange(void *privateArg, uint64_t first, uint64_t last)
{
    marking          *m = privateArg;
    const parserFile *file;
    uint32_t          index = 0;
    uint64_t          number = 0;

    if (m->hasMissing) {
        return;
    }
    if (first == 0 || last > m->files) {
        m->hasMissing = 1;
        m->missing = (first == 0 || first > m->files) ? first : last;
        return;
    }
    TAILQ_FOREACH(file, &m->torrent->files, entries) {
        if (!file->isPadding && ++number >= first && number <= last) {
            m->wanted[index] = 1;
        }
        index++;
    }
}

int32_t selectionApply(const selection *choice, const parserTorrent *torrent, uint8_t *wanted,
                       selectionResult *result)
{
    const parserFile *file;
    selectionResult   local;
    marking           m;
    uint32_t          index;
    uint32_t          i;
    char              path[4096];

    if (choice == NULL || torrent == NULL || wanted == NULL) {
        return SELECTION_EINVAL;
    }
    if (result == NULL) {
        result = &local;
    }
    memset(result, 0, sizeof(*result));
    memset(wanted, 0, torrent->fileCount);
    memset(&m, 0, sizeof(m));
    m.torrent = torrent;
    m.wanted = wanted;
    TAILQ_FOREACH(file, &torrent->files, entries) {
        m.files += file->isPadding ? 0u : 1u;
    }
    result->files = m.files;

    if (choice->list != NULL) {
        int32_t error = walkList(choice->list, markRange, &m, result);

        if (error != SELECTION_OK) {
            return error;
        }
        if (m.hasMissing) {
            result->number = m.missing;
            return SELECTION_ERANGE;
        }
    }
    for (i = 0; i < choice->patternCount; i++) {
        const char *pattern = choice->patterns[i];
        int         anywhere = strchr(pattern, '/') == NULL;
        uint32_t    matched = 0;

        index = 0;
        TAILQ_FOREACH(file, &torrent->files, entries) {
            if (!file->isPadding) {
                const char *name;

                selectionFilePath(file, path, sizeof(path));
                name = strrchr(path, '/');
                name = (name != NULL) ? name + 1 : path;
                if (selectionMatch(pattern, path) || (anywhere && selectionMatch(pattern, name))) {
                    wanted[index] = 1;
                    matched++;
                }
            }
            index++;
        }
        if (matched == 0) {
            result->unmatched |= 1u << i;
        }
    }

    index = 0;
    TAILQ_FOREACH(file, &torrent->files, entries) {
        if (!file->isPadding) {
            result->allBytes += file->length;
            if (wanted[index]) {
                result->chosen++;
                result->chosenBytes += file->length;
            }
        }
        index++;
    }
    return (result->chosen == 0) ? SELECTION_ENONE : SELECTION_OK;
}

const char *selectionErrorString(int32_t error)
{
    switch (error) {
    case SELECTION_OK:          return "success";
    case SELECTION_EINVAL:      return "invalid argument";
    case SELECTION_ESYNTAX:     return "expected file numbers like 1,3,5-7";
    case SELECTION_EBACKWARDS:  return "a range is backwards";
    case SELECTION_ERANGE:      return "no such file";
    case SELECTION_EFULL:       return "too many patterns";
    case SELECTION_ENONE:       return "no files chosen";
    default:                    return "unknown selection error";
    }
}
