/*
 * net.c - Binding sockets to one network interface. See net.h.
 */
#ifndef _WIN32
#define _DEFAULT_SOURCE         /* if_nametoindex() */
#endif

#include <stdio.h>
#include <string.h>

#include "net.h"
#include "platform.h"           /* tying sockets to an interface */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>           /* ConvertInterfaceAliasToLuid() */
#include <netioapi.h>
#else
#include <sys/types.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/* uv_fileno() gives a HANDLE on Windows; for sockets it holds the SOCKET. */
#ifdef _WIN32
#define NET_SOCKET(fd)          ((uv_os_sock_t)(uintptr_t)(fd))
#else
#define NET_SOCKET(fd)          ((uv_os_sock_t)(fd))
#endif

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void setMessage(char *message, size_t size, const char *text)
{
    if (message != NULL && size > 0) {
        snprintf(message, size, "%s", text);
    }
}

static int sameName(const char *a, const char *b)
{
#ifdef _WIN32
    /* Windows interface names ("Ethernet 2") are not case-sensitive. */
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;

        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

static int isLinkLocal(const struct sockaddr_in6 *address)
{
    const uint8_t *bytes = (const uint8_t *)&address->sin6_addr;

    return bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
}

/* "lo, eth0, tun0" into 'out' (each name once). */
static void listNames(const uv_interface_address_t *entries, int count, char *out, size_t size)
{
    size_t used = 0;
    int    i;
    int    j;

    out[0] = '\0';
    for (i = 0; i < count; i++) {
        int seen = 0;

        for (j = 0; j < i && !seen; j++) {
            seen = (strcmp(entries[i].name, entries[j].name) == 0);
        }
        if (!seen && used + strlen(entries[i].name) + 3u < size) {
            used += (size_t)snprintf(out + used, size - used, "%s%s", (used > 0) ? ", " : "",
                                     entries[i].name);
        }
    }
}

/* The interface index for 'name', or 0. */
static uint32_t interfaceIndex(const char *name)
{
#ifdef _WIN32
    /* The names libuv reports are the "friendly" aliases; if_nametoindex()
     * wants a different kind of name, so go through the LUID instead. */
    WCHAR   wide[NET_INTERFACE_NAME_LENGTH];
    NET_LUID luid;
    NET_IFINDEX index = 0;

    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, (int)NET_INTERFACE_NAME_LENGTH) == 0 ||
        ConvertInterfaceAliasToLuid(wide, &luid) != NO_ERROR ||
        ConvertInterfaceLuidToIndex(&luid, &index) != NO_ERROR) {
        return 0;
    }
    return (uint32_t)index;
#else
    return (uint32_t)if_nametoindex(name);
#endif
}

/*
 * Whether this process may tie sockets to the interface itself (see
 * platformInterfaceTieCheck()); if not, say why. Sockets are then bound to
 * the interface's address only.
 */
