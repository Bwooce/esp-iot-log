/*
 * ESP IoT Log - Zephyr/Thread Port
 * Wire-compatible with the Arduino/ESP-IDF libraries.
 *
 * Sends binary log packets via IPv6 multicast over OpenThread.
 * Uses Zephyr BSD sockets (works over the OpenThread IP stack).
 *
 * Key differences from ESP-IDF port:
 *  - IPv6 multicast (ff05::e510) instead of IPv4 (239.255.1.100)
 *  - Beacon-based discovery instead of mDNS (Thread has no mDNS).
 *    Python receiver sends periodic beacons; device only logs when
 *    a beacon has been recently received. Configurable always_on mode.
 *  - Device ID from IEEE 802.15.4 EUI-64
 *  - Thread-safe via Zephyr k_mutex
 */

#include "iot_log_zephyr.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/openthread.h>

#include <openthread/thread.h>
#include <openthread/link.h>
#include <openthread/ip6.h>
#include <openthread/instance.h>

/* v3.3: openthread_context.instance is no longer populated by the L2 layer.
 * The new singleton accessor lives in the openthread module's internal
 * header which we don't pull in directly. Forward-declare here. */
struct otInstance *openthread_get_default_instance(void);

#include <zephyr/logging/log_ctrl.h>     /* log_backend_get_by_name */
#include <zephyr/logging/log_backend.h>  /* struct log_backend */

#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(iot_log, LOG_LEVEL_INF);

/* CRC16-CCITT (must match Arduino library and Python receiver) */
static uint16_t crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* Internal state */
static struct {
    bool            initialised;
    iot_log_level_t level;
    bool            serial_mirror;
    bool            always_on;
    bool            listener_active;

    int             sock;           /* TX socket (sendto multicast) */
    int             rx_sock;        /* RX socket (recvfrom for beacons) */
    struct sockaddr_in6 mcast_addr;

    uint64_t        device_id;
    char            device_name[32];

    int64_t         last_beacon_ms; /* k_uptime when last beacon received */
    bool            mcast_joined;  /* True once OT multicast subscription done */
    bool            beacon_confirmed; /* True after first real beacon received */
    char            mcast_ip[48];  /* Saved for deferred join */

    uint32_t        sent_count;
    uint32_t        dropped_count;
} s_log;

static K_MUTEX_DEFINE(s_log_mutex);

/* Get device ID from OpenThread extended address (EUI-64) */
static uint64_t get_device_id(void)
{
    uint64_t id = 0;
    struct openthread_context *ot_context = openthread_get_default_context();

    if (ot_context) {
        openthread_api_mutex_lock(ot_context);
        const otExtAddress *ext = otLinkGetExtendedAddress(openthread_get_default_instance());
        if (ext) {
            /* Pack 8-byte EUI-64 into uint64_t, little-endian to match ESP MAC format */
            for (int i = 7; i >= 0; i--) {
                id = (id << 8) | ext->m8[i];
            }
        }
        openthread_api_mutex_unlock(ot_context);
    }

    return id;
}

/* Check if Thread network is attached and has a routable address */
static bool is_thread_attached(void)
{
    struct openthread_context *ot_context = openthread_get_default_context();
    if (!ot_context) {
        return false;
    }

    bool attached = false;
    openthread_api_mutex_lock(ot_context);
    otDeviceRole role = otThreadGetDeviceRole(openthread_get_default_instance());
    attached = (role == OT_DEVICE_ROLE_CHILD ||
                role == OT_DEVICE_ROLE_ROUTER ||
                role == OT_DEVICE_ROLE_LEADER);
    openthread_api_mutex_unlock(ot_context);

    return attached;
}

