/*
 * portmap.c - NAT-PMP and UPnP IGD port mapping. See portmap.h.
 *
 * One state machine driven by a timer, a UDP socket per protocol and the
 * HTTP module:
 *
 *   NAT-PMP   find the gateway; send "map TCP port" (retried after 0.25, 0.5,
 *             1 and 2 seconds); on success ask for the public address.
 *   UPnP      send M-SEARCH (twice) and wait 3 seconds for a LOCATION; GET
 *             the description; find the WANIPConnection (or WANPPPConnection)
 *             control URL; SOAP AddPortMapping (again with lease 0 if the
 *             router only does permanent mappings); GetExternalIPAddress.
 *   renewing  at half the lifetime (UPnP permanent: every 30 minutes).
 *   ending    NAT-PMP lifetime 0, or SOAP DeletePortMapping; then free.
 */
#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "platform.h"
#include "portmap.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#endif

#define PORTMAP_NATPMP_TRIES        4u      /* 0.25 + 0.5 + 1 + 2 seconds    */
#define PORTMAP_SSDP_WAIT_MS        3000u
#define PORTMAP_HTTP_TIMEOUT        5u      /* seconds per UPnP request      */
#define PORTMAP_END_TIMEOUT         2u
#define PORTMAP_PERMANENT_RENEW     1800u   /* re-add a permanent mapping    */
#define PORTMAP_RETRY_SECONDS       60u     /* after a failed renewal        */
#define PORTMAP_MAX_RESPONSE        (64u * 1024u)
#define PORTMAP_URL_LENGTH          512u

/* States */
#define STATE_NATPMP_MAP            1u
#define STATE_NATPMP_ADDRESS        2u
#define STATE_SSDP                  3u
#define STATE_DESCRIPTION           4u
#define STATE_UPNP_ADD              5u
#define STATE_UPNP_ADDRESS          6u
#define STATE_MAPPED                7u
#define STATE_FAILED                8u
#define STATE_REMOVING              9u

struct portmap {
    uv_loop_t          *loop;
    parserContext      *context;
    portmapCallback     callback;
    void               *privateArg;
    uint16_t            port;
    char                description[64];
    netBinding          binding;
    struct sockaddr_in  gateway;
    uint8_t             haveGateway;
    struct sockaddr_in  ssdp;

    uint32_t            state;
    uint32_t            attempts;
    uint8_t             reported;           /* first outcome given           */
    uint8_t             ended;
    uint8_t             httpUp;
    uint32_t            openHandles;        /* timer + UDP sockets           */

    uv_timer_t          timer;
    uv_udp_t            natpmp;
    uint8_t             natpmpOpen;
    uv_udp_t            ssdpSocket;
    uint8_t             ssdpOpen;
    uv_udp_t            probe;              /* finds our LAN address          */
    uint8_t             probeOpen;
    uint8_t             timerClosing;
    httpRequest        *http;

    char                location[PORTMAP_URL_LENGTH];
    char                controlUrl[PORTMAP_URL_LENGTH];
    char                serviceType[128];
    char                localAddress[64];
    uint32_t            lease;              /* UPnP lease asked for          */

