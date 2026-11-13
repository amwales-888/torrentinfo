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
 *   max_download_rate  = 2M             speed limits (see configParseRate();
 *   max_upload_rate    = 500k           0: no limit)
 *   encryption         = prefer         protocol encryption: prefer, require
 *                                       or off
 *   web_seeds          = yes            download from web seeds (BEP 19)
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
#define CONFIG_SET_MAX_DOWNLOAD_RATE    0x100u
#define CONFIG_SET_MAX_UPLOAD_RATE      0x200u
#define CONFIG_SET_ENCRYPTION           0x400u
#define CONFIG_SET_WEB_SEEDS            0x800u

#define CONFIG_ENCRYPTION_OFF           0u      /* the values of 'encryption' */
#define CONFIG_ENCRYPTION_PREFER        1u
#define CONFIG_ENCRYPTION_REQUIRE       2u

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
    uint64_t    maxDownloadRate;                /* bytes per second; 0: none */
    uint64_t    maxUploadRate;
    uint8_t     encryption;                     /* CONFIG_ENCRYPTION_*       */
    uint8_t     webSeeds;
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

/* A speed as bytes per second: a number, possibly with a fraction, and an
 * optional unit k, m or g (KiB/s, MiB/s, GiB/s; any case, "/s" optional).
 * A plain number is KiB/s. "0" (or "none", "unlimited") means no limit.
 * 1 if understood. At most 100 GiB/s. */
int configParseRate(const char *text, uint64_t *bytesPerSecond);

/* "prefer", "require" or "off" (any case) as CONFIG_ENCRYPTION_*; 1 if understood. */
int configParseEncryption(const char *text, uint8_t *mode);

/* Write a commented file listing every setting, all commented out. */
int32_t configWriteTemplate(const char *path, int32_t *systemError);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
