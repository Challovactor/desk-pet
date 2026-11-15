/*
 * Wi-Fi + HTTP 上传（ECW6700F）
 *
 * 流程（触摸触发）：
 *   1) POST {server}/api/record  JSON 事件；
 *   2) POST {server}/api/audio   WAV（PCM16LE 单声道 32kHz），
 *      body 从 SD 环形区流式读取，不在 RAM 里拼接整段录音。
 *
 * 传输层使用 lwip BSD socket（SDK 的 CONFIG_LWIP 已开启）。
 * 目前是明文 HTTP；以后换 HTTPS/其他协议只需要改本文件。
 */
#include "app_config.h"
#include "app_net.h"
#include "app_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "oshal.h"
#include "system_wifi.h"
#include "system_event.h"
#include "hal_sntp.h"
#include "app_mqtt.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"

#define NET_CONNECT_TIMEOUT_S   6
/* SDK 的 lwipopts.h 定义 LWIP_SO_SNDRCVTIMEO_NONSTANDARD=1，
 * SO_SNDTIMEO/SO_RCVTIMEO 的单位是 int 毫秒，不是 struct timeval。 */
#define NET_IO_TIMEOUT_MS       8000
#define NET_RETRY               2

static volatile int s_sta_ip;
static volatile int s_wifi_events_ok;
static volatile uint32_t s_upload_ok;
static volatile uint32_t s_upload_fail;

/* ------------------------------------------------------------ Wi-Fi 事件 */

static sys_err_t net_wifi_event(void *ctx, system_event_t *event)
{
    (void)ctx;
    if (event == NULL) {
        return SYS_OK;
    }
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_CONNECTED:
        os_printf(LL_INFO, "[net] sta associated\r\n");
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        s_sta_ip = 1;
        os_printf(LL_INFO, "[net] got ip\r\n");
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        s_sta_ip = 0;
        os_printf(LL_WARN, "[net] sta disconnected reason=%d\r\n",
                  event->event_info.disconnected.reason);
        if ((int)event->event_info.disconnected.reason == 15) {
            os_printf(LL_WARN, "[net] reason=15: AP 侧四次握手失败，"
                               "优先核对热点密码/安全方式\r\n");
        }
        break;
    case SYSTEM_EVENT_STA_CONNECTION_LOST:
    case SYSTEM_EVENT_STA_STOP:
    case SYSTEM_EVENT_STA_LOST_IP:
    case SYSTEM_EVENT_STA_GET_IP_FAIL:
        s_sta_ip = 0;
        os_printf(LL_WARN, "[net] link down event=%d\r\n", (int)event->event_id);
        break;
    default:
        break;
    }
    return SYS_OK;
}

void net_init(void)
{
    sys_err_t ret = sys_event_subscribe_group_app(
        EVENT_GROUP_STA_LIFECYCLE | EVENT_GROUP_STA_NETWORK,
        net_wifi_event, EVENT_APP_PRIO_NORMAL, NULL);
    if (ret != SYS_OK) {
        os_printf(LL_ERR, "[net] event subscribe failed: %d\r\n", (int)ret);
        return;
    }
    s_wifi_events_ok = 1;
}

/* --------------------------------------------------------------- socket */

static int net_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    unsigned int retries = 0;

    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            retries = 0;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++retries > 300u) {
                return -1;
            }
            os_msleep(10);
            continue;
        }
        return -1;
    }
    return 0;
}

static int net_open(const char *host, int port)
{
    const struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char portstr[8];

    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        os_printf(LL_ERR, "[net] dns failed: %s\r\n", host);
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        int flags;
        int rc;

        if (fd < 0) {
            continue;
        }
        flags = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0) {
            if (errno == EINPROGRESS || errno == EALREADY) {
                fd_set wset;
                struct timeval tv;
                int err = 0;
                socklen_t elen = sizeof(err);

                FD_ZERO(&wset);
                FD_SET(fd, &wset);
                tv.tv_sec = NET_CONNECT_TIMEOUT_S;
                tv.tv_usec = 0;
                rc = select(fd + 1, NULL, &wset, NULL, &tv);
                if (rc <= 0 ||
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 ||
                    err != 0) {
                    close(fd);
                    continue;
                }
            } else {
                close(fd);
                continue;
            }
        }

        (void)fcntl(fd, F_SETFL, flags);
        {
            int tmo = NET_IO_TIMEOUT_MS;
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
        }
        freeaddrinfo(res);
        return fd;
    }
    freeaddrinfo(res);
    os_printf(LL_ERR, "[net] connect %s:%d failed\r\n", host, port);
    return -1;
}