    portmapResult       result;
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void *allocate(portmap *pm, size_t size)
{
    void *memory = pm->context->allocator.realloc(NULL, size);

    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static void onHandleClosed(uv_handle_t *handle)
{
    portmap *pm = handle->data;

    if (--pm->openHandles == 0) {
        if (pm->httpUp) {
            httpShutdown();
        }
        pm->context->allocator.free(pm);
    }
}

static void closeUdp(portmap *pm, uv_udp_t *udp, uint8_t *open)
{
    (void)pm;
    if (*open) {
        *open = 0;
        uv_udp_recv_stop(udp);
        uv_close((uv_handle_t *)udp, onHandleClosed);
    }
}

/* After portmapEnd(): the removal is done (or abandoned); close up. */
static void finishRemoval(portmap *pm)
{
    if (pm->timerClosing) {
        return;
    }
    pm->timerClosing = 1;
    if (pm->http != NULL) {
        httpCancel(pm->http);
        pm->http = NULL;
    }
    closeUdp(pm, &pm->natpmp, &pm->natpmpOpen);
    closeUdp(pm, &pm->ssdpSocket, &pm->ssdpOpen);
    uv_timer_stop(&pm->timer);
    uv_close((uv_handle_t *)&pm->timer, onHandleClosed);
}

static void report(portmap *pm, int32_t status, const char *message)
{
    pm->result.status = status;
    if (message != NULL) {
        snprintf(pm->result.message, sizeof(pm->result.message), "%s", message);
    }
    if (status != PORTMAP_OK) {
        pm->state = (pm->state == STATE_MAPPED) ? STATE_MAPPED : STATE_FAILED;
    }
    if (pm->callback != NULL && !pm->ended) {
        pm->reported = 1;
        pm->callback(pm->privateArg, pm, &pm->result);
    }
}

static void onAlloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buffer)
{
    static char storage[2048];      /* loop thread only; data is copied out at once */

    (void)handle;
    (void)suggested;
    *buffer = uv_buf_init(storage, sizeof(storage));
}

static void startTimer(portmap *pm, uint64_t milliseconds, uv_timer_cb callback)
{
    uv_timer_stop(&pm->timer);
    uv_timer_start(&pm->timer, callback, milliseconds, 0);
}

/* Find "<tag>" ... "</tag>" in 'text' after 'from'; copies the trimmed text. */
static const char *xmlValue(const char *text, const char *from, const char *tag, char *out,
                            size_t size)
{
    char        open[64];
    char        close[64];
    const char *start;
    const char *end;
    size_t      length;

    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    start = strstr((from != NULL) ? from : text, open);
    if (start == NULL) {
        return NULL;
    }
    start += strlen(open);
    end = strstr(start, close);
    if (end == NULL) {
        return NULL;
    }
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
        start++;
    }
    length = (size_t)(end - start);
    while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\r' ||
                          start[length - 1] == '\n' || start[length - 1] == '\t')) {
        length--;
    }
    if (length >= size) {
        length = size - 1u;
    }
    memcpy(out, start, length);
    out[length] = '\0';
    return end + strlen(close);
}

/* ------------------------------------------------------------------------ */
/* Finding the default gateway                                              */
/* ------------------------------------------------------------------------ */

static int findGateway(portmap *pm)
{
    const char *name = pm->binding.active ? pm->binding.name : NULL;
    uint32_t    index = pm->binding.active ? pm->binding.index : 0u;
    uint32_t    gateway = 0;
    size_t      size = 16384u;
    void       *scratch = NULL;
    int32_t     result = UV_ENOBUFS;
    int         tries;

    /* The routing table is read into memory of ours (platform.c allocates
     * nothing); grow it if the table is larger. */
    for (tries = 0; tries < 4 && result == UV_ENOBUFS; tries++) {
        if (scratch != NULL) {
            pm->context->allocator.free(scratch);
        }
        scratch = allocate(pm, size);
        if (scratch == NULL) {
            return 0;
        }
        result = platformDefaultGateway(name, index, scratch, &size, &gateway);
    }
    pm->context->allocator.free(scratch);
    if (result != 0) {
        return 0;
    }
    pm->gateway.sin_addr.s_addr = gateway;
    return 1;
}

/* Our address as the router sees it (for UPnP's NewInternalClient). */
static void findLocalAddress(portmap *pm, const struct sockaddr_in *router)
{
    struct sockaddr_storage local;
    int                     length = (int)sizeof(local);

    snprintf(pm->localAddress, sizeof(pm->localAddress), "0.0.0.0");
    if (pm->binding.active && pm->binding.hasIPv4) {
        uv_ip4_name(&pm->binding.ipv4, pm->localAddress, sizeof(pm->localAddress));
        return;
    }
    /* Connecting a UDP socket sends nothing but picks the outgoing address. */
    if (pm->probeOpen || uv_udp_init(pm->loop, &pm->probe) != 0) {
        return;
    }
    pm->probe.data = pm;
    pm->probeOpen = 1;
    pm->openHandles++;
    if (uv_udp_connect(&pm->probe, (const struct sockaddr *)router) == 0 &&
        uv_udp_getsockname(&pm->probe, (struct sockaddr *)&local, &length) == 0 &&
        local.ss_family == AF_INET) {
        uv_ip4_name((struct sockaddr_in *)&local, pm->localAddress, sizeof(pm->localAddress));
    }
    closeUdp(pm, &pm->probe, &pm->probeOpen);
}

