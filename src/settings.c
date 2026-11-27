/*
 * settings.c - defaults, the settings file and overrides. See settings.h.
 */
#include <stdio.h>
#include <string.h>

#include "datafile.h"
#include "platform.h"
#include "settings.h"

void settingsDefaults(settingsValues *out)
{
    memset(out, 0, sizeof(*out));
    out->listenPort = SETTINGS_DEFAULT_LISTEN_PORT;
    out->portForwarding = 1;
    out->dht = 1;
    out->encryption = CONFIG_ENCRYPTION_PREFER;
    out->webSeeds = 1;
}

static void copyText(char *to, const char *from)
{
    snprintf(to, CONFIG_MAX_VALUE, "%s", from);
}

/* Take every setting the layer gives. */
static void apply(settingsValues *out, const configSettings *layer)
{
    uint32_t set = layer->set;

    if (set & CONFIG_SET_BLOCKLIST) {
        copyText(out->blocklist, layer->blocklist);
        out->blocklistNamed = 1;
    }
    if (set & CONFIG_SET_LISTEN_PORT) {
        out->listenPort = layer->listenPort;
    }
    if (set & CONFIG_SET_PORT_FORWARDING) {
        out->portForwarding = layer->portForwarding;
    }
    if (set & CONFIG_SET_DHT) {
        out->dht = layer->dht;
    }
    if (set & CONFIG_SET_DHT_BOOTSTRAP) {
        copyText(out->dhtBootstrap, layer->dhtBootstrap);
    }
    if (set & CONFIG_SET_DOWNLOAD_DIRECTORY) {
        copyText(out->downloadDirectory, layer->downloadDirectory);
    }
    if (set & CONFIG_SET_MAX_PEERS) {
        out->maxPeers = layer->maxPeers;
    }
    if (set & CONFIG_SET_UPLOAD_SLOTS) {
        out->uploadSlots = layer->uploadSlots;
    }
    if (set & CONFIG_SET_MAX_DOWNLOAD_RATE) {
        out->maxDownloadRate = layer->maxDownloadRate;
    }
    if (set & CONFIG_SET_MAX_UPLOAD_RATE) {
        out->maxUploadRate = layer->maxUploadRate;
    }
    if (set & CONFIG_SET_ENCRYPTION) {
        out->encryption = layer->encryption;
    }
    if (set & CONFIG_SET_WEB_SEEDS) {
        out->webSeeds = layer->webSeeds;
    }
}

void settingsMerge(settingsValues *out, const configSettings *file, const configSettings *overrides)
{
    settingsDefaults(out);
    if (file != NULL) {
        apply(out, file);
        out->fromFile = file->set;
    }
    if (overrides != NULL) {
        apply(out, overrides);
        out->fromOverrides = overrides->set;
    }
}

int32_t settingsLoadFile(parserContext *context, const char *path, configSettings *file,
                         configWarningCallback warning, void *privateArg, uint32_t *what,
                         char *usedPath, size_t usedPathSize, int32_t *systemError)
{
    int32_t error = 0;
    int32_t result;

    if (context == NULL || file == NULL || what == NULL || usedPath == NULL || usedPathSize == 0) {
        return SETTINGS_EINVAL;
    }
    memset(file, 0, sizeof(*file));
    *what = SETTINGS_FILE_READ;
    if (systemError != NULL) {
        *systemError = 0;
    }
    if (path != NULL) {
        if ((size_t)snprintf(usedPath, usedPathSize, "%s", path) >= usedPathSize) {
            return SETTINGS_ENAMETOOLONG;
        }
    } else {
        result = datafilePath(NULL, CONFIG_FILE_NAME, usedPath, usedPathSize);
        if (result == DATAFILE_ENODIR) {
            *what = SETTINGS_FILE_NONE;
            return SETTINGS_OK;
        }
        if (result != DATAFILE_OK) {
            return SETTINGS_ENAMETOOLONG;
        }
    }
    result = configLoad(context, usedPath, file, warning, privateArg, &error);
    if (result == CONFIG_OK) {
        return SETTINGS_OK;
    }
    memset(file, 0, sizeof(*file));
    if (result == CONFIG_ENOMEM) {
        return SETTINGS_ENOMEM;
    }
    if (error == UV_ENOENT && path == NULL) {
        /* First run: write the template so the user can find every setting. */
        if (configWriteTemplate(usedPath, &error) == CONFIG_OK) {
            *what = SETTINGS_FILE_CREATED;
            return SETTINGS_OK;
        }
    }
    if (systemError != NULL) {
        *systemError = error;
    }
    return SETTINGS_EIO;
}