/* 返回 HTTP 状态码；-1 表示没有响应或解析失败。 */
static int net_read_response(int fd)
{
    char buf[192];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    int code = -1;

    if (n > 0) {
        buf[n] = '\0';
        char *eol = strstr(buf, "\r\n");
        if (eol) {
            *eol = '\0';
        }
        os_printf(LL_INFO, "[net] http: %s\r\n", buf);
        if (sscanf(buf, "HTTP/%*d.%*d %d", &code) != 1) {
            code = -1;
        }
    } else {
        os_printf(LL_WARN, "[net] http: no response\r\n");
    }
    return code;
}

/* ---------------------------------------------------------------- HTTP */

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void net_wav_header(uint8_t h[44], uint32_t data_bytes)
{
    memcpy(h + 0, "RIFF", 4);
    put_u32le(h + 4, 36u + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32le(h + 16, 16u);
    put_u16le(h + 20, 1u);                                  /* PCM */
    put_u16le(h + 22, (uint16_t)APP_PCM_CHANNELS);
    put_u32le(h + 24, APP_PCM_SAMPLE_RATE);
    put_u32le(h + 28, APP_PCM_BYTES_PER_SEC);
    put_u16le(h + 32, (uint16_t)(APP_PCM_CHANNELS * 2u));   /* block align */
    put_u16le(h + 34, 16u);                                 /* bits */
    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, data_bytes);
}

int net_report_record(uint32_t clip_bytes, uint32_t seconds, uint32_t clip_id)
{
    char json[256];
    char header[448];
    int len;
    int fd;
    int hn;
    int code;

    len = snprintf(json, sizeof(json),
                   "{\"device\":\"%s\",\"event\":\"record\",\"clip_id\":%u,"
                   "\"seconds\":%u,\"bytes\":%u,\"sample_rate\":%u,"
                   "\"format\":\"pcm_s16le\"}",
                   APP_DEVICE_ID, (unsigned)clip_id, (unsigned)seconds,
                   (unsigned)clip_bytes, (unsigned)APP_PCM_SAMPLE_RATE);
    if (len <= 0 || len >= (int)sizeof(json)) {
        return -1;
    }
    fd = net_open(APP_SERVER_HOST, APP_SERVER_PORT);
    if (fd < 0) {
        return -1;
    }
    hn = snprintf(header, sizeof(header),
                  "POST %s HTTP/1.1\r\n"
                  "Host: %s:%d\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %d\r\n"
                  "X-Device-Id: %s\r\n"
                  "X-Clip-Id: %u\r\n"
                  "Connection: close\r\n\r\n",
                  APP_SERVER_EVENT_PATH, APP_SERVER_HOST, APP_SERVER_PORT, len,
                  APP_DEVICE_ID, (unsigned)clip_id);
    if (hn <= 0 || hn >= (int)sizeof(header) ||
        net_send_all(fd, header, (size_t)hn) != 0 ||
        net_send_all(fd, json, (size_t)len) != 0) {
        os_printf(LL_ERR, "[net] event post failed\r\n");
        close(fd);
        return -1;
    }
    code = net_read_response(fd);
    close(fd);
    if (code < 200 || code >= 300) {
        os_printf(LL_ERR, "[net] event rejected: http %d\r\n", code);
        return -1;
    }
    os_printf(LL_INFO, "[net] record event sent clip=%u (%u bytes)\r\n",
              (unsigned)clip_id, (unsigned)clip_bytes);
    return 0;
}