/* ------------------------------------------------------------------------ */
/* NAT-PMP                                                                  */
/* ------------------------------------------------------------------------ */

static void startUpnp(portmap *pm);
static void scheduleRenewal(portmap *pm, uint32_t seconds);

static void natpmpSend(portmap *pm)
{
    uint8_t  packet[12];
    uv_buf_t buffer;
    uint32_t lifetime = (pm->state == STATE_REMOVING) ? 0u : PORTMAP_LIFETIME;

    if (pm->state == STATE_NATPMP_ADDRESS) {
        packet[0] = 0;                              /* version */
        packet[1] = 0;                              /* opcode: public address */
        buffer = uv_buf_init((char *)packet, 2);
    } else {
        packet[0] = 0;
        packet[1] = 2;                              /* opcode: map TCP */
        packet[2] = 0;
        packet[3] = 0;
        packet[4] = (uint8_t)(pm->port >> 8);       /* internal port */
        packet[5] = (uint8_t)(pm->port & 0xffu);
        packet[6] = (uint8_t)(pm->port >> 8);       /* suggested external port; */
        packet[7] = (uint8_t)(pm->port & 0xffu);    /* 0 together with lifetime 0 */
        if (lifetime == 0) {
            packet[6] = 0;
            packet[7] = 0;
        }
        packet[8] = (uint8_t)(lifetime >> 24);
        packet[9] = (uint8_t)(lifetime >> 16);
        packet[10] = (uint8_t)(lifetime >> 8);
        packet[11] = (uint8_t)(lifetime & 0xffu);
        buffer = uv_buf_init((char *)packet, 12);
    }
    (void)uv_udp_try_send(&pm->natpmp, &buffer, 1, (const struct sockaddr *)&pm->gateway);
}

static void onNatpmpTimer(uv_timer_t *timer)
{
    portmap *pm = timer->data;

    if (pm->state != STATE_NATPMP_MAP && pm->state != STATE_NATPMP_ADDRESS &&
        pm->state != STATE_REMOVING) {
        return;
    }
    if (++pm->attempts < PORTMAP_NATPMP_TRIES) {
        natpmpSend(pm);
        startTimer(pm, 250u << pm->attempts, onNatpmpTimer);
        return;
    }
    if (pm->state == STATE_REMOVING) {
        closeUdp(pm, &pm->natpmp, &pm->natpmpOpen); /* gave it our best */
        return;
    }
    if (pm->state == STATE_NATPMP_ADDRESS) {
        pm->state = STATE_MAPPED;                   /* mapped, address unknown */
        report(pm, PORTMAP_OK, NULL);
        scheduleRenewal(pm, pm->result.lifetime / 2u);
        return;
    }
    if (pm->result.method == PORTMAP_METHOD_NATPMP) {
        report(pm, PORTMAP_ERROR, "the router stopped answering NAT-PMP renewals");
        scheduleRenewal(pm, PORTMAP_RETRY_SECONDS);
        return;
    }
    closeUdp(pm, &pm->natpmp, &pm->natpmpOpen);
    startUpnp(pm);                                  /* no NAT-PMP here */
}

