/*
 * config.c - the settings file. See config.h.
 */
#include <stdio.h>
#include <string.h>
#include <uv.h>

#include "config.h"
#include "platform.h"

static const char configTemplate[] =
    "# torrentinfo settings\n"
    "#\n"
    "# One \"key = value\" per line; lines starting with # are ignored. Remove\n"
    "# the # in front of a setting to use it. Command-line options win over\n"
    "# what is set here.\n"
    "\n"
    "# IP blocklist: peers and DHT nodes at these addresses are never contacted\n"
    "# or accepted. One address, CIDR block (10.0.0.0/8) or range\n"
    "# (1.2.3.0-1.2.3.255) per line; PeerGuardian (.p2p) and eMule ipfilter.dat\n"
    "# lists work as they are. A relative path is taken from this directory;\n"
    "# \"none\" turns it off. Default: blocklist.txt, if it exists.\n"
    "#blocklist = blocklist.txt\n"
    "\n"
    "# Port to accept connections from peers on (and for the DHT); 0: do not\n"
    "# accept connections. Like -L.\n"
    "#listen_port = 6881\n"
    "\n"
    "# Ask the router to forward the port (NAT-PMP or UPnP). \"no\" is like -N.\n"
    "#port_forwarding = yes\n"
    "\n"
    "# Use the DHT to find peers. \"no\" is like -X.\n"
    "#dht = yes\n"
    "\n"
    "# Routers to join the DHT through, as host:port separated by commas.\n"
    "# Default: the well-known public ones.\n"
    "#dht_bootstrap = router.bittorrent.com:6881, dht.transmissionbt.com:6881\n"
    "\n"
    "# Where downloads are saved (and where -V looks), unless -O is given.\n"
    "# ~/ means your home directory. Default: the current directory.\n"
    "#download_directory = ~/Downloads\n"
    "\n"
    "# Peers to be connected to at once, per torrent (1 to 1000).\n"
    "#max_peers = 50\n"
    "\n"
    "# Peers to upload to at once (1 to 100).\n"
    "#upload_slots = 4\n"
    "\n"
    "# Speed limits, in KiB/s, or with a unit: 500k, 1.5M. 0: no limit.\n"
    "# Like -d and -u.\n"
    "#max_download_rate = 0\n"
    "#max_upload_rate = 0\n";

typedef struct lineContext {
    configWarningCallback warning;
    void                 *privateArg;
    uint32_t              line;
} lineContext;

static void warn(lineContext *lc, const char *format, const char *a, const char *b)
{
    char message[CONFIG_MAX_VALUE + 256u];

    if (lc->warning == NULL) {
        return;
    }
    snprintf(message, sizeof(message), format, a, b);
    lc->warning(lc->privateArg, lc->line, message);
}

static int isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int sameKey(const char *key, size_t length, const char *name)
{
    return strlen(name) == length && memcmp(key, name, length) == 0;
}

/* Copy the value [start, end) into out, removing quotes and expanding a
 * leading "~/" when 'path'. 1 if it fits and is well formed. */
static int readValue(const char *start, const char *end, char *out, size_t size, int path,
                     const char **problem)
{
    size_t used = 0;

    *problem = "value too long";
    if (start < end && *start == '"') {
        const char *at = start + 1;

        while (at < end && *at != '"') {
            char c = *at++;

            if (c == '\\' && at < end && (*at == '"' || *at == '\\')) {
                c = *at++;
            }
            if (used + 1u >= size) {
                return 0;
            }
            out[used++] = c;
        }
        if (at >= end) {
            *problem = "missing closing quote";
            return 0;
        }
        at++;
        while (at < end && isSpace(*at)) {
            at++;
        }
        if (at < end && *at != '#') {
            *problem = "text after the closing quote";
            return 0;
        }
        out[used] = '\0';
    } else {
        const char *stop = start;

        /* An unquoted value ends at a '#' preceded by a space. */
        while (stop < end && !(*stop == '#' && stop > start && isSpace(stop[-1]))) {
            stop++;
        }
        while (stop > start && isSpace(stop[-1])) {
            stop--;
        }
        if ((size_t)(stop - start) + 1u > size) {
            return 0;
        }
        memcpy(out, start, (size_t)(stop - start));
        used = (size_t)(stop - start);
        out[used] = '\0';
    }
    if (path && (strcmp(out, "~") == 0 ||
                 (out[0] == '~' && (out[1] == '/' || out[1] == '\\')))) {
        char   home[CONFIG_MAX_VALUE];
        size_t homeLength = sizeof(home);

        if (uv_os_homedir(home, &homeLength) != 0) {
            *problem = "cannot find the home directory for ~";
            return 0;
        }
        if (homeLength + used > size) {             /* "~" replaced by the home */
            return 0;
        }
        memmove(out + homeLength, out + 1, used);   /* with the terminator */
        memcpy(out, home, homeLength);
    }
    return 1;
}