int net_upload_clip(uint32_t start_offset, uint32_t clip_bytes, uint32_t seconds,
                    uint32_t clip_id)
{
    static uint8_t body[4096] __attribute__((aligned(4)));
    uint8_t wav[44];
    char header[384];
    uint32_t cursor = start_offset;
    uint32_t left = clip_bytes;
    int fd;
    int hn;

    if (clip_bytes == 0) {
        return -1;
    }
    for (int attempt = 0; attempt < NET_RETRY; ++attempt) {
        fd = net_open(APP_SERVER_HOST, APP_SERVER_PORT);
        if (fd < 0) {
            os_msleep(300);
            continue;
        }
        hn = snprintf(header, sizeof(header),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Content-Type: audio/wav\r\n"
                      "Content-Length: %u\r\n"
                      "X-Device-Id: %s\r\n"
                      "X-Clip-Id: %u\r\n"
                      "X-Clip-Seconds: %u\r\n"
                      "Connection: close\r\n\r\n",
                      APP_SERVER_AUDIO_PATH, APP_SERVER_HOST, APP_SERVER_PORT,
                      (unsigned)(clip_bytes + 44u), APP_DEVICE_ID,
                      (unsigned)clip_id, (unsigned)seconds);
        if (hn <= 0 || hn >= (int)sizeof(header)) {
            close(fd);
            break;
        }
        net_wav_header(wav, clip_bytes);
        if (net_send_all(fd, header, (size_t)hn) != 0 ||
            net_send_all(fd, wav, sizeof(wav)) != 0) {
            os_printf(LL_ERR, "[net] upload header failed\r\n");
            close(fd);
            os_msleep(300);
            continue;
        }

        cursor = start_offset;
        left = clip_bytes;
        int failed = 0;
        while (left > 0) {
            uint32_t n = left < sizeof(body) ? left : (uint32_t)sizeof(body);
            if (store_clip_read(&cursor, body, n) != 0) {
                os_printf(LL_ERR, "[net] sd read failed at %u\r\n", (unsigned)left);
                failed = 1;
                break;
            }
            if (net_send_all(fd, body, n) != 0) {
                os_printf(LL_ERR, "[net] send failed at %u\r\n", (unsigned)left);
                failed = 1;
                break;
            }
            left -= n;
        }
        if (failed) {
            close(fd);
            os_msleep(500);
            continue;
        }
        {
            int code = net_read_response(fd);
            close(fd);
            if (code < 200 || code >= 300) {
                os_printf(LL_ERR, "[net] clip rejected: http %d\r\n", code);
                os_msleep(500);
                continue;
            }
        }
        s_upload_ok++;
        os_printf(LL_INFO, "[net] clip %u uploaded: %u bytes, %us\r\n",
                  (unsigned)clip_id, (unsigned)clip_bytes, (unsigned)seconds);
        return 0;
    }
    s_upload_fail++;
    os_printf(LL_ERR, "[net] clip upload failed (%u)\r\n", (unsigned)s_upload_fail);
    return -1;
}

/* ------------------------------------------------------------ Wi-Fi 任务 */

int net_connected(void)
{
    return s_sta_ip ? 1 : 0;
}

int net_wait_connected(uint32_t timeout_ms)
{
    long long start = os_time_get();
    while (!s_sta_ip) {
        if ((unsigned long long)(os_time_get() - start) >= timeout_ms) {
            return -1;
        }
        os_msleep(100);
    }
    return 0;
}

static int net_wifi_connect(void)
{
    wifi_config_u cfg;
    size_t ssid_len = strlen(APP_WIFI_SSID);
    size_t pwd_len = strlen(APP_WIFI_PASSWORD);

    if (strcmp(APP_WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
        os_printf(LL_ERR, "[net] Wi-Fi 未配置：请修改 main/app_config.h\r\n");
        return -1;
    }
    if (ssid_len == 0 || ssid_len >= WIFI_SSID_MAX_LEN ||
        pwd_len >= WIFI_PWD_MAX_LEN) {
        os_printf(LL_ERR, "[net] invalid ssid/password length\r\n");
        return -1;
    }
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.sta.ssid, APP_WIFI_SSID, ssid_len);
    memcpy(cfg.sta.password, APP_WIFI_PASSWORD, pwd_len);

    if (sys_wifi_get_mode() != WIFI_MODE_STA && sys_wifi_get_mode() != WIFI_MODE_AP_STA) {
        sys_wifi_set_mode(WIFI_MODE_STA);   /* 该 API 返回 void，异步生效 */
        for (int i = 0; i < 20 && sys_wifi_get_mode() != WIFI_MODE_STA; ++i) {
            os_msleep(100);
        }
    }
    {
        sys_err_t ret = sys_wifi_sta_start(&cfg);
        if (ret == SYS_OK) {
            os_printf(LL_INFO, "[net] connecting to %s ...\r\n", APP_WIFI_SSID);
            return 0;
        }
        if (ret == SYS_ERR_WIFI_BUSY) {
            /* SDK 自带的 NV 自动连接任务正在占用 STA，等它结束即可 */
            os_printf(LL_INFO, "[net] sta busy (auto-connect in progress)\r\n");
            return 1;
        }
        os_printf(LL_ERR, "[net] sta start failed: %d\r\n", (int)ret);
        return -1;
    }
}