static void onNatpmpReceive(uv_udp_t *udp, ssize_t count, const uv_buf_t *buffer,
                            const struct sockaddr *from, unsigned flags)
{
    portmap       *pm = udp->data;
    const uint8_t *data = (const uint8_t *)buffer->base;
    uint16_t       result;

    (void)flags;
    if (count < 8 || from == NULL || from->sa_family != AF_INET ||
        ((const struct sockaddr_in *)from)->sin_addr.s_addr != pm->gateway.sin_addr.s_addr ||
        data[0] != 0) {
        return;
    }
    result = (uint16_t)((data[2] << 8) | data[3]);
    if (pm->state == STATE_REMOVING) {
        if (data[1] == 130u) {
            finishRemoval(pm);                      /* the router confirmed */
        }
        return;
    }
    if (pm->state == STATE_NATPMP_MAP && data[1] == 130u && count >= 16) {
        if (result != 0) {
            char message[128];

            snprintf(message, sizeof(message), "the router refused NAT-PMP (result code %u)",
                     (unsigned)result);
            if (pm->result.method == PORTMAP_METHOD_NATPMP) {
                report(pm, PORTMAP_EREFUSED, message);      /* a renewal failed */
                scheduleRenewal(pm, PORTMAP_RETRY_SECONDS);
                return;
            }
            closeUdp(pm, &pm->natpmp, &pm->natpmpOpen);
            startUpnp(pm);
            return;
        }
        pm->result.method = PORTMAP_METHOD_NATPMP;
        pm->result.externalPort = (uint16_t)((data[10] << 8) | data[11]);
        pm->result.lifetime = ((uint32_t)data[12] << 24) | ((uint32_t)data[13] << 16) |
                              ((uint32_t)data[14] << 8) | data[15];
        if (pm->reported && pm->result.status == PORTMAP_OK) {
            pm->state = STATE_MAPPED;               /* a quiet renewal */
            scheduleRenewal(pm, pm->result.lifetime / 2u);
            return;
        }
        pm->state = STATE_NATPMP_ADDRESS;
        pm->attempts = 0;
        natpmpSend(pm);
        startTimer(pm, 250u, onNatpmpTimer);
        return;
    }
    if (pm->state == STATE_NATPMP_ADDRESS && data[1] == 128u && count >= 12 && result == 0) {
        struct sockaddr_in address;

        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        memcpy(&address.sin_addr, data + 8, 4);
        uv_ip4_name(&address, pm->result.externalAddress, sizeof(pm->result.externalAddress));
        uv_timer_stop(&pm->timer);
        pm->state = STATE_MAPPED;
        report(pm, PORTMAP_OK, NULL);
        scheduleRenewal(pm, pm->result.lifetime / 2u);
    }
}

static int openUdp(portmap *pm, uv_udp_t *udp, uint8_t *open, uv_udp_recv_cb onReceive)
{
    int result = netUdpInit(pm->loop, udp, &pm->binding, AF_INET);

    if (result != 0) {
        return result;
    }
    udp->data = pm;
    *open = 1;
    pm->openHandles++;
    result = netUdpBind(udp, &pm->binding, AF_INET);
    if (result == 0 && !pm->binding.active) {
        struct sockaddr_in any;

        uv_ip4_addr("0.0.0.0", 0, &any);
        result = uv_udp_bind(udp, (const struct sockaddr *)&any, 0);
    }
    if (result == 0) {
        result = uv_udp_recv_start(udp, onAlloc, onReceive);
    }
    return result;
}

static void startNatpmp(portmap *pm)
{
    char router[64];

    if (!pm->haveGateway) {
        startUpnp(pm);
        return;
    }
    uv_ip4_name(&pm->gateway, router, sizeof(router));
    snprintf(pm->result.router, sizeof(pm->result.router), "%s", router);
    if (!pm->natpmpOpen && openUdp(pm, &pm->natpmp, &pm->natpmpOpen, onNatpmpReceive) != 0) {
        closeUdp(pm, &pm->natpmp, &pm->natpmpOpen);
        startUpnp(pm);
        return;
    }
    pm->state = STATE_NATPMP_MAP;
    pm->attempts = 0;
    natpmpSend(pm);
    startTimer(pm, 250u, onNatpmpTimer);
}

/* ------------------------------------------------------------------------ */
/* UPnP                                                                     */
/* ------------------------------------------------------------------------ */

static void soapRequest(portmap *pm, const char *action, const char *arguments);

static void upnpFail(portmap *pm, int32_t status, const char *message)
{
    pm->http = NULL;
    closeUdp(pm, &pm->ssdpSocket, &pm->ssdpOpen);
    if (pm->result.method == PORTMAP_METHOD_UPNP && pm->reported) {
        report(pm, status, message);                /* a renewal failed */
        scheduleRenewal(pm, PORTMAP_RETRY_SECONDS);
        return;
    }
    pm->result.method = PORTMAP_METHOD_NONE;
    report(pm, status, message);
}

static void ssdpSend(portmap *pm)
{
    static const char *search =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "\r\n";
    uv_buf_t buffer = uv_buf_init((char *)search, (unsigned int)strlen(search));

    (void)uv_udp_try_send(&pm->ssdpSocket, &buffer, 1, (const struct sockaddr *)&pm->ssdp);
}

static void onDescription(void *privateArg, const httpResponse *response);

