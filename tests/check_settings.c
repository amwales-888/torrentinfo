/*
 * check_settings.c - which layer wins (defaults, file, overrides), reading
 * a settings file, the blocklist rules, and splitting router lists.
 */
#include <string.h>

#include "check.h"
#include "datafile.h"
#include "settings.h"

static uint32_t warnings;
static uint32_t lastWarningLine;

static void onWarning(void *privateArg, uint32_t line, const char *message)
{
    (void)privateArg;
    (void)message;
    warnings++;
    lastWarningLine = line;
}

static int32_t writeText(const char *path, const char *text)
{
    return datafileWrite(path, (const uint8_t *)text, strlen(text), NULL);
}

static void precedence(void)
{
    settingsValues values;
    configSettings file;
    configSettings overrides;

    settingsDefaults(&values);
    CHECK(values.listenPort == 6881 && values.portForwarding == 1 && values.dht == 1 &&
          values.encryption == CONFIG_ENCRYPTION_PREFER && values.webSeeds == 1 &&
          values.maxDownloadRate == 0 && !values.blocklistNamed);

    memset(&file, 0, sizeof(file));
    memset(&overrides, 0, sizeof(overrides));
    /* The file sets some values; others are present but not marked as set. */
    file.set = CONFIG_SET_LISTEN_PORT | CONFIG_SET_MAX_DOWNLOAD_RATE | CONFIG_SET_DHT |
               CONFIG_SET_BLOCKLIST | CONFIG_SET_DOWNLOAD_DIRECTORY;
    file.listenPort = 7000;
    file.maxDownloadRate = 1000;
    file.dht = 0;
    file.maxUploadRate = 999;                       /* not set: ignored */
    strcpy(file.blocklist, "lists/peers.txt");
    strcpy(file.downloadDirectory, "/data/from-file");
    /* The overrides win where they say something. */
    overrides.set = CONFIG_SET_MAX_DOWNLOAD_RATE | CONFIG_SET_ENCRYPTION |
                    CONFIG_SET_DOWNLOAD_DIRECTORY;
    overrides.maxDownloadRate = 0;                  /* "no limit" is a value too */
    overrides.encryption = CONFIG_ENCRYPTION_REQUIRE;
    strcpy(overrides.downloadDirectory, "/data/from-options");

    settingsMerge(&values, &file, &overrides);
    CHECK(values.listenPort == 7000);               /* file */
    CHECK(values.dht == 0);                         /* file */
    CHECK(values.maxDownloadRate == 0);             /* overrides beat file */
    CHECK(values.maxUploadRate == 0);               /* default: file did not set it */
    CHECK(values.encryption == CONFIG_ENCRYPTION_REQUIRE);
    CHECK(values.portForwarding == 1);              /* default */
    CHECK(strcmp(values.downloadDirectory, "/data/from-options") == 0);
    CHECK(values.blocklistNamed && strcmp(values.blocklist, "lists/peers.txt") == 0);
    CHECK(values.fromFile == file.set && values.fromOverrides == overrides.set);

    settingsMerge(&values, NULL, NULL);             /* back to the defaults */
    CHECK(values.listenPort == 6881 && values.dht == 1 && values.fromFile == 0);
}

