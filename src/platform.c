/*
 * platform.c - What differs between Linux, FreeBSD, macOS (POSIX) and Windows
 * (Win32).
 *
 * See platform.h. Errors are converted to libuv error codes with
 * uv_translate_sys_error(), which takes errno on Linux and FreeBSD and
 * GetLastError() values on Windows.
 */
#if !defined(_WIN32) && !defined(__FreeBSD__) && !defined(__APPLE__)
/* On FreeBSD and macOS this would hide the routing sysctl and other BSD
 * interfaces; their default environment already has everything POSIX. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE             /* glibc: SO_BINDTODEVICE */
#define _FILE_OFFSET_BITS 64        /* 64-bit off_t on 32-bit Linux too */
#endif

#ifndef _WIN32
#include <sys/types.h>              /* first, for the BSD network headers */
#endif
#include <stdio.h>
#include <string.h>

#include <uv.h>     /* uv_translate_sys_error(); on Windows also <windows.h> */

#include "platform.h"

#ifdef _WIN32
#include <iphlpapi.h>               /* GetIpForwardTable() */
#ifndef IP_UNICAST_IF
#define IP_UNICAST_IF           31
#endif
#ifndef IPV6_UNICAST_IF
#define IPV6_UNICAST_IF         31
#endif
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

int32_t platformFileStamp(const char *path, uint64_t *size, uint8_t *isRegular,
                          int64_t *modified)
{
    platformFile file;
    FILETIME     written;
    int32_t      result;

    result = openWith(path, FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, &file);
    if (result != 0) {
        return result;
    }
    result = platformFileInfo(&file, size, isRegular);
    if (result == 0) {
        if (GetFileTime((HANDLE)file.handle, NULL, NULL, &written)) {
            *modified = (int64_t)((((uint64_t)written.dwHighDateTime << 32) |
                                   written.dwLowDateTime) * 100u);
        } else {
            result = lastError();
        }
    }
    CloseHandle((HANDLE)file.handle);
    return result;
}