static void onSsdpReceive(uv_udp_t *udp, ssize_t count, const uv_buf_t *buffer,
                          const struct sockaddr *from, unsigned flags)
{
    portmap    *pm = udp->data;
    char        text[2048];
    const char *line;
    int32_t     status;

    (void)from;
    (void)flags;
    if (pm->state != STATE_SSDP || count <= 0) {
        return;
    }
    memcpy(text, buffer->base, (size_t)count < sizeof(text) - 1u ? (size_t)count : sizeof(text) - 1u);
    text[(size_t)count < sizeof(text) - 1u ? (size_t)count : sizeof(text) - 1u] = '\0';
    for (line = text; line != NULL && *line != '\0'; line = strstr(line, "\n")) {
        size_t length;

        if (*line == '\n') {
            line++;
        }
        if (strncmp(line, "LOCATION:", 9) != 0 && strncmp(line, "Location:", 9) != 0 &&
            strncmp(line, "location:", 9) != 0) {
            continue;
        }
        line += 9;
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        length = strcspn(line, "\r\n");
        if (length == 0 || length >= sizeof(pm->location) || strncmp(line, "http://", 7) != 0) {
            return;
        }
        memcpy(pm->location, line, length);
        pm->location[length] = '\0';
        pm->state = STATE_DESCRIPTION;
        uv_timer_stop(&pm->timer);
        closeUdp(pm, &pm->ssdpSocket, &pm->ssdpOpen);
        status = httpGet(pm->location, PORTMAP_HTTP_TIMEOUT, PORTMAP_MAX_RESPONSE, onDescription,
                         pm, &pm->http);
        if (status != HTTP_OK) {
            upnpFail(pm, PORTMAP_ERROR, "could not read the router's UPnP description");
        }
        return;
    }
}

static void onSsdpTimer(uv_timer_t *timer)
{
    portmap *pm = timer->data;

    if (pm->state != STATE_SSDP) {
        return;
    }
    if (++pm->attempts < 2u) {
        ssdpSend(pm);
        startTimer(pm, PORTMAP_SSDP_WAIT_MS, onSsdpTimer);
        return;
    }
    upnpFail(pm, PORTMAP_ENOROUTER, "no router answered NAT-PMP or UPnP");
}

static void startUpnp(portmap *pm)
{
    pm->state = STATE_SSDP;
    pm->attempts = 0;
    if (!pm->ssdpOpen && openUdp(pm, &pm->ssdpSocket, &pm->ssdpOpen, onSsdpReceive) != 0) {
        upnpFail(pm, PORTMAP_ERROR, "could not open a socket for UPnP discovery");
        return;
    }
    if (pm->binding.active && pm->binding.hasIPv4) {
        char address[64];

        uv_ip4_name(&pm->binding.ipv4, address, sizeof(address));
        (void)uv_udp_set_multicast_interface(&pm->ssdpSocket, address);
    }
    ssdpSend(pm);
    startTimer(pm, PORTMAP_SSDP_WAIT_MS / 2u, onSsdpTimer);
}

/* "http://host:port" of a URL, into 'out'. */
static void urlBase(const char *url, char *out, size_t size)
{
    const char *path = strchr(url + 7, '/');
    size_t      length = (path != NULL) ? (size_t)(path - url) : strlen(url);

    if (length >= size) {
        length = size - 1u;
    }
    memcpy(out, url, length);
    out[length] = '\0';
}

