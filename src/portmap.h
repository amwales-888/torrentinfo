/*
 * portmap.h - Asking the home router to forward a port to us.
 *
 * Behind a home router, peers on the internet cannot connect to us unless
 * the router forwards our listening port. Most routers will do that on
 * request, using one of two protocols:
 *
 *   NAT-PMP (RFC 6886)  a few bytes over UDP to the router (the default
 *                       gateway) on port 5351. Apple routers, many others,
 *                       and some VPN services.
 *   UPnP IGD            find the router by multicast (SSDP), read its
 *                       description over HTTP, then ask it to add a "port
 *                       mapping" with a SOAP request.
 *
 * portmapBegin() tries NAT-PMP, then UPnP, and reports the outcome once
 * through the callback. A mapping is renewed before it expires;
 * portmapEnd() removes it again (waiting at most a couple of seconds).
 * Only TCP is mapped.
 *
 * Uses the HTTP module (httpInit() is called and released here).
 */
#ifndef PORTMAP_H
#define PORTMAP_H

#include <stdint.h>
#include <uv.h>

#include "net.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PORTMAP_NATPMP_PORT
#define PORTMAP_NATPMP_PORT         5351u
#endif
#define PORTMAP_SSDP_ADDRESS        "239.255.255.250"
#define PORTMAP_SSDP_PORT           1900u
#define PORTMAP_LIFETIME            3600u   /* seconds asked for              */
#define PORTMAP_TEXT_LENGTH         256u

#define PORTMAP_METHOD_NONE         0u
#define PORTMAP_METHOD_NATPMP       1u
#define PORTMAP_METHOD_UPNP         2u

#define PORTMAP_OK                  0
#define PORTMAP_EINVAL              (-3)
#define PORTMAP_ENOMEM              (-4)
#define PORTMAP_ENOROUTER           (-5)    /* nothing answered               */
#define PORTMAP_EREFUSED            (-6)    /* the router said no             */
#define PORTMAP_ERROR               (-7)    /* something else; see message    */

typedef struct portmapOptions {
    uint16_t            port;           /* our TCP port; the same port is asked
                                           for on the router                 */
    const char         *description;    /* in the router's table; NULL for
                                           "torrentinfo"                     */
    const netBinding   *binding;        /* interface to use, or NULL (copied) */
    const char         *gateway;        /* router IPv4 address for NAT-PMP;
                                           NULL to use the default gateway   */
    const char         *ssdpAddress;    /* where to send UPnP discovery;
                                           NULL for 239.255.255.250 (tests)  */
    uint16_t            ssdpPort;       /* 0 for 1900 (tests)                */
} portmapOptions;

typedef struct portmapResult {
    int32_t     status;                     /* PORTMAP_OK or an error        */
    uint32_t    method;                     /* PORTMAP_METHOD_*              */
    uint16_t    externalPort;               /* the router's port for us      */
    uint32_t    lifetime;                   /* seconds; 0 = until removed    */
    char        externalAddress[64];        /* our public address, if known  */
    char        router[PORTMAP_TEXT_LENGTH];    /* gateway or UPnP address   */
    char        message[PORTMAP_TEXT_LENGTH];   /* what went wrong, if it did */
} portmapResult;

typedef struct portmap portmap;

/*
 * Called with the first outcome, and again if a renewal later fails. Not
 * called after portmapEnd().
 */
typedef void (*portmapCallback)(void *privateArg, portmap *mapper, const portmapResult *result);

int32_t portmapBegin(uv_loop_t *loop, parserContext *context, const portmapOptions *options,
                     portmapCallback callback, void *privateArg, portmap **mapper);

/*
 * Remove the mapping (if one was made) and release everything; the removal
 * is given at most 2 seconds. No callbacks run afterwards. Keep running the
 * loop until uv_run() returns.
 */
void portmapEnd(portmap *mapper);

const char *portmapMethodName(uint32_t method);

#ifdef __cplusplus
}
#endif

#endif /* PORTMAP_H */
