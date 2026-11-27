/*
 * aTorrent.h - the public interface of libaTorrent.
 *
 * Everything a program calls is named aTorrent... and marked ATORRENT_API.
 * The shared libraries (libaTorrent.so, libaTorrent.dylib, libaTorrent.dll)
 * export only those names; the rest of the library is internal and may
 * change between versions.
 *
 * Linking:
 *   static (libaTorrent.a, libaTorrent.lib)   nothing to define
 *   Windows DLL (libaTorrent.dll)             define ATORRENT_DLL before
 *                                             including this header, and link
 *                                             with libaTorrent.dll.lib
 *   .so / .dylib                              nothing to define
 *
 * The library builds itself with ATORRENT_BUILDING_DLL for the Windows DLL
 * and with -fvisibility=hidden elsewhere.
 *
 * This is the first piece of the API: the session (adding, pausing and
 * removing torrents, limits, status and events) follows; see DAEMON.md.
 */
#ifndef ATORRENT_H
#define ATORRENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Export marker                                                            */
/* ------------------------------------------------------------------------ */

#if defined(_WIN32)
#  if defined(ATORRENT_BUILDING_DLL)
#    define ATORRENT_API __declspec(dllexport)
#  elif defined(ATORRENT_DLL)
#    define ATORRENT_API __declspec(dllimport)
#  else
#    define ATORRENT_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define ATORRENT_API __attribute__((visibility("default")))
#else
#  define ATORRENT_API
#endif

/* ------------------------------------------------------------------------ */
/* Version                                                                  */
/* ------------------------------------------------------------------------ */

#define ATORRENT_VERSION_MAJOR      0
#define ATORRENT_VERSION_MINOR      1
#define ATORRENT_VERSION_PATCH      0
#define ATORRENT_VERSION_STRING     "0.1.0"

/* MAJOR * 10000 + MINOR * 100 + PATCH, for comparisons. */
#define ATORRENT_VERSION \
    (ATORRENT_VERSION_MAJOR * 10000 + ATORRENT_VERSION_MINOR * 100 + ATORRENT_VERSION_PATCH)

/* The version of the library actually loaded, which for a shared library may
 * differ from the header a program was built with. */
ATORRENT_API uint32_t    aTorrentVersion(void);
ATORRENT_API const char *aTorrentVersionString(void);

/* ------------------------------------------------------------------------ */
/* Events                                                                   */
/* ------------------------------------------------------------------------ */

/* Names for the levels and codes in event.h ("warning", "peer-connected"),
 * for programs that log events. */
ATORRENT_API const char *aTorrentEventLevelName(uint32_t level);
ATORRENT_API const char *aTorrentEventCodeName(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* ATORRENT_H */
