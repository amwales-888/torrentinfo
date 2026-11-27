/*
 * event.c - names for event levels and codes. See event.h.
 */
#include "event.h"

const char *eventLevelName(uint32_t level)
{
    switch (level) {
    case EVENT_LEVEL_ERROR:     return "error";
    case EVENT_LEVEL_WARNING:   return "warning";
    case EVENT_LEVEL_INFO:      return "info";
    case EVENT_LEVEL_DEBUG:     return "debug";
    default:                    return "unknown";
    }
}

const char *eventCodeName(uint32_t code)
{
    static const char *const names[EVENT_CODE_COUNT] = {
        "none",
        "endgame",
        "complete",
        "piece-failed",
        "piece-read-failed",
        "peer-connected",
        "peer-connect-failed",
        "peer-no-handshake",
        "peer-dropped",
        "peer-excluded",
        "pex-peers",
        "webseed-started",
        "webseed-retry",
        "webseed-corrupt",
        "webseed-disabled",
        "webseed-unusable",
        "webseeds-unavailable",
        "resume-loaded",
        "resume-save-failed",
        "metadata-size",
        "metadata-mismatch",
        "metadata-refused",
        "metadata-peer",
    };

    return (code < EVENT_CODE_COUNT) ? names[code] : "unknown";
}