static int deviceBindingAllowed(const netBinding *binding, char *message, size_t size)
{
    switch (platformInterfaceTieCheck(binding->name, binding->index)) {
    case 0:
        return 1;
    case UV_EPERM:
        setMessage(message, size,
                   "warning: not permitted to use SO_BINDTODEVICE (Linux before 5.7 needs "
                   "CAP_NET_RAW); sockets are bound to the interface's address only, so "
                   "traffic may still be routed out of another interface");
        return 0;
    case UV_ENODEV:
        setMessage(message, size,
                   "warning: could not find the interface index; sockets are bound to "
                   "its address only");
        return 0;
    default:
        setMessage(message, size,
                   "note: this system cannot tie sockets to an interface, so they are "
                   "bound to its address only; traffic follows the routing table (on "
                   "FreeBSD, give the VPN the default route, or use a separate FIB "
                   "with setfib(1))");
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t netBindingResolve(const char *spec, netBinding *binding, char *message, size_t size)
{
    uv_interface_address_t *entries = NULL;
    struct sockaddr_in      wanted4;
    struct sockaddr_in6     wanted6;
    char                    names[256];
    char                    text[NET_MESSAGE_LENGTH];
    int                     count = 0;
    int                     isAddress4;
    int                     isAddress6;
    int                     found = 0;
    int                     i;

    setMessage(message, size, "");
    if (binding == NULL) {
        return NET_EINVAL;
    }
    memset(binding, 0, sizeof(*binding));
    if (spec == NULL || spec[0] == '\0' || strlen(spec) >= NET_INTERFACE_NAME_LENGTH) {
        setMessage(message, size, "no interface given");
        return NET_EINVAL;
    }
    isAddress4 = (uv_ip4_addr(spec, 0, &wanted4) == 0);
    isAddress6 = !isAddress4 && (uv_ip6_addr(spec, 0, &wanted6) == 0);

    if (uv_interface_addresses(&entries, &count) != 0) {
        setMessage(message, size, "could not list the network interfaces");
        return NET_ERROR;
    }

    for (i = 0; i < count; i++) {
        const uv_interface_address_t *entry = &entries[i];
        int                            family = entry->address.address4.sin_family;

        if (isAddress4 || isAddress6) {
            /* An address: the interface that owns it, and only that address. */
            if (isAddress4 && family == AF_INET &&
                entry->address.address4.sin_addr.s_addr == wanted4.sin_addr.s_addr) {
                binding->ipv4 = entry->address.address4;
                binding->hasIPv4 = 1;
            } else if (isAddress6 && family == AF_INET6 &&
                       memcmp(&entry->address.address6.sin6_addr, &wanted6.sin6_addr,
                              sizeof(wanted6.sin6_addr)) == 0) {
                binding->ipv6 = entry->address.address6;
                binding->hasIPv6 = 1;
            } else {
                continue;
            }
            snprintf(binding->name, sizeof(binding->name), "%s", entry->name);
            found = 1;
            break;
        }
        if (!sameName(entry->name, spec)) {
            continue;
        }
        if (!found) {
            snprintf(binding->name, sizeof(binding->name), "%s", entry->name);
            found = 1;
        }
        if (family == AF_INET && !binding->hasIPv4) {
            binding->ipv4 = entry->address.address4;
            binding->hasIPv4 = 1;
        } else if (family == AF_INET6 && !binding->hasIPv6 &&
                   !isLinkLocal(&entry->address.address6)) {
            binding->ipv6 = entry->address.address6;
            binding->hasIPv6 = 1;
        }
    }
    listNames(entries, count, names, sizeof(names));
    uv_free_interface_addresses(entries, count);

    if (!found) {
        snprintf(text, sizeof(text), (isAddress4 || isAddress6)
                                         ? "no network interface has the address %s (interfaces: %s)"
                                         : "no network interface called \"%s\" (interfaces: %s)",
                 spec, names);
        setMessage(message, size, text);
        memset(binding, 0, sizeof(*binding));
        return NET_ENOENT;
    }
    if (!binding->hasIPv4 && !binding->hasIPv6) {
        snprintf(text, sizeof(text),
                 "interface %s has no usable IPv4 or IPv6 address (is it up?)", binding->name);
        setMessage(message, size, text);
        memset(binding, 0, sizeof(*binding));
        return NET_EADDRNOTAVAIL;
    }
    binding->ipv4.sin_port = 0;
    binding->ipv6.sin6_port = 0;
    binding->index = interfaceIndex(binding->name);
    binding->device = (uint8_t)deviceBindingAllowed(binding, message, size);
    binding->active = 1;
    return NET_OK;
}

void netBindingDescribe(const netBinding *binding, char *out, size_t size)
{
    char ipv4[64] = "";
    char ipv6[64] = "";

    if (out == NULL || size == 0) {
        return;
    }
    if (binding == NULL || !binding->active) {
        snprintf(out, size, "any interface");
        return;
    }
    if (binding->hasIPv4) {
        uv_ip4_name(&binding->ipv4, ipv4, sizeof(ipv4));
    }
    if (binding->hasIPv6) {
        uv_ip6_name(&binding->ipv6, ipv6, sizeof(ipv6));
    }
    snprintf(out, size, "%s (%s%s%s%s)%s", binding->name, ipv4,
             (binding->hasIPv4 && binding->hasIPv6) ? ", " : "", ipv6,
             binding->hasIPv4 && binding->hasIPv6 ? ""
             : binding->hasIPv4                   ? "; no IPv6"
                                                  : "; no IPv4",
             binding->device ? "" : " [address only]");
}

int netResolveFamily(const netBinding *binding)
{
    if (binding == NULL || !binding->active || (binding->hasIPv4 && binding->hasIPv6)) {
        return AF_UNSPEC;
    }
    return binding->hasIPv4 ? AF_INET : AF_INET6;
}

/* Is the binding able to reach 'family'? 0, or a libuv error. */
static int checkFamily(const netBinding *binding, int family)
{
    if ((family == AF_INET && !binding->hasIPv4) || (family == AF_INET6 && !binding->hasIPv6)) {
        return UV_EADDRNOTAVAIL;
    }
    if (family != AF_INET && family != AF_INET6) {
        return UV_EAFNOSUPPORT;
    }
    return 0;
}

int netTcpInit(uv_loop_t *loop, uv_tcp_t *tcp, const netBinding *binding, int family)
{
    if (binding == NULL || !binding->active) {
        return uv_tcp_init(loop, tcp);
    }
    return uv_tcp_init_ex(loop, tcp, (unsigned int)family);
}

int netUdpInit(uv_loop_t *loop, uv_udp_t *udp, const netBinding *binding, int family)
{
    if (binding == NULL || !binding->active) {
        return uv_udp_init(loop, udp);
    }
    return uv_udp_init_ex(loop, udp, (unsigned int)family);
}

/* Set the interface option on the socket. 0, or a libuv error. */
static int tieToDevice(uv_os_sock_t sock, const netBinding *binding, int family)
{
    if (!binding->device) {
        return 0;
    }
    return (int)platformInterfaceTie(sock, family, binding->name, binding->index);
}

static const struct sockaddr *localAddress(const netBinding *binding, int family)
{
    return (family == AF_INET) ? (const struct sockaddr *)&binding->ipv4
                               : (const struct sockaddr *)&binding->ipv6;
}

int netTcpBind(uv_tcp_t *tcp, const netBinding *binding, int family)
{
    uv_os_fd_t   fd;
    uv_os_sock_t sock;
    int          result;

    if (binding == NULL || !binding->active) {
        return 0;
    }
    if ((result = checkFamily(binding, family)) != 0) {
        return result;
    }
    if ((result = uv_fileno((const uv_handle_t *)tcp, &fd)) != 0) {
        return result;
    }
    sock = NET_SOCKET(fd);
    if ((result = tieToDevice(sock, binding, family)) != 0) {
        return result;
    }
    return uv_tcp_bind(tcp, localAddress(binding, family), 0);
}

int netUdpBind(uv_udp_t *udp, const netBinding *binding, int family)
{
    uv_os_fd_t   fd;
    uv_os_sock_t sock;
    int          result;

    if (binding == NULL || !binding->active) {
        return 0;
    }
    if ((result = checkFamily(binding, family)) != 0) {
        return result;
    }
    if ((result = uv_fileno((const uv_handle_t *)udp, &fd)) != 0) {
        return result;
    }
    sock = NET_SOCKET(fd);
    if ((result = tieToDevice(sock, binding, family)) != 0) {
        return result;
    }
    return uv_udp_bind(udp, localAddress(binding, family), 0);
}

int netTcpListenBind(uv_tcp_t *tcp, const netBinding *binding, int family, uint16_t port)
{
    struct sockaddr_storage local;
    uv_os_fd_t              fd;
    int                     result;

    memset(&local, 0, sizeof(local));
    if (binding != NULL && binding->active) {
        if ((result = checkFamily(binding, family)) != 0) {
            return result;
        }
        if ((result = uv_fileno((const uv_handle_t *)tcp, &fd)) != 0) {
            return result;
        }
        if ((result = tieToDevice(NET_SOCKET(fd), binding, family)) != 0) {
            return result;
        }
        memcpy(&local, localAddress(binding, family),
               (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
    } else if (family == AF_INET) {
        uv_ip4_addr("0.0.0.0", 0, (struct sockaddr_in *)&local);
    } else if (family == AF_INET6) {
        uv_ip6_addr("::", 0, (struct sockaddr_in6 *)&local);
    } else {
        return UV_EAFNOSUPPORT;
    }
    if (family == AF_INET) {
        ((struct sockaddr_in *)&local)->sin_port = htons(port);
    } else {
        ((struct sockaddr_in6 *)&local)->sin6_port = htons(port);
    }
    /* IPv6 listeners take IPv6 only; IPv4 has its own listener. */
    return uv_tcp_bind(tcp, (const struct sockaddr *)&local,
                       (family == AF_INET6) ? UV_TCP_IPV6ONLY : 0u);
}

int netUdpBindPort(uv_udp_t *udp, const netBinding *binding, int family, uint16_t port)
{
    struct sockaddr_storage local;
    uv_os_fd_t              fd;
    int                     result;

    memset(&local, 0, sizeof(local));
    if (binding != NULL && binding->active) {
        if ((result = checkFamily(binding, family)) != 0) {
            return result;
        }
        if ((result = uv_fileno((const uv_handle_t *)udp, &fd)) != 0) {
            return result;
        }
        if ((result = tieToDevice(NET_SOCKET(fd), binding, family)) != 0) {
            return result;
        }
        memcpy(&local, localAddress(binding, family),
               (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
    } else if (family == AF_INET) {
        uv_ip4_addr("0.0.0.0", 0, (struct sockaddr_in *)&local);
    } else {
        uv_ip6_addr("::", 0, (struct sockaddr_in6 *)&local);
    }
    if (family == AF_INET) {
        ((struct sockaddr_in *)&local)->sin_port = htons(port);
    } else {
        ((struct sockaddr_in6 *)&local)->sin6_port = htons(port);
    }
    return uv_udp_bind(udp, (const struct sockaddr *)&local,
                       (family == AF_INET6) ? UV_UDP_IPV6ONLY : 0u);
}

const char *netStatusString(int32_t status)
{
    switch (status) {
    case NET_OK:            return "OK";
    case NET_EINVAL:        return "invalid argument";
    case NET_ENOENT:        return "no such interface or address";
    case NET_EADDRNOTAVAIL: return "interface has no usable address";
    case NET_ERROR:         return "could not list the network interfaces";
    default:                return "unknown status";
    }
}