/* Check for beacon packets from the Python receiver (non-blocking) */
static void check_for_beacons(void)
{
    if (s_log.rx_sock < 0) {
        return;
    }

    uint8_t buf[32];
    struct sockaddr_in6 src;
    socklen_t src_len = sizeof(src);

    /* Drain all pending beacon packets */
    for (;;) {
        ssize_t n = zsock_recvfrom(s_log.rx_sock, buf, sizeof(buf), ZSOCK_MSG_DONTWAIT,
                                   (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            break;
        }

        /* Validate: at least header (18) + CRC (2) = 20 bytes */
        if (n < 20) {
            continue;
        }

        /* Check magic */
        uint16_t magic = buf[0] | (buf[1] << 8);
        if (magic != IOT_LOG_MAGIC) {
            continue;
        }

        /* Check type = BEACON (0xFF)
         * Header layout: magic(2) + ver(1) + devid(8) + ts(4) + type(1) + len(2) = 18
         * log_type is at byte offset 15 */
        uint8_t type = buf[15];
        if (type == IOT_LOG_BEACON_TYPE) {
            bool was_active = s_log.listener_active;
            s_log.last_beacon_ms = k_uptime_get();
            s_log.listener_active = true;
            s_log.beacon_confirmed = true;
            if (!was_active) {
                LOG_INF("Log listener discovered via beacon");
            }
        }
    }
}

/* Send a raw log message using the binary protocol */
static bool send_message(iot_log_type_t type, const uint8_t *payload, size_t payload_len)
{
    if (!s_log.initialised) {
        return false;
    }

    /* Check if we should send */
    if (!is_thread_attached()) {
        s_log.dropped_count++;
        return false;
    }

    if (!s_log.always_on && !s_log.listener_active) {
        s_log.dropped_count++;
        return false;
    }

    /* Header: magic(2) + version(1) + device_id(8) + timestamp(4) + type(1) + length(2) = 18 */
    const size_t HEADER_SIZE = 18;
    size_t total_size = HEADER_SIZE + payload_len + 2; /* +2 for CRC16 */
    if (total_size > 1024) {
        s_log.dropped_count++;
        return false;
    }

    /* Static — protected by s_log_mutex which all callers hold. Keeping this
     * off the workqueue stack matters: system_workq is only 2KB on this
     * platform, and a 1KB stack buffer in this path crashed the device with
     * a MemManage fault during heavy iot_log activity. */
    static uint8_t packet[1024];
    size_t pos = 0;

    /* Magic (little-endian) */
    packet[pos++] = IOT_LOG_MAGIC & 0xFF;
    packet[pos++] = (IOT_LOG_MAGIC >> 8) & 0xFF;

    /* Protocol version */
    packet[pos++] = IOT_LOG_PROTOCOL_VERSION;

    /* Device ID (little-endian, 8 bytes) */
    uint64_t did = s_log.device_id;
    for (int i = 0; i < 8; i++) {
        packet[pos++] = did & 0xFF;
        did >>= 8;
    }

    /* Timestamp in ms (little-endian, 4 bytes) — Zephyr uptime */
    uint32_t ts_ms = k_uptime_get_32();
    packet[pos++] = ts_ms & 0xFF;
    packet[pos++] = (ts_ms >> 8) & 0xFF;
    packet[pos++] = (ts_ms >> 16) & 0xFF;
    packet[pos++] = (ts_ms >> 24) & 0xFF;

    /* Log type */
    packet[pos++] = (uint8_t)type;

    /* Payload length (little-endian, 2 bytes) */
    packet[pos++] = payload_len & 0xFF;
    packet[pos++] = (payload_len >> 8) & 0xFF;

    /* Payload */
    if (payload_len > 0) {
        memcpy(packet + pos, payload, payload_len);
    }

    /* CRC16 over header + payload */
    uint16_t crc = crc16_ccitt(packet, HEADER_SIZE + payload_len);
    packet[HEADER_SIZE + payload_len]     = crc & 0xFF;
    packet[HEADER_SIZE + payload_len + 1] = (crc >> 8) & 0xFF;

    ssize_t sent = zsock_sendto(s_log.sock, packet, total_size, 0,
                                (struct sockaddr *)&s_log.mcast_addr,
                                sizeof(s_log.mcast_addr));
    if (sent == (ssize_t)total_size) {
        s_log.sent_count++;
        return true;
    } else {
        if (s_log.dropped_count < 3) {
            /* Log first few failures to diagnose, avoid spam */
            LOG_WRN("sendto failed: %d (errno %d), total=%u",
                    (int)sent, errno, s_log.dropped_count + 1);
        }
        s_log.dropped_count++;
        return false;
    }
}

/* Self-driving poll work — keeps draining and beacon-checking even when the
 * application is blocked in a long-running call (DNS, TLS handshake, etc.).
 * Without this, the optimistic-grace window can expire while messages sit
 * undelivered in the ring buffer, and they'd be dropped at drain time. */
#define IOT_LOG_POLL_INTERVAL K_MSEC(1000)
static void iot_log_poll_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(iot_log_poll_work, iot_log_poll_work_handler);

static void iot_log_poll_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    iot_log_poll();

    /* Log only when state changes (listener-active edge, attachment edge,
     * or drops have grown) so we get visibility into something going wrong
     * without spamming the log when everything's fine. */
    if (s_log.initialised) {
        static int8_t  prev_attached = -1;
        static int8_t  prev_active = -1;
        static uint32_t prev_dropped = 0;
        static uint32_t prev_calls = 0;
        static uint32_t prev_queued = 0;
        bool attached_now = is_thread_attached();
        bool active_now = s_log.listener_active;

        /* Pull backend stats so the status emit can show why messages
         * aren't flowing when sent_count stays at 0. */
        uint32_t be_calls = 0, be_inactive = 0, be_no_fmt = 0;
        uint32_t be_zero_len = 0, be_queued = 0, be_ring_full = 0;
        log_backend_iot_get_stats(&be_calls, &be_inactive, &be_no_fmt,
                                  &be_zero_len, &be_queued, &be_ring_full);

        /* Emit on state edges, on app-side drops, or when the backend has
         * seen new calls but couldn't queue any of them — that last case
         * is the "log subsystem is calling us but we're rejecting" failure
         * mode (most often `bail_inactive` during pre-attach boot, but
         * also `ring_full` if drain can't keep up). Steady-state success
         * (calls climbing, queued climbing in lockstep) stays quiet. */
        bool emit = (prev_attached != (int8_t)attached_now) ||
                    (prev_active != (int8_t)active_now) ||
                    (s_log.dropped_count > prev_dropped) ||
                    (be_calls > prev_calls && be_queued == prev_queued);
        if (emit) {
            LOG_INF("status: sent=%u drop=%u attached=%d active=%d "
                    "be[calls=%u inactive=%u nofmt=%u zlen=%u queued=%u rfull=%u]",
                    (unsigned)s_log.sent_count,
                    (unsigned)s_log.dropped_count,
                    (int)attached_now,
                    (int)active_now,
                    (unsigned)be_calls, (unsigned)be_inactive,
                    (unsigned)be_no_fmt, (unsigned)be_zero_len,
                    (unsigned)be_queued, (unsigned)be_ring_full);
            prev_attached = attached_now;
            prev_active = active_now;
            prev_dropped = s_log.dropped_count;
            prev_calls = be_calls;
            prev_queued = be_queued;
        }

        k_work_reschedule(&iot_log_poll_work, IOT_LOG_POLL_INTERVAL);
    }
}