static void onDescription(void *privateArg, const httpResponse *response)
{
    portmap    *pm = privateArg;
    char       *xml;
    const char *at;
    char        type[128];
    char        control[PORTMAP_URL_LENGTH];
    char        base[PORTMAP_URL_LENGTH];
    int         found = 0;

    pm->http = NULL;
    if (pm->ended) {
        return;
    }
    if (response->status != HTTP_OK || response->statusCode != 200 || response->body == NULL) {
        upnpFail(pm, PORTMAP_ERROR, "could not read the router's UPnP description");
        return;
    }
    xml = allocate(pm, (size_t)response->bodyLength + 1u);
    if (xml == NULL) {
        upnpFail(pm, PORTMAP_ENOMEM, "out of memory");
        return;
    }
    memcpy(xml, response->body, (size_t)response->bodyLength);

    /* Prefer WANIPConnection; WANPPPConnection (DSL modems) will do. */
    for (at = xml; (at = xmlValue(xml, at, "serviceType", type, sizeof(type))) != NULL;) {
        int ip = strstr(type, ":WANIPConnection:") != NULL;
        int ppp = strstr(type, ":WANPPPConnection:") != NULL;

        if ((ip || (ppp && !found)) &&
            xmlValue(xml, at, "controlURL", control, sizeof(control)) != NULL) {
            snprintf(pm->serviceType, sizeof(pm->serviceType), "%s", type);
            snprintf(pm->controlUrl, sizeof(pm->controlUrl), "%s", control);
            found = 1;
            if (ip) {
                break;
            }
        }
    }
    if (xmlValue(xml, NULL, "URLBase", base, sizeof(base)) == NULL || base[0] == '\0') {
        urlBase(pm->location, base, sizeof(base));
    }
    pm->context->allocator.free(xml);
    if (!found) {
        upnpFail(pm, PORTMAP_ERROR, "the UPnP device is not an internet gateway");
        return;
    }
    if (strncmp(pm->controlUrl, "http://", 7) != 0) {
        char relative[PORTMAP_URL_LENGTH];
        size_t baseLength = strlen(base);

        snprintf(relative, sizeof(relative), "%s", pm->controlUrl);
        while (baseLength > 0 && base[baseLength - 1u] == '/') {
            base[--baseLength] = '\0';
        }
        snprintf(pm->controlUrl, sizeof(pm->controlUrl), "%.200s%s%.300s", base,
                 (relative[0] == '/') ? "" : "/", relative);
    }
    {
        char               host[PORTMAP_URL_LENGTH];
        const char        *start = pm->controlUrl + 7;
        size_t             length = strcspn(start, ":/");
        struct sockaddr_in router;

        snprintf(host, sizeof(host), "%.*s", (int)length, start);
        snprintf(pm->result.router, sizeof(pm->result.router), "%.255s", host);
        memset(&router, 0, sizeof(router));
        if (uv_ip4_addr(host, 1900, &router) == 0) {
            findLocalAddress(pm, &router);
        } else {
            findLocalAddress(pm, &pm->gateway);
        }
    }
    pm->lease = PORTMAP_LIFETIME;
    pm->state = STATE_UPNP_ADD;
    soapRequest(pm, "AddPortMapping", NULL);
}

static void onSoap(void *privateArg, const httpResponse *response);

/* Send a SOAP action to the router's control URL. */
static void soapRequest(portmap *pm, const char *action, const char *arguments)
{
    char    body[2048];
    char    headers[256];
    char    generated[1024];
    int32_t status;
    int     length;

    if (arguments == NULL && strcmp(action, "AddPortMapping") == 0) {
        snprintf(generated, sizeof(generated),
                 "<NewRemoteHost></NewRemoteHost>"
                 "<NewExternalPort>%u</NewExternalPort>"
                 "<NewProtocol>TCP</NewProtocol>"
                 "<NewInternalPort>%u</NewInternalPort>"
                 "<NewInternalClient>%s</NewInternalClient>"
                 "<NewEnabled>1</NewEnabled>"
                 "<NewPortMappingDescription>%s</NewPortMappingDescription>"
                 "<NewLeaseDuration>%u</NewLeaseDuration>",
                 (unsigned)pm->port, (unsigned)pm->port, pm->localAddress, pm->description,
                 (unsigned)pm->lease);
        arguments = generated;
    } else if (arguments == NULL && strcmp(action, "DeletePortMapping") == 0) {
        snprintf(generated, sizeof(generated),
                 "<NewRemoteHost></NewRemoteHost>"
                 "<NewExternalPort>%u</NewExternalPort>"
                 "<NewProtocol>TCP</NewProtocol>",
                 (unsigned)pm->port);
        arguments = generated;
    }
    length = snprintf(body, sizeof(body),
                      "<?xml version=\"1.0\"?>\r\n"
                      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                      "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
                      action, pm->serviceType, (arguments != NULL) ? arguments : "", action);
    snprintf(headers, sizeof(headers), "SOAPAction: \"%s#%s\"\r\n", pm->serviceType, action);
    status = httpPost(pm->controlUrl, "text/xml; charset=\"utf-8\"", headers,
                      (const uint8_t *)body, (uint64_t)length,
                      (pm->state == STATE_REMOVING) ? PORTMAP_END_TIMEOUT : PORTMAP_HTTP_TIMEOUT,
                      PORTMAP_MAX_RESPONSE, onSoap, pm, &pm->http);
    if (status != HTTP_OK) {
        pm->http = NULL;
        if (pm->state != STATE_REMOVING) {
            upnpFail(pm, PORTMAP_ERROR, "could not send a request to the router");
        }
    }
}