static int readBoolean(const char *value, uint8_t *out)
{
    static const char *const yes[] = { "yes", "true", "on", "1" };
    static const char *const no[] = { "no", "false", "off", "0" };
    char     lower[8];
    size_t   i;

    for (i = 0; value[i] != '\0' && i + 1u < sizeof(lower); i++) {
        lower[i] = (char)((value[i] >= 'A' && value[i] <= 'Z') ? value[i] + 32 : value[i]);
    }
    if (value[i] != '\0') {
        return 0;
    }
    lower[i] = '\0';
    for (i = 0; i < 4u; i++) {
        if (strcmp(lower, yes[i]) == 0) {
            *out = 1;
            return 1;
        }
        if (strcmp(lower, no[i]) == 0) {
            *out = 0;
            return 1;
        }
    }
    return 0;
}

static int readNumber(const char *value, uint32_t minimum, uint32_t maximum, uint32_t *out)
{
    uint32_t number = 0;
    size_t   i;

    if (value[0] == '\0') {
        return 0;
    }
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] < '0' || value[i] > '9' || number > 100000000u) {
            return 0;
        }
        number = number * 10u + (uint32_t)(value[i] - '0');
    }
    if (number < minimum || number > maximum) {
        return 0;
    }
    *out = number;
    return 1;
}

int configParseRate(const char *text, uint64_t *bytesPerSecond)
{
    uint64_t whole = 0;
    uint64_t fraction = 0;
    uint64_t scale = 1;                         /* 10^digits of the fraction */
    uint64_t unit = 1024u;
    int      digits = 0;
    char     c;

    if (text == NULL || bytesPerSecond == NULL) {
        return 0;
    }
    if (strcmp(text, "none") == 0 || strcmp(text, "unlimited") == 0) {
        *bytesPerSecond = 0;
        return 1;
    }
    while (*text >= '0' && *text <= '9') {
        if (whole > 100000000u) {
            return 0;
        }
        whole = whole * 10u + (uint64_t)(*text++ - '0');
        digits++;
    }
    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9') {
            if (scale < 1000000u) {
                fraction = fraction * 10u + (uint64_t)(*text - '0');
                scale *= 10u;
            }
            text++;
            digits++;
        }
    }
    if (digits == 0) {
        return 0;
    }
    c = *text;
    if (c == 'k' || c == 'K') {
        text++;
    } else if (c == 'm' || c == 'M') {
        unit = 1024u * 1024u;
        text++;
    } else if (c == 'g' || c == 'G') {
        unit = 1024u * 1024u * 1024u;
        text++;
    }
    if (*text == 'i' || *text == 'I') {
        text++;                                 /* KiB */
    }
    if (*text == 'b' || *text == 'B') {
        text++;
    }
    if (text[0] == '/' && (text[1] == 's' || text[1] == 'S')) {
        text += 2;
    }
    if (*text != '\0') {
        return 0;
    }
    if (whole > ((uint64_t)100u * 1024u * 1024u * 1024u) / unit) {
        return 0;
    }
    *bytesPerSecond = whole * unit + fraction * unit / scale;
    if (*bytesPerSecond == 0 && (whole != 0 || fraction != 0)) {
        *bytesPerSecond = 1;                    /* a tiny limit, not none */
    }
    return 1;
}