/* 上板诊断：扫描并打印实际看到的 AP 列表，用于判断射频/天线是否工作。 */
#if APP_WIFI_SCAN_DEBUG
static void net_scan_debug(void)
{
    wifi_scan_config_t cfg;
    int n;

    memset(&cfg, 0, sizeof(cfg));
    os_printf(LL_INFO, "[net] scanning APs...\r\n");
    if (sys_wifi_scan_start(false, &cfg) != SYS_OK) {
        os_printf(LL_ERR, "[net] scan start failed\r\n");
        return;
    }
    for (int i = 0; i < 50 && sys_wifi_scan_get_count() <= 0; ++i) {
        os_msleep(200);
    }
    n = sys_wifi_scan_get_count();
    os_printf(LL_INFO, "[net] scan found %d AP(s)\r\n", n);
    for (int i = 0; i < n && i < 10; ++i) {
        wifi_info_t info;
        memset(&info, 0, sizeof(info));
        if (sys_wifi_scan_get_result((unsigned int)i, &info) == SYS_OK) {
            os_printf(LL_INFO, "[net]  '%s' ch=%d rssi=%d\r\n",
                      (char *)info.ssid, (int)info.channel, (int)info.rssi);
        }
    }
    (void)sys_wifi_scan_stop();
}
#endif /* APP_WIFI_SCAN_DEBUG */

/* 关联一次在本 SDK 上常常要 20–50 秒（扫描 + 认证 + 四次握手 + DHCP）。
 * 之前的实现按固定 30 秒 sta_stop/sta_start，正好把正在进行的四次握手
 * 掐断：日志里 `tx eapol done:-12` → `sta disconnected reason=15`，
 * 随后 AP 侧报 PSK 错误，形成“一直连不上”的假象。 */
/* AP 的"不活跃设备踢线"通常按"多久没收到客户端上行帧"计。手机热点没有开关，
 * 只能由设备自己定期发一个极小上行帧：ARP 请求网关即可（42 字节，开销可忽略）。
 * 15 秒一次，远低于常见 60 秒阈值。 */
#define NET_KEEPALIVE_MS        15000u
#define NET_RESTART_QUIET_MS    90000u  /* 完全断开且无 IP 的等待时间 */
#define NET_RESTART_HARD_MS     180000u /* 任何状态下无 IP 的硬上限 */
#define NET_STOP_WAIT_MS        3000u   /* 等待 STA 停稳 */

static void net_sta_restart(void)
{
    (void)sys_wifi_sta_stop();
    for (int i = 0; i < (int)(NET_STOP_WAIT_MS / 100); ++i) {
        if (sys_wifi_sta_get_status() == STA_STATUS_STOP) {
            break;
        }
        os_msleep(100);
    }
#if APP_WIFI_SCAN_DEBUG
    /* 恢复路径同样必须预扫描：被 AP 踢线后 SDK 的"快速重连"会复用旧状态，
     * 四次握手超时（reason 0x1000）；没有扫描的 network_start 也不会自己发起
     * 扫描（见 docs/10-wifi-boot-sequencing.md 第 1 节）。 */
    net_scan_debug();
#endif
    /* 清掉 NV 里的 PMKSA 缓存：上一次连接成功后直接烧录重启时，
     * 过期缓存会让四次握手一直 MIC 失败（reason=15，看起来像密码错）。 */
    (void)sys_wifi_pmk_cache_clean(WIFI_VIF_STA);
    (void)net_wifi_connect();
}