static void onSoap(void *privateArg, const httpResponse *response)
{
    portmap *pm = privateArg;
    char     text[2048];
    char     value[256];
    size_t   length;

    pm->http = NULL;
    if (pm->ended) {
        finishRemoval(pm);                          /* the removal finished */
        return;
    }
    if (response->status != HTTP_OK) {
        upnpFail(pm, PORTMAP_ERROR, "the router did not answer the UPnP request");
        return;
    }
    length = (response->body != NULL) ? (size_t)response->bodyLength : 0u;
    if (length >= sizeof(text)) {
        length = sizeof(text) - 1u;
    }
    if (length > 0) {
        memcpy(text, response->body, length);
    }
    text[length] = '\0';

    if (response->statusCode != 200) {
        long code = (xmlValue(text, NULL, "errorCode", value, sizeof(value)) != NULL)
                        ? strtol(value, NULL, 10) : 0;
        char message[PORTMAP_TEXT_LENGTH];

        if (pm->state == STATE_UPNP_ADD && code == 725 && pm->lease != 0) {
            pm->lease = 0;                          /* only permanent mappings */
            soapRequest(pm, "AddPortMapping", NULL);
            return;
        }
        if (pm->state == STATE_UPNP_ADDRESS) {
            pm->state = STATE_MAPPED;               /* mapped; address unknown */
            report(pm, PORTMAP_OK, NULL);
            scheduleRenewal(pm, pm->lease ? pm->lease / 2u : PORTMAP_PERMANENT_RENEW);
            return;
        }
        if (code == 718) {
            snprintf(message, sizeof(message),
                     "the router already forwards port %u to another computer",
                     (unsigned)pm->port);
        } else {
            char description[128] = "";

            xmlValue(text, NULL, "errorDescription", description, sizeof(description));
            snprintf(message, sizeof(message), "the router refused (UPnP error %ld%s%s)", code,
                     (description[0] != '\0') ? ": " : "", description);
        }
        upnpFail(pm, PORTMAP_EREFUSED, message);
        return;
    }
    if (pm->state == STATE_UPNP_ADD) {
        pm->result.method = PORTMAP_METHOD_UPNP;
        pm->result.externalPort = pm->port;
        pm->result.lifetime = pm->lease;
        if (pm->reported && pm->result.status == PORTMAP_OK) {
            pm->state = STATE_MAPPED;               /* a quiet renewal */
            scheduleRenewal(pm, pm->lease ? pm->lease / 2u : PORTMAP_PERMANENT_RENEW);
            return;
        }
        pm->state = STATE_UPNP_ADDRESS;
        soapRequest(pm, "GetExternalIPAddress", "");
        return;
    }
    if (pm->state == STATE_UPNP_ADDRESS) {
        if (xmlValue(text, NULL, "NewExternalIPAddress", value, sizeof(value)) != NULL) {
            snprintf(pm->result.externalAddress, sizeof(pm->result.externalAddress), "%.63s",
                     value);
        }
        pm->state = STATE_MAPPED;
        report(pm, PORTMAP_OK, NULL);
        scheduleRenewal(pm, pm->lease ? pm->lease / 2u : PORTMAP_PERMANENT_RENEW);
    }
}

/* ------------------------------------------------------------------------ */
/* Renewing                                                                 */
/* ------------------------------------------------------------------------ */

static void onRenewTimer(uv_timer_t *timer)
{
    portmap *pm = timer->data;

    if (pm->ended) {
        return;
    }
    if (pm->result.method == PORTMAP_METHOD_NATPMP) {
        pm->state = STATE_NATPMP_MAP;
        pm->attempts = 0;
        natpmpSend(pm);
        startTimer(pm, 250u, onNatpmpTimer);
    } else if (pm->result.method == PORTMAP_METHOD_UPNP) {
        pm->state = STATE_UPNP_ADD;
        soapRequest(pm, "AddPortMapping", NULL);
    }
}