static void parseSetting(lineContext *lc, configSettings *s, const char *key, size_t keyLength,
                         const char *start, const char *end)
{
    char        value[CONFIG_MAX_VALUE];
    char        keyText[64];
    const char *problem;
    uint32_t    number;
    int         isPath = sameKey(key, keyLength, "blocklist") ||
                         sameKey(key, keyLength, "download_directory");

    snprintf(keyText, sizeof(keyText), "%.*s", (int)((keyLength < 60u) ? keyLength : 60u), key);
    if (!readValue(start, end, value, sizeof(value), isPath, &problem)) {
        warn(lc, "%s: %s", keyText, problem);
        return;
    }
    if (sameKey(key, keyLength, "blocklist")) {
        if (strcmp(value, "none") == 0) {
            value[0] = '\0';
        } else if (value[0] == '\0') {
            warn(lc, "%s: no file given (use \"none\" for no blocklist)%s", keyText, "");
            return;
        }
        memcpy(s->blocklist, value, sizeof(value));
        s->set |= CONFIG_SET_BLOCKLIST;
    } else if (sameKey(key, keyLength, "listen_port")) {
        if (!readNumber(value, 0u, 65535u, &number)) {
            warn(lc, "%s: not a port number: %s", keyText, value);
            return;
        }
        s->listenPort = (uint16_t)number;
        s->set |= CONFIG_SET_LISTEN_PORT;
    } else if (sameKey(key, keyLength, "port_forwarding") || sameKey(key, keyLength, "dht")) {
        uint8_t flag;

        if (!readBoolean(value, &flag)) {
            warn(lc, "%s: use yes or no, not %s", keyText, value);
            return;
        }
        if (keyLength == 3u) {
            s->dht = flag;
            s->set |= CONFIG_SET_DHT;
        } else {
            s->portForwarding = flag;
            s->set |= CONFIG_SET_PORT_FORWARDING;
        }
    } else if (sameKey(key, keyLength, "dht_bootstrap")) {
        char  *out = s->dhtBootstrap;
        size_t i;

        for (i = 0; value[i] != '\0'; i++) {        /* drop spaces around commas */
            if (!isSpace(value[i])) {
                *out++ = value[i];
            }
        }
        *out = '\0';
        if (s->dhtBootstrap[0] == '\0') {
            warn(lc, "%s: no routers given%s", keyText, "");
            return;
        }
        s->set |= CONFIG_SET_DHT_BOOTSTRAP;
    } else if (sameKey(key, keyLength, "download_directory")) {
        memcpy(s->downloadDirectory, value, sizeof(value));
        s->set |= CONFIG_SET_DOWNLOAD_DIRECTORY;
    } else if (sameKey(key, keyLength, "max_peers")) {
        if (!readNumber(value, 1u, 1000u, &number)) {
            warn(lc, "%s: use a number from 1 to 1000, not %s", keyText, value);
            return;
        }
        s->maxPeers = number;
        s->set |= CONFIG_SET_MAX_PEERS;
    } else if (sameKey(key, keyLength, "upload_slots")) {
        if (!readNumber(value, 1u, 100u, &number)) {
            warn(lc, "%s: use a number from 1 to 100, not %s", keyText, value);
            return;
        }
        s->uploadSlots = number;
        s->set |= CONFIG_SET_UPLOAD_SLOTS;
    } else if (sameKey(key, keyLength, "max_download_rate") ||
               sameKey(key, keyLength, "max_upload_rate")) {
        uint64_t rate;

        if (!configParseRate(value, &rate)) {
            warn(lc, "%s: not a speed (examples: 500k, 2M, 0 for no limit): %s", keyText,
                 value);
            return;
        }
        if (keyText[4] == 'd') {
            s->maxDownloadRate = rate;
            s->set |= CONFIG_SET_MAX_DOWNLOAD_RATE;
        } else {
            s->maxUploadRate = rate;
            s->set |= CONFIG_SET_MAX_UPLOAD_RATE;
        }
    } else {
        warn(lc, "unknown setting \"%s\"%s", keyText, "");
    }
}

