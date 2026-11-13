/*
 * config.h - the settings file.
 *
 * Plain text, one "key = value" per line. Blank lines and lines starting
 * with '#' or ';' are ignored, as is anything after a '#' that follows a
 * space or tab. A value may be put in double quotes (to keep a '#' or
 * surrounding spaces); inside quotes, \" and \\ stand for " and \.
 *
 *   blocklist          = blocklist.txt  path of the IP blocklist ("none": no
 *                                       blocklist); relative paths are taken
 *                                       from the data directory
 *   listen_port        = 6881           TCP/UDP port to accept peers on (0: none)
 *   port_forwarding    = yes            ask the router to forward the port
 *   dht                = yes            use the DHT to find peers
 *   dht_bootstrap      = host:port, ... routers for joining the DHT
 *   download_directory = ~/Downloads    where downloads go ("~/" is the home
 *                                       directory)
 *   max_peers          = 50             connections per torrent (1 to 1000)
 *   upload_slots       = 4              peers uploaded to at once (1 to 100)
 *
 * Booleans are yes/no, true/false, on/off or 1/0. Unknown keys and bad
 * values are reported (with their line number) and otherwise ignored, so an
 * older or newer file never stops the program.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_OK               0
#define CONFIG_EINVAL           (-3)
#define CONFIG_ENOMEM           (-4)
#define CONFIG_EIO              (-5)

#define CONFIG_MAX_VALUE        1024u           /* bytes, with the terminator */
#define CONFIG_MAX_FILE         (1024u * 1024u)
#define CONFIG_FILE_NAME        "config"

/* Bits of configSettings.set: which settings the file gave. */
#define CONFIG_SET_BLOCKLIST            0x01u
#define CONFIG_SET_LISTEN_PORT          0x02u
#define CONFIG_SET_PORT_FORWARDING      0x04u
#define CONFIG_SET_DHT                  0x08u
#define CONFIG_SET_DHT_BOOTSTRAP        0x10u
#define CONFIG_SET_DOWNLOAD_DIRECTORY   0x20u
#define CONFIG_SET_MAX_PEERS            0x40u
#define CONFIG_SET_UPLOAD_SLOTS         0x80u

typedef struct configSettings {
    uint32_t    set;                            /* CONFIG_SET_ bits       */
    char        blocklist[CONFIG_MAX_VALUE];    /* "" for "none"          */
    uint16_t    listenPort;
    uint8_t     portForwarding;
    uint8_t     dht;
    char        dhtBootstrap[CONFIG_MAX_VALUE]; /* "host:port,host:port"  */
    char        downloadDirectory[CONFIG_MAX_VALUE];
    uint32_t    maxPeers;
    uint32_t    uploadSlots;
} configSettings;

/* Told about each line that is not understood. */
typedef void (*configWarningCallback)(void *privateArg, uint32_t line, const char *message);

/* Parse settings into *settings (cleared first). */
int32_t configParse(const char *text, size_t length, configSettings *settings,
                    configWarningCallback warning, void *privateArg);

/* Read and parse a file. CONFIG_EIO (the libuv error in *systemError, if not
 * NULL) if it cannot be read: UV_ENOENT if it does not exist. */
int32_t configLoad(parserContext *context, const char *path, configSettings *settings,
                   configWarningCallback warning, void *privateArg, int32_t *systemError);

/* Write a commented file listing every setting, all commented out. */
int32_t configWriteTemplate(const char *path, int32_t *systemError);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
