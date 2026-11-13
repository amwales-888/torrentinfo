/*
 * platform.c - File access for Linux (POSIX) and Windows (Win32).
 *
 * See platform.h. Errors are converted to libuv error codes with
 * uv_translate_sys_error(), which takes errno on Linux and GetLastError()
 * values on Windows.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64        /* 64-bit off_t on 32-bit Linux too */
#endif

#include <string.h>

#include <uv.h>     /* uv_translate_sys_error(); on Windows also <windows.h> */

#include "platform.h"

#ifdef _WIN32
/* ======================================================================== */
/* Windows                                                                  */
/* ======================================================================== */

static int32_t lastError(void)
{
    return (int32_t)uv_translate_sys_error((int)GetLastError());
}

/* UTF-8 to UTF-16, into the caller's buffer (no allocation). */
static int32_t widen(const char *path, wchar_t *wide)
{
    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide, (int)PLATFORM_MAX_PATH) == 0) {
        return lastError();
    }
    return 0;
}

static int32_t openWith(const char *path, DWORD access, DWORD share, DWORD disposition,
                        DWORD flags, platformFile *file)
{
    /* 64 KiB on the stack, which libuv's pool threads (8 MiB) and the main
     * thread (1 MiB) can afford; it avoids calling the allocator. */
    wchar_t wide[PLATFORM_MAX_PATH];
    HANDLE  handle;
    int32_t result;

    file->handle = NULL;
    result = widen(path, wide);
    if (result != 0) {
        return result;
    }
    /* NULL security attributes: the handle is not inherited by children,
     * like O_CLOEXEC on Linux. */
    handle = CreateFileW(wide, access, share, NULL, disposition, flags, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return lastError();
    }
    file->handle = handle;
    return 0;
}

int32_t platformFileInfo(platformFile *file, uint64_t *size, uint8_t *isRegular)
{
    BY_HANDLE_FILE_INFORMATION information;

    if (!GetFileInformationByHandle((HANDLE)file->handle, &information)) {
        return lastError();
    }
    *size = ((uint64_t)information.nFileSizeHigh << 32) | information.nFileSizeLow;
    *isRegular = (uint8_t)(GetFileType((HANDLE)file->handle) == FILE_TYPE_DISK &&
                           !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    return 0;
}

int32_t platformFileStat(const char *path, uint64_t *size, uint8_t *isRegular)
{
    platformFile file;
    int32_t      result;

    /* Opening (rather than GetFileAttributesEx) follows symbolic links, as
     * stat() does. BACKUP_SEMANTICS allows directories to be opened. */
    result = openWith(path, FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, &file);
    if (result != 0) {
        return result;
    }
    result = platformFileInfo(&file, size, isRegular);
    CloseHandle((HANDLE)file.handle);
    return result;
}

int32_t platformFileOpenRead(const char *path, platformFile *file)
{
    return openWith(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, file);
}

int32_t platformFileOpenWrite(const char *path, platformFile *file)
{
    return openWith(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, file);
}

int32_t platformFileRead(platformFile *file, void *buffer, uint32_t length, uint32_t *count)
{
    DWORD got = 0;

    *count = 0;
    if (!ReadFile((HANDLE)file->handle, buffer, (DWORD)length, &got, NULL)) {
        return lastError();
    }
    *count = (uint32_t)got;
    return 0;
}

int32_t platformFileWrite(platformFile *file, const void *buffer, uint32_t length,
                          uint32_t *count)
{
    DWORD written = 0;

    *count = 0;
    if (!WriteFile((HANDLE)file->handle, buffer, (DWORD)length, &written, NULL)) {
        return lastError();
    }
    *count = (uint32_t)written;
    return 0;
}

int32_t platformFileClose(platformFile *file)
{
    int32_t result = 0;

    if (file->handle != NULL && !CloseHandle((HANDLE)file->handle)) {
        result = lastError();
    }
    file->handle = NULL;
    return result;
}

int32_t platformMakeDirectory(const char *path)
{
    wchar_t wide[PLATFORM_MAX_PATH];
    DWORD   attributes;
    int32_t result = widen(path, wide);

    if (result != 0) {
        return result;
    }
    if (CreateDirectoryW(wide, NULL)) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        attributes = GetFileAttributesW(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return 0;
        }
        return UV_ENOTDIR;                          /* a file is in the way */
    }
    return lastError();
}

int32_t platformFileOpenReadWrite(const char *path, platformFile *file)
{
    return openWith(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, file);
}

int32_t platformFileSetSize(platformFile *file, uint64_t size)
{
    LARGE_INTEGER position;

    position.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx((HANDLE)file->handle, position, NULL, FILE_BEGIN) ||
        !SetEndOfFile((HANDLE)file->handle)) {
        return lastError();
    }
    return 0;
}

/* An OVERLAPPED with an offset makes ReadFile/WriteFile positional, even on
 * a handle opened for synchronous I/O. */