int settingsIsAbsolutePath(const char *path)
{
#ifdef _WIN32
    return path[0] == '\\' || path[0] == '/' || (path[0] != '\0' && path[1] == ':');
#else
    return path[0] == '/';
#endif
}

int32_t settingsLoadBlocklist(parserContext *context, const settingsValues *values,
                              blocklist **list, blocklistStats *stats, char *usedPath, size_t usedPathSize,
                              int32_t *systemError)
{
    const char *name;
    int32_t     error = 0;
    int32_t     result;

    if (context == NULL || values == NULL || list == NULL || stats == NULL || usedPath == NULL ||
        usedPathSize == 0) {
        return SETTINGS_EINVAL;
    }
    name = values->blocklistNamed ? values->blocklist : DATAFILE_BLOCKLIST;
    *list = NULL;
    memset(stats, 0, sizeof(*stats));
    usedPath[0] = '\0';
    if (systemError != NULL) {
        *systemError = 0;
    }
    if (values->blocklistNamed && values->blocklist[0] == '\0') {
        return SETTINGS_OK;                         /* blocklist = none */
    }
    if (settingsIsAbsolutePath(name)) {
        if ((size_t)snprintf(usedPath, usedPathSize, "%s", name) >= usedPathSize) {
            return SETTINGS_ENAMETOOLONG;
        }
    } else {
        result = datafilePath(NULL, name, usedPath, usedPathSize);
        if (result != DATAFILE_OK) {
            if (!values->blocklistNamed) {
                usedPath[0] = '\0';
                return SETTINGS_OK;                 /* no data directory: no list */
            }
            return (result == DATAFILE_ENAMETOOLONG) ? SETTINGS_ENAMETOOLONG : SETTINGS_EIO;
        }
    }
    result = blocklistLoad(context, usedPath, list, stats, &error);
    if (result == BLOCKLIST_EIO && error == UV_ENOENT && !values->blocklistNamed) {
        *list = NULL;
        usedPath[0] = '\0';
        return SETTINGS_OK;                         /* no default list: nothing blocked */
    }
    if (result != BLOCKLIST_OK) {
        *list = NULL;
        if (result == BLOCKLIST_EIO) {
            if (systemError != NULL) {
                *systemError = error;
            }
            return SETTINGS_EIO;
        }
        return SETTINGS_ENOMEM;
    }
    if (stats->ranges4 == 0 && stats->ranges6 == 0) {
        blocklistFree(context, *list);              /* nothing in it */
        *list = NULL;
    }
    return SETTINGS_OK;
}

uint32_t settingsSplitBootstrap(const char *text, char *storage, size_t storageSize,
                                const char *list[SETTINGS_MAX_BOOTSTRAP + 1u])
{
    uint32_t count = 0;
    char    *at;

    list[0] = NULL;
    if (text == NULL || storage == NULL || storageSize == 0 ||
        (size_t)snprintf(storage, storageSize, "%s", text) >= storageSize) {
        return 0;
    }
    at = storage;
    while (*at != '\0' && count < SETTINGS_MAX_BOOTSTRAP) {
        char *comma = strchr(at, ',');
        char *end = (comma != NULL) ? comma : at + strlen(at);

        if (comma != NULL) {
            *comma = '\0';
        }
        while (*at == ' ' || *at == '\t') {
            at++;
        }
        while (end > at && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (*at != '\0') {
            list[count++] = at;
        }
        if (comma == NULL) {
            break;
        }
        at = comma + 1;
    }
    list[count] = NULL;
    return count;
}

const char *settingsErrorString(int32_t error)
{
    switch (error) {
    case SETTINGS_OK:           return "success";
    case SETTINGS_EINVAL:       return "invalid argument";
    case SETTINGS_ENOMEM:       return "out of memory";
    case SETTINGS_EIO:          return "input/output error";
    case SETTINGS_ENAMETOOLONG: return "path too long";
    default:                    return "unknown settings error";
    }
}