int iot_log_init(const iot_log_config_t *config)
{
    if (s_log.initialised) {
        return 0;
    }

    memset(&s_log, 0, sizeof(s_log));
    s_log.sock = -1;
    s_log.rx_sock = -1;

    /* Apply config or defaults */
    iot_log_config_t cfg = IOT_LOG_CONFIG_DEFAULT();
    if (config) {
        cfg = *config;
    }

    s_log.level = cfg.level;
    s_log.serial_mirror = cfg.serial_mirror;
    s_log.always_on = cfg.always_on;

    /* Get device ID from Thread EUI-64 */
    s_log.device_id = get_device_id();

    /* Device name */
    if (cfg.device_name) {
        strncpy(s_log.device_name, cfg.device_name, sizeof(s_log.device_name) - 1);
    } else {
        snprintf(s_log.device_name, sizeof(s_log.device_name),
                 "OT-%02X%02X%02X",
                 (uint8_t)(s_log.device_id >> 16),
                 (uint8_t)(s_log.device_id >> 8),
                 (uint8_t)(s_log.device_id));
    }

    const char *mcast_ip = cfg.multicast_ip6 ? cfg.multicast_ip6 : IOT_LOG_DEFAULT_MCAST_IP6;
    uint16_t mcast_port = cfg.multicast_port > 0 ? cfg.multicast_port : IOT_LOG_DEFAULT_MCAST_PORT;

    /* Create TX socket (for sending log packets) */
    s_log.sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s_log.sock < 0) {
        LOG_ERR("Failed to create TX UDP6 socket: %d", errno);
        return -1;
    }

    /* Bump multicast hop limit so packets survive the OTBR's kernel
     * mc-forwarder. Linux's ip6mr_forward2() drops the packet if
     * hop_limit <= 1 BEFORE decrementing — and silently increments the
     * mc_cache Pkts counter regardless. The default Zephyr/RFC-3493
     * IPV6_MULTICAST_HOPS is 1, so every iot_log packet was being
     * "counted as forwarded" but actually dropped at the BBR. Setting
     * this to 8 lets the BBR forward (decremented to 7), the AP
     * bridges to wifi (no further decrement), receiver gets hlim=7. */
    int hops = 8;
    if (zsock_setsockopt(s_log.sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                         &hops, sizeof(hops)) < 0) {
        LOG_WRN("Failed to set IPV6_MULTICAST_HOPS: %d", errno);
    }

    /* Set up multicast destination */
    memset(&s_log.mcast_addr, 0, sizeof(s_log.mcast_addr));
    s_log.mcast_addr.sin6_family = AF_INET6;
    s_log.mcast_addr.sin6_port = htons(mcast_port);
    zsock_inet_pton(AF_INET6, mcast_ip, &s_log.mcast_addr.sin6_addr);

    /* Save multicast IP for deferred OT subscription.
     * We can't subscribe until Thread is attached — the parent needs
     * to proxy the MLR to the PBBR. Done in iot_log_poll(). */
    strncpy(s_log.mcast_ip, mcast_ip, sizeof(s_log.mcast_ip) - 1);

    /* Create RX socket (for receiving beacons from Python receiver) */
    if (!s_log.always_on) {
        s_log.rx_sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (s_log.rx_sock < 0) {
            LOG_WRN("Failed to create RX UDP6 socket: %d (beacon discovery disabled)", errno);
            s_log.always_on = true;
        } else {
            struct sockaddr_in6 bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin6_family = AF_INET6;
            bind_addr.sin6_port = htons(mcast_port);
            /* Bind to in6addr_any to receive multicast */
            memset(&bind_addr.sin6_addr, 0, sizeof(bind_addr.sin6_addr));

            if (zsock_bind(s_log.rx_sock, (struct sockaddr *)&bind_addr,
                           sizeof(bind_addr)) < 0) {
                LOG_WRN("Failed to bind RX socket: %d", errno);
                zsock_close(s_log.rx_sock);
                s_log.rx_sock = -1;
                s_log.always_on = true;
            }
        }
    }

    s_log.initialised = true;

    /* Activate the log backend now that sockets and state are ready.
     * The backend was registered with autostart=false in
     * log_backend_iot.c so that pre-init log messages aren't silently
     * consumed and dropped before networking is up — they stay queued
     * in the deferred log buffer for delivery once a backend is
     * actually capable of handling them.
     *
     * IMPORTANT: log_backend_init() MUST be called before
     * log_backend_enable(). Without init, the backend never gets a
     * runtime ID assigned via log_backend_id_set(), so the per-backend
     * filter table doesn't route messages to our process() callback —
     * Zephyr's log_core.c only auto-inits backends declared with
     * autostart=true (log_core.c:346). Calling enable() alone sets
     * cb->active=true and lets a few process() calls through during
     * the brief window before filters lock in, then it goes silent
     * forever (was the symptom: be[calls=23 inactive=23, queued=0]). */
    {
        const struct log_backend *be = log_backend_get_by_name("log_backend_iot");
        if (be) {
            log_backend_init(be);
            log_backend_enable(be, be->cb->ctx, CONFIG_LOG_DEFAULT_LEVEL);
        }
    }

    /* Self-drive poll() so messages flow during long blocking phases
     * (DNS resolve, TLS handshake) instead of waiting for the app to
     * call iot_log_poll() from its main loop. */
    k_work_schedule(&iot_log_poll_work, IOT_LOG_POLL_INTERVAL);

    LOG_INF("Initialised: %s -> [%s]:%d (%s)", s_log.device_name, mcast_ip, mcast_port,
            s_log.always_on ? "always-on" : "beacon discovery");
    return 0;
}

