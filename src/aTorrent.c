/*
 * aTorrent.c - the public entry points of libaTorrent. See aTorrent.h.
 */
#include "aTorrent.h"
#include "event.h"

ATORRENT_API uint32_t aTorrentVersion(void)
{
    return (uint32_t)ATORRENT_VERSION;
}

ATORRENT_API const char *aTorrentVersionString(void)
{
    return ATORRENT_VERSION_STRING;
}

ATORRENT_API const char *aTorrentEventLevelName(uint32_t level)
{
    return eventLevelName(level);
}

ATORRENT_API const char *aTorrentEventCodeName(uint32_t code)
{
    return eventCodeName(code);
}
