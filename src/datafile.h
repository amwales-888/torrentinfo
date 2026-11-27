/*
 * datafile.h - files in the data directory, and whole-file reads and writes.
 *
 * The data directory is ~/.torrentinfo on Linux and FreeBSD,
 * ~/Library/Application Support/torrentinfo on macOS and
 * %APPDATA%\torrentinfo on Windows (see platformDataDirectory). In it:
 *
 *     config                       the settings file
 *     blocklist.txt                the default IP blocklist
 *     dht.dat                      the DHT routing table
 *     resume/<info hash>.resume    fast-resume data
 *     torrents/<info hash>.torrent torrents fetched for magnet links
 *
 * Errors are DATAFILE_ codes; for DATAFILE_EIO the libuv error is in
 * *systemError (when not NULL).
 */
#ifndef DATAFILE_H
#define DATAFILE_H

#include <stddef.h>
#include <stdint.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATAFILE_OK                 0
#define DATAFILE_EINVAL             (-1)
#define DATAFILE_ENOMEM             (-2)
#define DATAFILE_EIO                (-3)    /* see *systemError                    */
#define DATAFILE_ENAMETOOLONG       (-4)
#define DATAFILE_ENOTFILE           (-5)    /* not a regular file                  */
#define DATAFILE_ESIZE              (-6)    /* empty, or larger than allowed       */
#define DATAFILE_ENODIR             (-7)    /* no data directory                   */

#define DATAFILE_DHT_STATE          "dht.dat"
#define DATAFILE_BLOCKLIST          "blocklist.txt"

/* <data directory>[/subdirectory]/name; the directories are created. */
int32_t datafilePath(const char *subdirectory, const char *name, char *path, size_t size);

/* resume/<info hash in hex>.resume */
int32_t datafileResumePath(const uint8_t infoHash[PARSER_SHA1_LENGTH], char *path, size_t size);

/* torrents/<info hash in hex>.torrent */
int32_t datafileTorrentPath(const uint8_t infoHash[PARSER_SHA1_LENGTH], char *path, size_t size);

/* Read a whole regular file of 1 to maxSize bytes into a buffer from the
 * allocator (the caller frees it). */
int32_t datafileRead(const parserAllocator *allocator, const char *path, uint64_t maxSize,
                     uint8_t **data, uint64_t *length, int32_t *systemError);

/* Write a whole file, replacing it. */
int32_t datafileWrite(const char *path, const uint8_t *data, uint64_t length,
                      int32_t *systemError);

/* Write "<path>.tmp", then rename it over path, so a reader sees either the
 * old file or the new one, never a partial one. */
int32_t datafileWriteAtomic(const char *path, const uint8_t *data, uint64_t length,
                            int32_t *systemError);

const char *datafileErrorString(int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* DATAFILE_H */