int32_t platformFileReplace(const char *from, const char *to)
{
    wchar_t wideFrom[PLATFORM_MAX_PATH];
    wchar_t wideTo[PLATFORM_MAX_PATH];
    int32_t result = widen(from, wideFrom);

    if (result == 0) {
        result = widen(to, wideTo);
    }
    if (result == 0 &&
        !MoveFileExW(wideFrom, wideTo, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result = lastError();
    }
    return result;
}

int32_t platformFileRemove(const char *path)
{
    wchar_t wide[PLATFORM_MAX_PATH];
    int32_t result = widen(path, wide);

    if (result == 0 && !DeleteFileW(wide)) {
        result = lastError();
    }
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

/* --- Networking ---------------------------------------------------------- */

int32_t platformInterfaceTieCheck(const char *name, uint32_t index)
{
    (void)name;
    return (index != 0) ? 0 : UV_ENODEV;
}

int32_t platformInterfaceTie(uv_os_sock_t sock, int family, const char *name, uint32_t index)
{
    DWORD value;
    int   result;

    (void)name;
    if (family == AF_INET) {
        value = htonl((u_long)index);               /* network byte order for IPv4 */
        result = setsockopt(sock, IPPROTO_IP, IP_UNICAST_IF, (const char *)&value,
                            (int)sizeof(value));
    } else {
        value = (DWORD)index;                       /* host byte order for IPv6 */
        result = setsockopt(sock, IPPROTO_IPV6, IPV6_UNICAST_IF, (const char *)&value,
                            (int)sizeof(value));
    }
    return (result == 0) ? 0 : (int32_t)uv_translate_sys_error(WSAGetLastError());
}

int32_t platformDefaultGateway(const char *name, uint32_t index, void *scratch, size_t *size,
                               uint32_t *gateway)
{
    MIB_IPFORWARDTABLE *table = scratch;
    ULONG               length = (ULONG)((*size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : *size);
    DWORD               bestMetric = (DWORD)-1;
    DWORD               result;
    DWORD               i;
    int                 found = 0;

    (void)name;
    result = GetIpForwardTable((scratch != NULL) ? table : NULL, &length, TRUE);
    if (result == ERROR_INSUFFICIENT_BUFFER || scratch == NULL) {
        *size = (size_t)length + 4096u;             /* room for a route added meanwhile */
        return UV_ENOBUFS;
    }
    if (result != NO_ERROR) {
        return (int32_t)uv_translate_sys_error((int)result);
    }
    for (i = 0; i < table->dwNumEntries; i++) {
        const MIB_IPFORWARDROW *row = &table->table[i];

        if (row->dwForwardDest != 0 || row->dwForwardMask != 0 || row->dwForwardNextHop == 0) {
            continue;
        }
        if (index != 0 && row->dwForwardIfIndex != index) {
            continue;
        }
        if (!found || row->dwForwardMetric1 < bestMetric) {
            bestMetric = row->dwForwardMetric1;
            *gateway = row->dwForwardNextHop;       /* network byte order */
            found = 1;
        }
    }
    return found ? 0 : UV_ENOENT;
}

const char *const *platformCertificateBundles(void)
{
    static const char *const none[] = { NULL };     /* the certificate store instead */

    return none;
}

#else
/* ======================================================================== */
/* Linux, FreeBSD and macOS (POSIX)                                         */
/* ======================================================================== */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__FreeBSD__) || defined(__APPLE__)
#include <stddef.h>
#include <sys/sysctl.h>
#include <net/route.h>
#endif

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

int32_t platformFileStamp(const char *path, uint64_t *size, uint8_t *isRegular,
                          int64_t *modified)
{
    struct stat information;

    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    if (stat(path, &information) != 0) {
        return lastError();
    }
    describe(&information, size, isRegular);
#if defined(__APPLE__)
    /* macOS names it st_mtimespec. */
    *modified = (int64_t)information.st_mtimespec.tv_sec * 1000000000 +
                (int64_t)information.st_mtimespec.tv_nsec;
#else
    /* st_mtim: POSIX 2008, on Linux (glibc, musl) and FreeBSD alike. */
    *modified = (int64_t)information.st_mtim.tv_sec * 1000000000 +
                (int64_t)information.st_mtim.tv_nsec;
#endif
    return 0;
}

int32_t platformFileReplace(const char *from, const char *to)
{
    if (from == NULL || to == NULL) {
        return UV_EINVAL;
    }
    return (rename(from, to) == 0) ? 0 : lastError();
}

int32_t platformFileRemove(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return UV_EINVAL;
    }
    return (unlink(path) == 0) ? 0 : lastError();
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

/* --- Networking ---------------------------------------------------------- */

#if defined(__linux__)

int32_t platformInterfaceTieCheck(const char *name, uint32_t index)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int result;
    int error;

    (void)index;
    if (fd < 0) {
        fd = socket(AF_INET6, SOCK_DGRAM, 0);
    }
    if (fd < 0) {
        return 0;                       /* cannot tell; the real sockets will say */
    }
    result = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, (socklen_t)strlen(name));
    error = errno;
    close(fd);
    return (result != 0 && (error == EPERM || error == EACCES)) ? UV_EPERM : 0;
}

int32_t platformInterfaceTie(uv_os_sock_t sock, int family, const char *name, uint32_t index)
{
    (void)family;
    (void)index;
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, name, (socklen_t)strlen(name)) != 0) {
        return lastError();
    }
    return 0;
}

int32_t platformDefaultGateway(const char *name, uint32_t index, void *scratch, size_t *size,
                               uint32_t *gateway)
{
    FILE         *file = fopen("/proc/net/route", "r");
    char          line[256];
    unsigned long bestMetric = (unsigned long)-1;
    int           found = 0;

    (void)index;
    (void)scratch;
    (void)size;
    if (file == NULL) {
        return lastError();
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char          interfaceName[64];
        unsigned long destination;
        unsigned long via;
        unsigned long flags;
        unsigned long metric;
        unsigned long mask;

        if (sscanf(line, "%63s %lx %lx %lx %*d %*d %lu %lx", interfaceName, &destination, &via,
                   &flags, &metric, &mask) != 6) {
            continue;                               /* the header line */
        }
        if (destination != 0 || mask != 0 || !(flags & 0x2u) || via == 0) {
            continue;                               /* not a default route via a router */
        }
        if (name != NULL && name[0] != '\0' && strcmp(interfaceName, name) != 0) {
            continue;
        }
        if (!found || metric < bestMetric) {
            bestMetric = metric;
            *gateway = (uint32_t)via;               /* already network byte order */
            found = 1;
        }
    }
    fclose(file);
    return found ? 0 : UV_ENOENT;
}

#elif defined(__APPLE__)

/* macOS: IP_BOUND_IF / IPV6_BOUND_IF tie a socket to an interface by index. */
int32_t platformInterfaceTieCheck(const char *name, uint32_t index)
{
    (void)index;
    return (if_nametoindex(name) != 0) ? 0 : UV_ENODEV;
}