void iot_log_deinit(void)
{
    if (!s_log.initialised) {
        return;
    }

    if (s_log.sock >= 0) {
        zsock_close(s_log.sock);
        s_log.sock = -1;
    }

    if (s_log.rx_sock >= 0) {
        zsock_close(s_log.rx_sock);
        s_log.rx_sock = -1;
    }

    s_log.initialised = false;
    s_log.listener_active = false;
}

/* Defined in log_backend_iot.c — drains the ring buffer from main thread */
extern void iot_log_backend_drain(void);

void iot_log_poll(void)
{
    if (!s_log.initialised) {
        return;
    }

    /* Deferred multicast join — must happen after Thread attaches so
     * the parent can include ff05::e510 in Address Registration and
     * proxy MLR.req to the PBBR. The PBBR then joins the group on
     * the backbone via MLDv2, enabling inbound multicast forwarding. */
    if (!s_log.mcast_joined && is_thread_attached()) {
        struct openthread_context *ot_ctx = openthread_get_default_context();
        if (ot_ctx) {
            otIp6Address ot_mcast;
            otIp6AddressFromString(s_log.mcast_ip, &ot_mcast);
            openthread_api_mutex_lock(ot_ctx);
            otError err = otIp6SubscribeMulticastAddress(openthread_get_default_instance(), &ot_mcast);
            openthread_api_mutex_unlock(ot_ctx);
            if (err == OT_ERROR_NONE || err == OT_ERROR_ALREADY) {
                LOG_INF("Joined multicast group [%s]", s_log.mcast_ip);
                s_log.mcast_joined = true;
                /* Optimistically assume a listener exists so we capture
                 * boot/connect logs. Will go silent after BEACON_GRACE_S
                 * if no beacon arrives to confirm. */
                if (!s_log.always_on && !s_log.listener_active) {
                    s_log.listener_active = true;
                    s_log.last_beacon_ms = k_uptime_get();
                    LOG_INF("Optimistic send active (%ds grace)",
                            IOT_LOG_BEACON_GRACE_S);
                }
            } else {
                LOG_WRN("Failed to join multicast group: %d", err);
            }
        }
    }

    /* Check for beacon packets first — beacons that arrive in the same poll
     * tick as the timeout check should refresh listener_active before any
     * expiry decision. */
    if (!s_log.always_on) {
        check_for_beacons();
    }

    /* Drain queued log messages from the Zephyr log backend BEFORE the
     * timeout check. If poll() hasn't run for a while (e.g. blocked in TLS
     * handshake), we can have a backlog of messages queued under the still-
     * valid optimistic-grace listener. Expiring the listener first would
     * cause those messages to be dropped at send time. Drain → then expire. */
    iot_log_backend_drain();

    /* Expire listener if no beacon received within timeout.
     * Uses shorter grace period until first real beacon arrives,
     * then the normal 3x-interval timeout after that. */
    if (!s_log.always_on) {
        if (s_log.listener_active && s_log.last_beacon_ms > 0) {
            int64_t elapsed = k_uptime_get() - s_log.last_beacon_ms;
            int64_t timeout = s_log.beacon_confirmed ?
                (int64_t)IOT_LOG_BEACON_TIMEOUT_S * 1000 :
                (int64_t)IOT_LOG_BEACON_GRACE_S * 1000;
            if (elapsed > timeout) {
                s_log.listener_active = false;
                LOG_INF("Log listener timed out (%s, %llds)",
                        s_log.beacon_confirmed ? "no beacon" : "grace expired",
                        elapsed / 1000);
            }
        }
    }
}

