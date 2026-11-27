/*
 * settings.h - the settings a session runs with.
 *
 * Three layers, each overriding the one before:
 *
 *     1. built-in defaults               settingsDefaults()
 *     2. the settings file               config.c parses it into a configSettings
 *     3. the caller's overrides          also a configSettings, e.g. from options
 *
 * settingsMerge() combines them; a layer only counts for the settings whose
 * CONFIG_SET_ bit it has. Nothing here prints, and there are no globals: a
 * program (or a daemon running many torrents) keeps one settings value and
 * hands it to what needs it.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#include "blocklist.h"
#include "config.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_OK                 0
#define SETTINGS_EINVAL             (-1)
#define SETTINGS_ENOMEM             (-2)
#define SETTINGS_EIO                (-3)    /* systemError holds the libuv error   */
#define SETTINGS_ENAMETOOLONG       (-4)

#define SETTINGS_DEFAULT_LISTEN_PORT    6881u
#define SETTINGS_MAX_BOOTSTRAP          8u  /* routers taken from dht_bootstrap     */

typedef struct settingsValues {
    char     blocklist[CONFIG_MAX_VALUE];   /* "" : none; see settingsLoadBlocklist */
    uint8_t  blocklistNamed;                /* given by a layer (else the default
                                               blocklist.txt, if there is one)      */
    uint16_t listenPort;                    /* 0: do not accept peers               */
    uint8_t  portForwarding;
    uint8_t  dht;
    char     dhtBootstrap[CONFIG_MAX_VALUE];        /* "" : the well-known routers   */
    char     downloadDirectory[CONFIG_MAX_VALUE];   /* "" : the current directory    */
    uint32_t maxPeers;                      /* 0: the download module's default     */
    uint32_t uploadSlots;                   /* 0: the download module's default     */
    uint64_t maxDownloadRate;               /* bytes per second; 0: no limit         */
    uint64_t maxUploadRate;
    uint8_t  encryption;                    /* CONFIG_ENCRYPTION_*                  */
    uint8_t  webSeeds;
    uint32_t fromFile;                      /* CONFIG_SET_ bits the file supplied   */
    uint32_t fromOverrides;                 /* ...and the overrides                 */
} settingsValues;

/* What settingsLoadFile did. */
#define SETTINGS_FILE_READ          0u      /* read (maybe with warnings)           */
#define SETTINGS_FILE_CREATED       1u      /* missing: a commented template was
                                               written; defaults apply              */
#define SETTINGS_FILE_NONE          2u      /* no data directory: defaults apply    */

void settingsDefaults(settingsValues *out);

/* defaults <- file <- overrides; either may be NULL. */
void settingsMerge(settingsValues *out, const configSettings *file,
                   const configSettings *overrides);

/*
 * Read the settings file into *file. With path NULL, the file is "config" in
 * the data directory and is created as a commented template listing every
 * setting if it does not exist (*what says which). A named file that is
 * missing is an error. On error *file is cleared, so defaults apply.
 */
int32_t settingsLoadFile(parserContext *context, const char *path, configSettings *file,
                         configWarningCallback warning, void *privateArg, uint32_t *what,
                         char *usedPath, size_t usedPathSize, int32_t *systemError);

/*
 * Load the IP blocklist the settings name, or blocklist.txt in the data
 * directory if they name none and it exists. Relative paths are taken from
 * the data directory. *list is NULL if there is no list, or it is empty
 * (stats still describe what was read). The path read is put in usedPath,
 * which is "" when there was no list to read (blocklist = none, or no
 * blocklist.txt).
 * A list the settings name that cannot be read is an error: better not to
 * run unprotected.
 */
int32_t settingsLoadBlocklist(parserContext *context, const settingsValues *values,
                              blocklist **list, blocklistStats *stats, char *usedPath, size_t usedPathSize,
                              int32_t *systemError);

/*
 * Split "host:port,host:port" into list[] (ending in NULL, at most
 * SETTINGS_MAX_BOOTSTRAP entries) using storage for the text. Returns the
 * number of routers; 0 (and list[0] NULL) for an empty text.
 */
uint32_t settingsSplitBootstrap(const char *text, char *storage, size_t storageSize,
                                const char *list[SETTINGS_MAX_BOOTSTRAP + 1u]);

/* Is the path absolute on this system? */
int settingsIsAbsolutePath(const char *path);

const char *settingsErrorString(int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