int32_t platformInterfaceTie(uv_os_sock_t sock, int family, const char *name, uint32_t index)
{
    int interfaceIndex = (int)if_nametoindex(name);
    int result;

    (void)index;
    if (interfaceIndex == 0) {
        return UV_ENODEV;
    }
    if (family == AF_INET6) {
        result = setsockopt(sock, IPPROTO_IPV6, IPV6_BOUND_IF, &interfaceIndex,
                            (socklen_t)sizeof(interfaceIndex));
    } else {
        result = setsockopt(sock, IPPROTO_IP, IP_BOUND_IF, &interfaceIndex,
                            (socklen_t)sizeof(interfaceIndex));
    }
    return (result == 0) ? 0 : lastError();
}

#else   /* FreeBSD and other POSIX systems: no per-interface socket option */

int32_t platformInterfaceTieCheck(const char *name, uint32_t index)
{
    (void)name;
    (void)index;
    return UV_ENOTSUP;
}

int32_t platformInterfaceTie(uv_os_sock_t sock, int family, const char *name, uint32_t index)
{
    (void)sock;
    (void)family;
    (void)name;
    (void)index;
    return UV_ENOTSUP;
}

#endif  /* networking */

#if defined(__FreeBSD__) || defined(__APPLE__)

/*
 * The routing sysctl (NET_RT_FLAGS) on FreeBSD and macOS: messages of an
 * rt_msghdr followed by its addresses, each padded to a boundary: a long
 * on FreeBSD (SA_SIZE), 32 bits on macOS.
 */
#if defined(__APPLE__)
#define ROUTE_STEP(address) ((address)->sa_len > 0                                              \
                             ? (1u + (((size_t)(address)->sa_len - 1u) | (sizeof(uint32_t) - 1u))) \
                             : sizeof(uint32_t))
#else
#define ROUTE_STEP(address) SA_SIZE(address)
#endif

/* Is the IPv4 netmask in the routing message all zeros (a default route)?
 * It may be cut short (sa_len), the missing bytes being zero. */