bool iot_log_is_active(void)
{
    if (!s_log.initialised || !is_thread_attached()) {
        return false;
    }
    return s_log.always_on || s_log.listener_active;
}

void iot_log(iot_log_level_t level, const char *fmt, ...)
{
    if (!s_log.initialised || level > s_log.level) {
        return;
    }

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Mirror to Zephyr LOG if enabled */
    if (s_log.serial_mirror) {
        switch (level) {
        case IOT_LOG_ERROR:   LOG_ERR("%s", msg); break;
        case IOT_LOG_WARN:    LOG_WRN("%s", msg); break;
        case IOT_LOG_INFO:    LOG_INF("%s", msg); break;
        case IOT_LOG_DEBUG:   LOG_DBG("%s", msg); break;
        case IOT_LOG_VERBOSE: LOG_DBG("%s", msg); break;
        default: break;
        }
    }

    /* Send over network */
    k_mutex_lock(&s_log_mutex, K_FOREVER);

    size_t msg_len = strlen(msg);
    if (msg_len > 500) {
        msg_len = 500;
    }

    uint8_t payload[502];
    payload[0] = (uint8_t)level;
    memcpy(payload + 1, msg, msg_len);

    send_message(IOT_LOG_TYPE_TEXT, payload, 1 + msg_len);

    k_mutex_unlock(&s_log_mutex);
}

