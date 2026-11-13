/*
 * platform.h - The few things that differ between Linux and Windows.
 *
 * Everything else in the library is portable C or goes through libuv.
 *
 *   - (Lists come from the bundled 4.4BSD queue.h on every platform.)
 *   - File access with UTF-8 paths: POSIX calls on Linux, the wide-character
 *     Win32 calls on Windows (so non-ASCII paths work without any code page
 *     settings).
 *
 * The file functions never allocate memory and keep no shared state, so
 * they are safe to call from libuv's thread pool. They return 0 or a
 * negative libuv error code (UV_ENOENT, UV_EACCES, ...), which
 * uv_strerror() describes on either platform.
 *
 * Building on Windows: define WIN32_LEAN_AND_MEAN (the Visual Studio project
 * does) so that <windows.h> does not pull in the old <winsock.h>.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest path accepted on Windows, in UTF-16 units (the NTFS limit). */
#define PLATFORM_MAX_PATH       32768u

/* An open file. Treat as opaque. */
typedef struct platformFile {
#ifdef _WIN32
    void       *handle;         /* HANDLE */
#else
    int         descriptor;
#endif
} platformFile;

/* Size of a file, and whether it is a regular file (not a directory or device). */
int32_t platformFileStat(const char *path, uint64_t *size, uint8_t *isRegular);

/* Open an existing file for reading. */
int32_t platformFileOpenRead(const char *path, platformFile *file);

/* Create a file for writing, or truncate it if it exists. */
int32_t platformFileOpenWrite(const char *path, platformFile *file);

/* Size and type of an open file. */
int32_t platformFileInfo(platformFile *file, uint64_t *size, uint8_t *isRegular);

/* Read up to 'length' bytes. *count is 0 at end of file. */
int32_t platformFileRead(platformFile *file, void *buffer, uint32_t length, uint32_t *count);

/* Write up to 'length' bytes; *count says how many were written. */
int32_t platformFileWrite(platformFile *file, const void *buffer, uint32_t length,
                          uint32_t *count);

/* Close a file. Returns an error if pending writes failed. */
int32_t platformFileClose(platformFile *file);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_H */
