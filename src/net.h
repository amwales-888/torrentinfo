/*
 * net.h - Binding sockets to one network interface.
 *
 * netBindingResolve() turns an interface name ("tun0", "eth0", or on
 * Windows a name such as "Ethernet 2") or one of an interface's IP
 * addresses into a netBinding. The peer, HTTP and UDP modules then pass it
 * to netTcpInit()/netTcpBind() or netUdpInit()/netUdpBind() for every
 * socket they open, so that all traffic leaves through that interface:
 *
 *   - the socket is bound to the interface's address (port 0), and
 *   - it is tied to the interface itself: SO_BINDTODEVICE on Linux,
 *     IP_UNICAST_IF / IPV6_UNICAST_IF on Windows. On Linux the address
 *     alone is not enough, since packets may still be routed out of
 *     another interface.
 *
 * A destination whose family the interface has no address for (an IPv6
 * peer on an IPv4-only VPN, say) fails with UV_EADDRNOTAVAIL instead of
 * going out some other way. If the interface goes down, connections fail.
 *
 * Not covered: DNS lookups (uv_getaddrinfo) use the system resolver and its
 * routes. Use netResolveFamily() to ask only for addresses the binding can
 * reach.
 *
 * Everything here is synchronous and cheap except netBindingResolve(), which
 * lists the system's interfaces; call it once at start-up.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_INTERFACE_NAME_LENGTH   256u
#define NET_MESSAGE_LENGTH          512u
#define NET_DESCRIPTION_LENGTH      512u

#define NET_OK                      0
#define NET_EINVAL                  (-1)
#define NET_ENOENT                  (-2)    /* no such interface or address */
#define NET_EADDRNOTAVAIL           (-3)    /* interface has no usable address */
#define NET_ERROR                   (-4)    /* listing interfaces failed      */

typedef struct netBinding {
    uint8_t             active;         /* 0: no binding (sockets as usual)  */
    uint8_t             device;         /* also tied to the interface itself */
    uint8_t             hasIPv4;
    uint8_t             hasIPv6;
    uint32_t            index;          /* interface index (0 if unknown)    */
    char                name[NET_INTERFACE_NAME_LENGTH];
    struct sockaddr_in  ipv4;           /* port 0                            */
    struct sockaddr_in6 ipv6;           /* port 0                            */
} netBinding;

/*
 * Resolve 'spec' (an interface name, or an IPv4/IPv6 address belonging to
 * one) into *binding. Given a name, the interface's first IPv4 address and
 * first non-link-local IPv6 address are used; given an address, only that
 * one. On failure *binding is inactive and 'message' (if not NULL) explains,
 * listing the interfaces that exist. On success 'message' may hold a
 * warning (for example, that the kernel refused SO_BINDTODEVICE, so only the
 * address is bound); it is "" otherwise.
 */
int32_t netBindingResolve(const char *spec, netBinding *binding, char *message, size_t size);

/* Describe a binding, e.g. "tun0 (10.8.0.2, fd00::2)". */
void netBindingDescribe(const netBinding *binding, char *out, size_t size);

/* AF_UNSPEC, AF_INET or AF_INET6: the address family to ask DNS for. */
int netResolveFamily(const netBinding *binding);

/*
 * Initialise a handle for a connection to an address of 'family'. With an
 * active binding the socket is created at once (uv_tcp_init_ex), so that
 * netTcpBind() can set its options. Returns a libuv error code; on success
 * the handle must eventually be closed with uv_close() as usual.
 */
int netTcpInit(uv_loop_t *loop, uv_tcp_t *tcp, const netBinding *binding, int family);
int netUdpInit(uv_loop_t *loop, uv_udp_t *udp, const netBinding *binding, int family);

/*
 * Tie an initialised handle to the binding: interface option, then bind to
 * the interface's address. Call before uv_tcp_connect()/uv_udp_connect().
 * Does nothing (returns 0) for an inactive or NULL binding. Returns a libuv
 * error code; on failure close the handle and give up on this address.
 */
int netTcpBind(uv_tcp_t *tcp, const netBinding *binding, int family);
int netUdpBind(uv_udp_t *udp, const netBinding *binding, int family);

const char *netStatusString(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* NET_H */