static void scheduleRenewal(portmap *pm, uint32_t seconds)
{
    if (seconds < 30u) {
        seconds = 30u;
    }
    startTimer(pm, (uint64_t)seconds * 1000u, onRenewTimer);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t portmapBegin(uv_loop_t *loop, parserContext *context, const portmapOptions *options,
                     portmapCallback callback, void *privateArg, portmap **mapper)
{
    httpConfig config;
    portmap   *pm;

    if (mapper != NULL) {
        *mapper = NULL;
    }
    if (loop == NULL || context == NULL || options == NULL || mapper == NULL ||
        options->port == 0) {
        return PORTMAP_EINVAL;
    }
    pm = context->allocator.realloc(NULL, sizeof(*pm));
    if (pm == NULL) {
        return PORTMAP_ENOMEM;
    }
    memset(pm, 0, sizeof(*pm));
    pm->loop = loop;
    pm->context = context;
    pm->callback = callback;
    pm->privateArg = privateArg;
    pm->port = options->port;
    snprintf(pm->description, sizeof(pm->description), "%s",
             (options->description != NULL) ? options->description : "torrentinfo");
    if (options->binding != NULL) {
        pm->binding = *options->binding;
    }
    pm->gateway.sin_family = AF_INET;
    pm->gateway.sin_port = htons(PORTMAP_NATPMP_PORT);
    if (options->gateway != NULL) {
        pm->haveGateway = (uint8_t)(inet_pton(AF_INET, options->gateway,
                                              &pm->gateway.sin_addr) == 1);
    } else {
        pm->haveGateway = (uint8_t)findGateway(pm);
    }
    if (uv_ip4_addr((options->ssdpAddress != NULL) ? options->ssdpAddress : PORTMAP_SSDP_ADDRESS,
                    (options->ssdpPort != 0) ? options->ssdpPort : (int)PORTMAP_SSDP_PORT,
                    &pm->ssdp) != 0) {
        context->allocator.free(pm);
        return PORTMAP_EINVAL;
    }
    memset(&config, 0, sizeof(config));
    config.binding = options->binding;
    pm->httpUp = (uint8_t)(httpInit(loop, context, &config) == HTTP_OK);

    uv_timer_init(loop, &pm->timer);
    pm->timer.data = pm;
    pm->openHandles = 1;
    *mapper = pm;
    startNatpmp(pm);
    return PORTMAP_OK;
}

static void onRemoveTimeout(uv_timer_t *timer)
{
    finishRemoval(timer->data);
}

void portmapEnd(portmap *pm)
{
    if (pm == NULL || pm->ended) {
        return;
    }
    pm->ended = 1;
    if (pm->http != NULL) {
        httpCancel(pm->http);
        pm->http = NULL;
    }
    closeUdp(pm, &pm->ssdpSocket, &pm->ssdpOpen);
    uv_timer_stop(&pm->timer);

    if (pm->result.status == PORTMAP_OK && pm->result.method == PORTMAP_METHOD_NATPMP &&
        pm->natpmpOpen) {
        pm->state = STATE_REMOVING;                 /* lifetime 0 removes it */
        natpmpSend(pm);
        natpmpSend(pm);                             /* UDP: say it twice */
    } else if (pm->result.status == PORTMAP_OK && pm->result.method == PORTMAP_METHOD_UPNP &&
               pm->reported) {
        pm->state = STATE_REMOVING;
        soapRequest(pm, "DeletePortMapping", NULL);
    }
    if (pm->state != STATE_REMOVING || (pm->result.method == PORTMAP_METHOD_UPNP &&
                                        pm->http == NULL)) {
        finishRemoval(pm);                          /* nothing to wait for */
        return;
    }
    uv_timer_start(&pm->timer, onRemoveTimeout, (uint64_t)PORTMAP_END_TIMEOUT * 1000u, 0);
}

const char *portmapMethodName(uint32_t method)
{
    switch (method) {
    case PORTMAP_METHOD_NATPMP: return "NAT-PMP";
    case PORTMAP_METHOD_UPNP:   return "UPnP";
    default:                    return "none";
    }
}