static void loadFile(void)
{
    parserContext  context;
    configSettings file;
    settingsValues values;
    uint32_t       what = 99;
    int32_t        error = 0;
    char           path[512];
    char           used[512];

    CHECK(parserInit(&context, &checkAllocator) == PARSER_OK);
    checkScratchPath("settings.txt", path, sizeof(path));
    CHECK(writeText(path, "# test settings\n"
                          "listen_port = 7100\n"
                          "max_upload_rate = 2M\n"
                          "no_such_key = 1\n"
                          "encryption = off\n") == DATAFILE_OK);
    warnings = 0;
    CHECK(settingsLoadFile(&context, path, &file, onWarning, NULL, &what, used, sizeof(used),
                           &error) == SETTINGS_OK);
    CHECK(what == SETTINGS_FILE_READ && strcmp(used, path) == 0);
    CHECK(warnings == 1 && lastWarningLine == 4);
    settingsMerge(&values, &file, NULL);
    CHECK(values.listenPort == 7100 && values.maxUploadRate == 2u * 1024u * 1024u &&
          values.encryption == CONFIG_ENCRYPTION_OFF);

    /* A named file that does not exist is an error (and leaves defaults). */
    checkScratchPath("no-such-settings.txt", path, sizeof(path));
    CHECK(settingsLoadFile(&context, path, &file, onWarning, NULL, &what, used, sizeof(used),
                           &error) == SETTINGS_EIO);
    CHECK(error == UV_ENOENT && file.set == 0);
}

static void loadBlocklist(void)
{
    parserContext  context;
    settingsValues values;
    blocklist     *list = NULL;
    blocklistStats stats;
    int32_t        error = 0;
    char           path[512];
    char           used[512];

    CHECK(parserInit(&context, &checkAllocator) == PARSER_OK);

    /* blocklist = none */
    settingsDefaults(&values);
    values.blocklistNamed = 1;
    values.blocklist[0] = '\0';
    CHECK(settingsLoadBlocklist(&context, &values, &list, &stats, used, sizeof(used), &error) ==
          SETTINGS_OK);
    CHECK(list == NULL && used[0] == '\0');

    /* An absolute path with two ranges and one bad line. */
    checkScratchPath("blocklist.txt", path, sizeof(path));
    CHECK(writeText(path, "10.0.0.0/8\nnot an address\n192.168.1.1-192.168.1.9\n") ==
          DATAFILE_OK);
    snprintf(values.blocklist, sizeof(values.blocklist), "%s", path);
    CHECK(settingsLoadBlocklist(&context, &values, &list, &stats, used, sizeof(used), &error) ==
          SETTINGS_OK);
    CHECK(list != NULL && stats.ranges4 == 2 && stats.rejected == 1 &&
          strcmp(used, path) == 0);
    blocklistFree(&context, list);

    /* A list with nothing usable in it is no list. */
    CHECK(writeText(path, "# only a comment\n") == DATAFILE_OK);
    CHECK(settingsLoadBlocklist(&context, &values, &list, &stats, used, sizeof(used), &error) ==
          SETTINGS_OK);
    CHECK(list == NULL && used[0] != '\0');

    /* A list the settings name must be readable. */
    checkScratchPath("missing-blocklist.txt", path, sizeof(path));
    snprintf(values.blocklist, sizeof(values.blocklist), "%s", path);
    CHECK(settingsLoadBlocklist(&context, &values, &list, &stats, used, sizeof(used), &error) ==
          SETTINGS_EIO);
    CHECK(list == NULL && error == UV_ENOENT);
}

static void bootstrap(void)
{
    char        storage[128];
    const char *list[SETTINGS_MAX_BOOTSTRAP + 1u];

    CHECK(settingsSplitBootstrap("a:1, b:2 ,,c:3", storage, sizeof(storage), list) == 3);
    CHECK(strcmp(list[0], "a:1") == 0 && strcmp(list[1], "b:2") == 0 &&
          strcmp(list[2], "c:3") == 0 && list[3] == NULL);
    CHECK(settingsSplitBootstrap("", storage, sizeof(storage), list) == 0 && list[0] == NULL);
    CHECK(settingsSplitBootstrap("1,2,3,4,5,6,7,8,9,10", storage, sizeof(storage), list) ==
          SETTINGS_MAX_BOOTSTRAP);
    CHECK(list[SETTINGS_MAX_BOOTSTRAP] == NULL);
    CHECK(settingsSplitBootstrap("too long for the storage", storage, 8, list) == 0);
}

void checkSettings(void)
{
    precedence();
    loadFile();
    loadBlocklist();
    bootstrap();
}