static void net_task(void *arg)
{
    long long last_restart;
    long long down_since = 0;
    long long last_mqtt_ok = os_time_get();

    (void)arg;
    while (!sys_wifi_is_ready()) {
        os_msleep(100);
    }
    os_printf(LL_INFO, "[net] wifi stack ready\r\n");

    /* standalone 启动代码会创建 NV 自动连接任务；关掉自动连接并停掉它，
     * 由本应用用编译期凭据接管 STA。 */
    (void)sys_wifi_sta_set_auto_connect(false);
    (void)sys_wifi_sta_stop();
    for (int i = 0; i < (int)(NET_STOP_WAIT_MS / 100); ++i) {
        if (sys_wifi_sta_get_status() == STA_STATUS_STOP) {
            break;
        }
        os_msleep(100);
    }

#if APP_WIFI_SCAN_DEBUG
    /* 连接前必须扫描：sta_stop 之后没有预扫描时 supplicant 完全不扫描
     * （实测 100 秒零 wpa 日志）。详见 docs/10-wifi-boot-sequencing.md。 */
    net_scan_debug();
#endif

    /* 烧录后首次启动的握手失败（reason=15）多与 NV 里过期的 PMKSA 有关，
     * 连接前先清一次缓存。 */
    (void)sys_wifi_pmk_cache_clean(WIFI_VIF_STA);

    last_restart = os_time_get();
    (void)net_wifi_connect();

    for (;;) {
        long long now = os_time_get();
        wifi_status_e st = sys_wifi_sta_get_status();
        int may_restart = 0;

        if (s_sta_ip) {
            static long long last_keepalive;
            static unsigned keepalive_count;
            static int sntp_configured;

            down_since = 0;
            /* 每次拿到 IP 后把 SNTP 周期改为 30 秒：单播 UDP 校时请求
             * 既能刷新热点对客户端的活跃计时（广播 ARP 对部分热点无效），
             * 又能持续校准 telemetry 的 sent_at_ms。 */
            if (!sntp_configured) {
                int r = hal_sntp_period_set(30000u, HAL_SNTP_SYNC_NTP);
                sntp_configured = 1;
                os_printf(LL_INFO, "[net] sntp period 30s (ret=%d)\r\n", r);
            }
            /* 心跳：ARP 请求网关，刷新 AP 的客户端活跃计时器 */
            if ((unsigned long long)(now - last_keepalive) >= NET_KEEPALIVE_MS) {
                struct netif *nif = netif_default;

                last_keepalive = now;
                if (nif != NULL && !ip4_addr_isany_val(*netif_ip4_gw(nif))) {
                    (void)etharp_request(nif, netif_ip4_gw(nif));
                    if ((++keepalive_count % 4u) == 0u) {
                        os_printf(LL_INFO, "[net] keepalive %u (arp gw)\r\n",
                                  (unsigned)keepalive_count);
                    }
                }
            }
#if APP_UPLOAD_FORMAL
            /* 静默掉线检测：AP 踢掉设备时 supplicant 可能收不到 deauth，
             * s_sta_ip 一直为 1（实测热点列表已无客户端但 wifi=1）。
             * 以 MQTT 连接作为"真的通"的判据：150 秒都没连上就强制
             * 重新扫描关联，而不是一直等 SDK 自己发现。 */
            if (mqtt_connected()) {
                last_mqtt_ok = now;
            } else if ((unsigned long long)(now - last_mqtt_ok) >= 150000u &&
                       (unsigned long long)(now - last_restart) >= 150000u) {
                os_printf(LL_WARN, "[net] no mqtt for 150s (wifi=%d), "
                                   "restart sta\r\n", (int)s_sta_ip);
                last_restart = now;
                last_mqtt_ok = now;
                net_sta_restart();
                down_since = os_time_get();
            }
#endif
        } else {
            if (down_since == 0) {
                down_since = now;
            }
            /* SDK 自带无限重连策略，正常情况下交给它；只有“彻底停下”
             * （DISCON/STOP，即既不在关联也不在握手）才允许我们介入。 */
            if (st == STA_STATUS_DISCON || st == STA_STATUS_STOP) {
                may_restart = ((unsigned long long)(now - down_since) >=
                                   NET_RESTART_QUIET_MS &&
                               (unsigned long long)(now - last_restart) >=
                                   NET_RESTART_QUIET_MS);
            } else if ((unsigned long long)(now - down_since) >=
                       NET_RESTART_HARD_MS) {
                /* 关联/握手卡死超过 3 分钟才强杀，避免永久无网 */
                may_restart = 1;
            }
            if (may_restart) {
                last_restart = now;
                os_printf(LL_WARN, "[net] no ip for %us (status=%d), restart sta\r\n",
                          (unsigned)((now - down_since) / 1000),
                          (int)st);
                net_sta_restart();
                down_since = os_time_get();
            }
        }
        os_msleep(500);
    }
}

int net_start(void)
{
    int task = os_task_create("app-net", APP_TASK_NET_PRIO, APP_TASK_NET_STACK,
                              net_task, NULL);
    if (task == OS_TASK_CREATE_FAILED) {
        os_printf(LL_ERR, "[net] task create failed\r\n");
        return -1;
    }
    return 0;
}