int32_t configParse(const char *text, size_t length, configSettings *settings,
                    configWarningCallback warning, void *privateArg)
{
    lineContext lc;
    const char *at = text;
    const char *end = text + length;

    if (settings == NULL || (text == NULL && length > 0)) {
        return CONFIG_EINVAL;
    }
    memset(settings, 0, sizeof(*settings));
    lc.warning = warning;
    lc.privateArg = privateArg;
    lc.line = 0;
    /* A UTF-8 byte order mark, as Windows editors like to write. */
    if (length >= 3u && memcmp(text, "\xEF\xBB\xBF", 3) == 0) {
        at += 3;
    }
    while (at < end) {
        const char *lineEnd = memchr(at, '\n', (size_t)(end - at));
        const char *start = at;
        const char *stop;
        const char *equals;
        const char *keyEnd;
        const char *valueStart;

        if (lineEnd == NULL) {
            lineEnd = end;
        }
        stop = lineEnd;
        at = (lineEnd < end) ? lineEnd + 1 : end;
        lc.line++;
        while (start < stop && isSpace(*start)) {
            start++;
        }
        while (stop > start && isSpace(stop[-1])) {
            stop--;
        }
        if (start == stop || *start == '#' || *start == ';') {
            continue;
        }
        equals = memchr(start, '=', (size_t)(stop - start));
        if (equals == NULL) {
            warn(&lc, "expected \"key = value\"%s%s", "", "");
            continue;
        }
        keyEnd = equals;
        while (keyEnd > start && isSpace(keyEnd[-1])) {
            keyEnd--;
        }
        valueStart = equals + 1;
        while (valueStart < stop && isSpace(*valueStart)) {
            valueStart++;
        }
        if (keyEnd == start) {
            warn(&lc, "no setting name before '='%s%s", "", "");
            continue;
        }
        parseSetting(&lc, settings, start, (size_t)(keyEnd - start), valueStart, stop);
    }
    return CONFIG_OK;
}

int32_t configLoad(parserContext *context, const char *path, configSettings *settings,
                   configWarningCallback warning, void *privateArg, int32_t *systemError)
{
    platformFile file;
    char        *text;
    uint64_t     size;
    uint8_t      isRegular;
    uint32_t     got = 0;
    int32_t      result;

    if (systemError != NULL) {
        *systemError = 0;
    }
    if (context == NULL || path == NULL || settings == NULL) {
        return CONFIG_EINVAL;
    }
    memset(settings, 0, sizeof(*settings));
    result = platformFileStat(path, &size, &isRegular);
    if (result == 0 && (!isRegular || size > CONFIG_MAX_FILE)) {
        result = UV_EINVAL;
    }
    if (result == 0) {
        result = platformFileOpenRead(path, &file);
    }
    if (result != 0) {
        if (systemError != NULL) {
            *systemError = result;
        }
        return CONFIG_EIO;
    }
    text = context->allocator.realloc(NULL, (size_t)size + 1u);
    if (text == NULL) {
        platformFileClose(&file);
        return CONFIG_ENOMEM;
    }
    {
        uint64_t done = 0;

        result = 0;
        while (done < size) {
            result = platformFileRead(&file, text + done, (uint32_t)(size - done), &got);
            if (result != 0 || got == 0) {
                break;
            }
            done += got;
        }
        got = (uint32_t)done;
    }
    platformFileClose(&file);
    if (result != 0 || got != size) {
        context->allocator.free(text);
        if (systemError != NULL) {
            *systemError = (result != 0) ? result : UV_EIO;
        }
        return CONFIG_EIO;
    }
    result = configParse(text, (size_t)size, settings, warning, privateArg);
    context->allocator.free(text);
    return result;
}

int32_t configWriteTemplate(const char *path, int32_t *systemError)
{
    platformFile file;
    uint32_t     written = 0;
    int32_t      result;

    if (systemError != NULL) {
        *systemError = 0;
    }
    if (path == NULL) {
        return CONFIG_EINVAL;
    }
    result = platformFileOpenWrite(path, &file);
    if (result == 0) {
        uint32_t total = (uint32_t)(sizeof(configTemplate) - 1u);
        uint32_t done = 0;

        while (result == 0 && done < total) {
            result = platformFileWrite(&file, configTemplate + done, total - done, &written);
            if (result == 0 && written == 0) {
                result = UV_EIO;
            }
            done += written;
        }
        if (platformFileClose(&file) != 0 && result == 0) {
            result = UV_EIO;
        }
    }
    if (result != 0) {
        if (systemError != NULL) {
            *systemError = result;
        }
        return CONFIG_EIO;
    }
    return CONFIG_OK;
}