static void atOffset(OVERLAPPED *overlapped, uint64_t offset)
{
    memset(overlapped, 0, sizeof(*overlapped));
    overlapped->Offset = (DWORD)(offset & 0xFFFFFFFFu);
    overlapped->OffsetHigh = (DWORD)(offset >> 32);
}

int32_t platformFileReadAt(platformFile *file, uint64_t offset, void *buffer, uint32_t length,
                           uint32_t *count)
{
    OVERLAPPED overlapped;
    DWORD      got = 0;

    *count = 0;
    atOffset(&overlapped, offset);
    if (!ReadFile((HANDLE)file->handle, buffer, (DWORD)length, &got, &overlapped)) {
        if (GetLastError() == ERROR_HANDLE_EOF) {
            return 0;                               /* reading past the end: 0 bytes */
        }
        return lastError();
    }
    *count = (uint32_t)got;
    return 0;
}

int32_t platformFileWriteAt(platformFile *file, uint64_t offset, const void *buffer,
                            uint32_t length, uint32_t *count)
{
    OVERLAPPED overlapped;
    DWORD      written = 0;

    *count = 0;
    atOffset(&overlapped, offset);
    if (!WriteFile((HANDLE)file->handle, buffer, (DWORD)length, &written, &overlapped)) {
        return lastError();
    }
    *count = (uint32_t)written;
    return 0;
}

#else
/* ======================================================================== */
/* Linux (POSIX)                                                            */
/* ======================================================================== */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int32_t lastError(void)
{
    return (int32_t)uv_translate_sys_error(errno);
}

static void describe(const struct stat *information, uint64_t *size, uint8_t *isRegular)
{
    *size = (uint64_t)information->st_size;
    *isRegular = (uint8_t)S_ISREG(information->st_mode);
}

int32_t platformFileStat(const char *path, uint64_t *size, uint8_t *isRegular)
{
    struct stat information;

    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    if (stat(path, &information) != 0) {
        return lastError();
    }
    describe(&information, size, isRegular);
    return 0;
}

int32_t platformFileOpenRead(const char *path, platformFile *file)
{
    file->descriptor = -1;
    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    file->descriptor = open(path, O_RDONLY | O_CLOEXEC);
    return (file->descriptor < 0) ? lastError() : 0;
}

int32_t platformFileOpenWrite(const char *path, platformFile *file)
{
    file->descriptor = -1;
    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    file->descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    return (file->descriptor < 0) ? lastError() : 0;
}

int32_t platformFileInfo(platformFile *file, uint64_t *size, uint8_t *isRegular)
{
    struct stat information;

    if (fstat(file->descriptor, &information) != 0) {
        return lastError();
    }
    describe(&information, size, isRegular);
    return 0;
}

int32_t platformFileRead(platformFile *file, void *buffer, uint32_t length, uint32_t *count)
{
    ssize_t got;

    *count = 0;
    do {
        got = read(file->descriptor, buffer, (size_t)length);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        return lastError();
    }
    *count = (uint32_t)got;
    return 0;
}

int32_t platformFileWrite(platformFile *file, const void *buffer, uint32_t length,
                          uint32_t *count)
{
    ssize_t written;

    *count = 0;
    do {
        written = write(file->descriptor, buffer, (size_t)length);
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        return lastError();
    }
    *count = (uint32_t)written;
    return 0;
}

int32_t platformFileClose(platformFile *file)
{
    int32_t result = 0;

    if (file->descriptor >= 0 && close(file->descriptor) != 0) {
        result = lastError();
    }
    file->descriptor = -1;
    return result;
}

int32_t platformMakeDirectory(const char *path)
{
    struct stat information;

    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        if (stat(path, &information) == 0 && S_ISDIR(information.st_mode)) {
            return 0;
        }
        return UV_ENOTDIR;                          /* a file is in the way */
    }
    return lastError();
}

int32_t platformFileOpenReadWrite(const char *path, platformFile *file)
{
    file->descriptor = -1;
    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    file->descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    return (file->descriptor < 0) ? lastError() : 0;
}

int32_t platformFileSetSize(platformFile *file, uint64_t size)
{
    int result;

    do {
        result = ftruncate(file->descriptor, (off_t)size);
    } while (result != 0 && errno == EINTR);
    return (result != 0) ? lastError() : 0;
}

int32_t platformFileReadAt(platformFile *file, uint64_t offset, void *buffer, uint32_t length,
                           uint32_t *count)
{
    ssize_t got;

    *count = 0;
    do {
        got = pread(file->descriptor, buffer, (size_t)length, (off_t)offset);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        return lastError();
    }
    *count = (uint32_t)got;
    return 0;
}

int32_t platformFileWriteAt(platformFile *file, uint64_t offset, const void *buffer,
                            uint32_t length, uint32_t *count)
{
    ssize_t written;

    *count = 0;
    do {
        written = pwrite(file->descriptor, buffer, (size_t)length, (off_t)offset);
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        return lastError();
    }
    *count = (uint32_t)written;
    return 0;
}

#endif