static int zeroMask(const struct sockaddr *mask)
{
    const uint8_t *bytes = (const uint8_t *)mask;
    size_t         start = offsetof(struct sockaddr_in, sin_addr);
    size_t         i;

    if (mask == NULL) {
        return 1;
    }
    for (i = start; i < start + 4u && i < mask->sa_len; i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

int32_t platformDefaultGateway(const char *name, uint32_t index, void *scratch, size_t *size,
                               uint32_t *gateway)
{
    int                       mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS,
                                         RTF_GATEWAY };
    size_t                    needed = 0;
    size_t                    length;
    const char               *at;
    const char               *end;
    unsigned int              wantIndex = 0;

    (void)index;
    if (name != NULL && name[0] != '\0') {
        wantIndex = if_nametoindex(name);
        if (wantIndex == 0) {
            return UV_ENODEV;
        }
    }
    if (sysctl(mib, 6, NULL, &needed, NULL, 0) != 0) {
        return lastError();
    }
    needed += needed / 4u + 1024u;                  /* routes may be added meanwhile */
    if (scratch == NULL || *size < needed) {
        *size = needed;
        return UV_ENOBUFS;
    }
    length = *size;
    if (sysctl(mib, 6, scratch, &length, NULL, 0) != 0) {
        if (errno == ENOMEM) {
            *size *= 2u;
            return UV_ENOBUFS;
        }
        return lastError();
    }
    at = scratch;
    end = at + length;
    while (at + sizeof(struct rt_msghdr) <= end) {
        const struct rt_msghdr *message = (const struct rt_msghdr *)(const void *)at;
        const struct sockaddr  *address = (const struct sockaddr *)(const void *)(message + 1);
        const struct sockaddr  *destination = NULL;
        const struct sockaddr  *via = NULL;
        const struct sockaddr  *mask = NULL;
        int                     bit;

        if (message->rtm_msglen == 0 || at + message->rtm_msglen > end) {
            break;
        }
        at += message->rtm_msglen;
        if (message->rtm_version != RTM_VERSION || !(message->rtm_flags & RTF_UP) ||
            !(message->rtm_flags & RTF_GATEWAY) ||
            (wantIndex != 0 && message->rtm_index != wantIndex)) {
            continue;
        }
        /* The addresses follow the header, in RTAX_ order, each padded. */
        for (bit = 0; bit < RTAX_MAX; bit++) {
            if (!(message->rtm_addrs & (1 << bit))) {
                continue;
            }
            if ((const char *)address >= at) {
                break;
            }
            if (bit == RTAX_DST) {
                destination = address;
            } else if (bit == RTAX_GATEWAY) {
                via = address;
            } else if (bit == RTAX_NETMASK) {
                mask = address;
            }
            address = (const struct sockaddr *)(const void *)((const char *)address +
                                                               ROUTE_STEP(address));
        }
        if (destination == NULL || via == NULL || destination->sa_family != AF_INET ||
            via->sa_family != AF_INET ||
            ((const struct sockaddr_in *)(const void *)destination)->sin_addr.s_addr != 0 ||
            !zeroMask(mask)) {
            continue;
        }
        *gateway = ((const struct sockaddr_in *)(const void *)via)->sin_addr.s_addr;
        return 0;
    }
    return UV_ENOENT;
}

#elif !defined(__linux__)

int32_t platformDefaultGateway(const char *name, uint32_t index, void *scratch, size_t *size,
                               uint32_t *gateway)
{
    (void)name;
    (void)index;
    (void)scratch;
    (void)size;
    (void)gateway;
    return UV_ENOTSUP;
}

#endif

const char *const *platformCertificateBundles(void)
{
    static const char *const bundles[] = {
#if defined(__APPLE__)
        /* macOS keeps its roots in the Keychain, which OpenSSL cannot read;
         * these are PEM copies. */
        "/opt/homebrew/etc/openssl@3/cert.pem",                 /* Homebrew, Apple silicon */
        "/usr/local/etc/openssl@3/cert.pem",                    /* Homebrew, Intel   */
        "/opt/homebrew/etc/ca-certificates/cert.pem",           /* ca-certificates   */
        "/usr/local/etc/ca-certificates/cert.pem",
        "/opt/local/etc/openssl/cert.pem",                      /* MacPorts          */
        "/etc/ssl/cert.pem",                                    /* the system's copy */
#elif defined(__FreeBSD__)
        "/etc/ssl/cert.pem",                                    /* base system (certctl) */
        "/usr/local/share/certs/ca-root-nss.crt",               /* ca_root_nss package   */
        "/usr/local/etc/ssl/cert.pem",                          /* ports OpenSSL         */
        "/usr/local/openssl/cert.pem",
#else
        "/etc/ssl/certs/ca-certificates.crt",                   /* Debian, Ubuntu, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",                     /* Fedora, RHEL       */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",    /* RHEL 7+            */
        "/etc/ssl/ca-bundle.pem",                               /* openSUSE           */
        "/etc/ssl/cert.pem",                                    /* Alpine, macOS      */
#endif
        NULL
    };

    return bundles;
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

/* ------------------------------------------------------------------------ */
/* All platforms                                                            */
/* ------------------------------------------------------------------------ */

int32_t platformDataDirectory(const char *subdirectory, char *path, size_t size)
{
    char    base[PLATFORM_MAX_PATH];
    size_t  length = sizeof(base);
    int     written;
    int32_t result;

    if (path == NULL || size == 0) {
        return UV_EINVAL;
    }
#ifdef _WIN32
    result = uv_os_getenv("APPDATA", base, &length);    /* UTF-8 */
    if (result != 0) {
        return result;
    }
    written = snprintf(path, size, "%s\\torrentinfo", base);
#elif defined(__APPLE__)
    /* The macOS convention: ~/Library/Application Support/torrentinfo */
    result = uv_os_homedir(base, &length);
    if (result != 0) {
        return result;
    }
    written = snprintf(path, size, "%s/Library/Application Support", base);
    if (written < 0 || (size_t)written >= size) {
        return UV_ENAMETOOLONG;
    }
    result = platformMakeDirectory(path);           /* there already, normally */
    if (result != 0) {
        return result;
    }
    written = snprintf(path, size, "%s/Library/Application Support/torrentinfo", base);
#else
    result = uv_os_homedir(base, &length);
    if (result != 0) {
        return result;
    }
    written = snprintf(path, size, "%s/.torrentinfo", base);
#endif
    if (written < 0 || (size_t)written >= size) {
        return UV_ENAMETOOLONG;
    }
    result = platformMakeDirectory(path);
    if (result != 0 || subdirectory == NULL || subdirectory[0] == '\0') {
        return result;
    }
    length = strlen(path);
    written = snprintf(path + length, size - length, "%c%s", PLATFORM_SEPARATOR, subdirectory);
    if (written < 0 || (size_t)written >= size - length) {
        return UV_ENAMETOOLONG;
    }
    return platformMakeDirectory(path);
}