bool iot_log_send_raw(iot_log_level_t level, const char *msg, size_t len)
{
    if (!s_log.initialised) {
        return false;
    }

    k_mutex_lock(&s_log_mutex, K_FOREVER);

    if (len > 500) {
        len = 500;
    }

    /* Static — protected by s_log_mutex. See note in send_message. */
    static uint8_t payload[502];
    payload[0] = (uint8_t)level;
    memcpy(payload + 1, msg, len);

    bool ok = send_message(IOT_LOG_TYPE_TEXT, payload, 1 + len);

    k_mutex_unlock(&s_log_mutex);
    return ok;
}

void iot_log_metric(const char *name, int32_t value)
{
    if (!s_log.initialised || !name || name[0] == '\0') {
        return;
    }

    k_mutex_lock(&s_log_mutex, K_FOREVER);

    size_t name_len = strlen(name);
    if (name_len > 255) {
        name_len = 255;
    }

    uint8_t payload[260];
    payload[0] = (uint8_t)name_len;
    memcpy(payload + 1, name, name_len);
    payload[1 + name_len]     = (uint8_t)(value);
    payload[1 + name_len + 1] = (uint8_t)(value >> 8);
    payload[1 + name_len + 2] = (uint8_t)(value >> 16);
    payload[1 + name_len + 3] = (uint8_t)(value >> 24);

    send_message(IOT_LOG_TYPE_METRIC, payload, 1 + name_len + 4);

    k_mutex_unlock(&s_log_mutex);
}

void iot_log_metric_str(const char *name, const char *value)
{
    if (!s_log.initialised || !name || name[0] == '\0') {
        return;
    }
    if (!value) {
        value = "";
    }

    k_mutex_lock(&s_log_mutex, K_FOREVER);

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (name_len > 255) {
        name_len = 255;
    }
    if (value_len > 255) {
        value_len = 255;
    }

    uint8_t payload[514];
    payload[0] = (uint8_t)name_len;
    memcpy(payload + 1, name, name_len);
    payload[1 + name_len] = (uint8_t)value_len;
    memcpy(payload + 2 + name_len, value, value_len);

    send_message(IOT_LOG_TYPE_METRIC, payload, 2 + name_len + value_len);

    k_mutex_unlock(&s_log_mutex);
}

uint32_t iot_log_get_sent_count(void)
{
    return s_log.sent_count;
}

uint32_t iot_log_get_dropped_count(void)
{
    return s_log.dropped_count;
}
