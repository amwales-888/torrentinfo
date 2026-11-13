/*
 * platform.h - The few things that differ between Linux, FreeBSD and Windows.
 *
 * Everything else in the library is portable C or goes through libuv.
 *
 *   - (Lists come from the bundled 4.4BSD queue.h on every platform.)
 *   - File access with UTF-8 paths: POSIX calls on Linux and FreeBSD, the
 *     wide-character Win32 calls on Windows (so non-ASCII paths work without
 *     any code page settings).
 *   - Tying a socket to a network interface: SO_BINDTODEVICE on Linux,
 *     IP_UNICAST_IF / IPV6_UNICAST_IF on Windows. FreeBSD has no such
 *     option; sockets there can only be bound to the interface's address.
 *   - Finding the default gateway (for NAT-PMP): /proc/net/route on Linux,
 *     the routing sysctl (NET_RT_FLAGS) on FreeBSD, GetIpForwardTable() on
 *     Windows.
 *   - Where the system keeps its trusted CA certificates.
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

#include <stddef.h>
#include <stdint.h>

#include <uv.h>     /* uv_os_sock_t; libuv error codes */

#ifdef __cplusplus
extern "C" {
#endif

/* Longest path accepted on Windows, in UTF-16 units (the NTFS limit). */
#define PLATFORM_MAX_PATH       32768u

/* The preferred path separator. */
#ifdef _WIN32
#define PLATFORM_SEPARATOR      '\\'
#else
#define PLATFORM_SEPARATOR      '/'
#endif

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

/* Create a directory. Succeeds (0) if it already exists as a directory. */
int32_t platformMakeDirectory(const char *path);

/*
 * The directory for the program's own lasting data, created if missing:
 * ~/.torrentinfo on Linux and FreeBSD, %APPDATA%\\torrentinfo on Windows; with a
 * 'subdirectory' (or NULL), that directory inside it, created too. The
 * UTF-8 path goes to 'path'. Returns 0 or a libuv error code.
 */
int32_t platformDataDirectory(const char *subdirectory, char *path, size_t size);

/* Open a file for reading and writing, creating it if needed; never truncates. */
int32_t platformFileOpenReadWrite(const char *path, platformFile *file);

/* Extend or truncate an open file to 'size' bytes. */
int32_t platformFileSetSize(platformFile *file, uint64_t size);

/* Read or write at a byte offset, independent of any file position. */
int32_t platformFileReadAt(platformFile *file, uint64_t offset, void *buffer, uint32_t length,
                           uint32_t *count);
int32_t platformFileWriteAt(platformFile *file, uint64_t offset, const void *buffer,
                            uint32_t length, uint32_t *count);

/* ------------------------------------------------------------------------ */
/* Networking                                                               */
/* ------------------------------------------------------------------------ */

/*
 * Can sockets be tied to the interface (name, or index on Windows)? 0 if
 * so; UV_EPERM if the system refuses (Linux before 5.7 needs CAP_NET_RAW);
 * UV_ENODEV if the interface's index is unknown (Windows); UV_ENOTSUP where
 * there is no such option (FreeBSD, others). In every case but 0 the
 * caller can still bind sockets to the interface's address.
 */
int32_t platformInterfaceTieCheck(const char *name, uint32_t index);

/* Tie a socket of the given family (AF_INET or AF_INET6) to the interface.
 * 0, or a libuv error (UV_ENOTSUP on FreeBSD). */
int32_t platformInterfaceTie(uv_os_sock_t sock, int family, const char *name, uint32_t index);

/*
 * The IPv4 default gateway, in network byte order, into *gateway: the
 * lowest-metric default route through a router, only through the given
 * interface if 'name' (or, on Windows, 'index') is set.
 *
 * 'scratch' is working memory from the caller (the routing table is read
 * into it on FreeBSD and Windows; Linux needs none), *size its length. If
 * it is too small the call fails with UV_ENOBUFS and sets *size to what is
 * needed; call again with that much. Returns 0, UV_ENOENT if there is no
 * default route, or another libuv error.
 */
int32_t platformDefaultGateway(const char *name, uint32_t index, void *scratch, size_t *size,
                               uint32_t *gateway);

/*
 * Files where the system may keep its trusted CA certificates (PEM), most
 * likely first; NULL-terminated. Windows keeps them in its certificate
 * store instead, so the list there is empty.
 */
const char *const *platformCertificateBundles(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_H */
