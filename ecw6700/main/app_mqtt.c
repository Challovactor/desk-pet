/*
 * desk-recorder/v1 MQTT 通道（MQTT 3.1.1 + TLS 8883）
 *
 * SDK 的 ESP-MQTT 移植自带 TLS/自动重连/QoS outbox；本文件只做：
 *   - 连接与订阅 commands/events；
 *   - 每 30 s 发布 presence（QoS1 retained）和 telemetry（QoS0）；
 *   - 把服务端 audio.export 命令解析成 job 参数交给上传任务。
 *
 * 注意：API.md 没有给出 presence/telemetry/命令的完整 JSON schema；
 * 这里按字段名做最小解析，原始 payload 会打印到日志，便于按服务端
 * HARDWARE_INTEGRATION.md 调整。
 */
#include "app_config.h"
#include "app_mqtt.h"
#include "app_ca_cert.h"
#include "app_json.h"
#include "app_net.h"
#include "app_store.h"
#include "app_sdspi.h"
#include "app_https.h"

#include <stdio.h>
#include <string.h>

#include "oshal.h"
#include "mqtt_client.h"
#include "trng.h"

static trs_mqtt_client_handle_t s_client;
static volatile int s_connected;
static volatile uint32_t s_bytes_sent;
static os_mutex_handle_t s_cmd_lock;
static volatile int s_cmd_pending;
static struct mqtt_command s_cmd;
static char s_boot_id[33];
static char s_conn_id[33];
static volatile long long s_retention_minutes = APP_RETENTION_MINUTES;
static uint32_t s_telemetry_seq;
static volatile long long s_epoch_offset_ms;   /* server Date - os_time_get */
static volatile int s_time_synced;
static volatile int s_reconnect_needed;
static char s_lwt_buf[256];
/* 这两个缓冲原来在 MQTT 事件回调的 6KB 栈上，1KB telemetry 会溢出；
 * 改为 BSS 静态并用 s_pub_lock 串行化（事件回调和 5 秒心跳都会调用）。 */
static char s_presence_buf[384];
static char s_telemetry_buf[1024];
static os_mutex_handle_t s_pub_lock;

static int config_ready(void)
{
    if (app_ca_is_placeholder()) {
        return 0;
    }
    if (strstr(APP_DEVICE_TOKEN, "YOUR_") != NULL ||
        strstr(APP_MQTT_USERNAME, "YOUR_") != NULL ||
        strstr(APP_MQTT_PASSWORD, "YOUR_") != NULL) {
        return 0;
    }
    return 1;
}

/* 生成 32 位小写十六进制 ID（boot_id / connection_id）。
 *
 * 旧实现只用 os_time_get() 做 LCG 种子，而 os_time_get() 是开机计时：
 * 每次启动在同一时刻调用会得到完全相同的 ID。服务器因此看到
 * "同一个 connection_id 配不同 boot_id"（BOOT_CHANGED_WITHIN_SAME_CONNECTION），
 * 拒绝全部心跳，设备被判离线（POST /recordings 也就一直 409）。
 *
 * 现在以硬件 TRNG 为主，再异或时间/盐做兜底。 */
static void make_hex_id(char out[33], uint32_t salt)
{
    uint8_t buf[16];
    uint32_t x = (uint32_t)os_time_get() ^ salt ^ 0x9E3779B9u;

    drv_trng_fill(buf, sizeof(buf));
    for (unsigned i = 0; i < sizeof(buf); ++i) {
        x = x * 1664525u + 1013904223u;
        buf[i] ^= (uint8_t)(x >> 24);
    }
    for (unsigned i = 0; i < sizeof(buf); ++i) {
        snprintf(&out[i * 2u], 3u, "%02x", (unsigned)buf[i]);
    }
    out[32] = '\0';
}

/* SDK 的格式化器不支持 %llu/%lld（日志会打印成 "ld"），JSON 里的
 * 64 位字段自己转十进制字符串。 */
static void u64_to_dec(char *out, uint64_t v)
{
    char tmp[24];
    unsigned n = 0u;

    if (v == 0u) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v != 0u && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    }
    for (unsigned i = 0; i < n; ++i) {
        out[i] = tmp[n - 1u - i];
    }
    out[n] = '\0';
}

/* 服务端校验 sent_at_ms 与收到时刻的偏差（-15s/+5s）。设备没有 RTC，
 * 用一次 HTTPS GET / 的 Date 头获取服务器 UTC 时间，换算成本地 offset。 */
static int mqtt_sync_time(void)
{
    struct app_http_response resp;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (app_https_request("GET", APP_SERVER_HOST, APP_SERVER_HTTPS_PORT,
                              "/", NULL, NULL, 0u, &resp) == 0 &&
            resp.date_ms > 0) {
            s_epoch_offset_ms = resp.date_ms - (long long)os_time_get();
            s_time_synced = 1;
            os_printf(LL_INFO, "[mqtt] time synced: server=%lu s offset=%ld s\r\n",
                      (unsigned long)(resp.date_ms / 1000),
                      (long)(s_epoch_offset_ms / 1000));
            return 0;
        }
        os_msleep(2000);
    }
    os_printf(LL_ERR, "[mqtt] time sync failed; telemetry timestamps invalid\r\n");
    return -1;
}

static void publish_text(const char *topic, const char *payload, int qos, int retain)
{
    if (s_client == NULL || !s_connected || topic == NULL || payload == NULL) {
        return;
    }
    int id = trs_mqtt_client_publish(s_client, topic, payload,
                                     (int)strlen(payload), qos, retain);
    if (id < 0) {
        os_printf(LL_WARN, "[mqtt] publish %s failed: %d\r\n", topic, id);
    }
}

void mqtt_publish_presence(void)
{
    if (s_pub_lock != NULL) {
        os_mutex_lock(s_pub_lock, 1000);
    }
    snprintf(s_presence_buf, sizeof(s_presence_buf),
             "{\"protocol\":\"desk-recorder/v1\",\"device_id\":\"%s\","
             "\"boot_id\":\"%s\",\"connection_id\":\"%s\","
             "\"firmware_version\":\"%s\","
             "\"capabilities\":[\"audio.export\"],"
             "\"online\":true}",
             APP_DEVICE_ID, s_boot_id, s_conn_id, APP_FIRMWARE_VERSION);
    publish_text(APP_MQTT_TOPIC_PRESENCE, s_presence_buf, 1, 1);
    if (s_pub_lock != NULL) {
        os_mutex_unlock(s_pub_lock);
    }
}

/* telemetry 字段按服务端 xiao-ec63004aec24 的实机 payload 对齐：
 * protocol / firmware_version / capabilities / sd_* / audio{...} / seq。
 * sent_at_ms 目前没有 UTC 校时，先发 0；服务端在线判定主要用接收时间。 */
void mqtt_publish_telemetry(uint32_t bytes_sent)
{
    char total_s[24], sd_total_s[24], sd_used_s[24], sent_s[24];
    uint64_t total = store_total_bytes();
    uint32_t total_sec = (uint32_t)(total / APP_PCM_BYTES_PER_SEC);
    uint32_t ret_min = (uint32_t)mqtt_retention_minutes();
    uint32_t avail = total_sec;
    uint32_t wav_bytes;
    int sd_ready = app_sd_is_ready();
    uint64_t sd_total = sd_ready ? (uint64_t)app_sd_sector_count() * 512u : 0u;
    uint64_t used = sd_ready ? total : 0u;
    uint64_t sent_at_ms = s_time_synced
                              ? (uint64_t)os_time_get() + (uint64_t)s_epoch_offset_ms
                              : 0u;

    if (ret_min < 1u) {
        ret_min = 1u;
    }
    if (ret_min > 60u) {
        ret_min = 60u;
    }
    if (avail > ret_min * 60u) {
        avail = ret_min * 60u;
    }
    wav_bytes = avail * APP_SOUND_SAMPLE_RATE * 2u + 44u;

    if (bytes_sent > 0u) {
        s_bytes_sent += bytes_sent;
    }
    ++s_telemetry_seq;

    u64_to_dec(total_s, used);
    u64_to_dec(sd_total_s, sd_total);
    u64_to_dec(sd_used_s, used);
    u64_to_dec(sent_s, sent_at_ms);

    if (s_pub_lock != NULL) {
        os_mutex_lock(s_pub_lock, 1000);
    }
    snprintf(s_telemetry_buf, sizeof(s_telemetry_buf),
             "{\"protocol\":\"desk-recorder/v1\",\"device_id\":\"%s\","
             "\"boot_id\":\"%s\",\"connection_id\":\"%s\","
             "\"firmware_version\":\"%s\","
             "\"capabilities\":[\"audio.export\"],"
             "\"busy\":false,\"bytes_sent\":%lu,\"total_bytes\":%s,"
             "\"sd_ready\":%s,\"sd_total_bytes\":%s,\"sd_used_bytes\":%s,"
             "\"seq\":%lu,\"sent_at_ms\":%s,"
             "\"audio\":{\"available_seconds\":%lu,\"bits_per_sample\":16,"
             "\"capture_lag_ms\":0,\"channels\":1,\"dma_overflows\":0,"
             "\"download_active\":false,\"download_url\":\"/api/audio.wav\","
             "\"error\":\"\",\"export_active\":false,"
             "\"last_audio_uptime_ms\":%lu,\"max_minutes\":60,\"min_minutes\":1,"
             "\"peak_percent\":0,\"queue_blocks\":0,\"retention_minutes\":%lu,"
             "\"sample_rate\":%u,\"settings_persistent\":false,"
             "\"state\":\"recording\",\"wav_bytes\":%lu}}",
             APP_DEVICE_ID, s_boot_id, s_conn_id, APP_FIRMWARE_VERSION,
             (unsigned long)s_bytes_sent, total_s,
             sd_ready ? "true" : "false",
             sd_total_s, sd_used_s,
             (unsigned long)s_telemetry_seq, sent_s,
             (unsigned long)avail,
             (unsigned long)os_time_get(), (unsigned long)ret_min,
             (unsigned)APP_SOUND_SAMPLE_RATE, (unsigned long)wav_bytes);
    publish_text(APP_MQTT_TOPIC_TELEMETRY, s_telemetry_buf, 0, 0);
    if (s_pub_lock != NULL) {
        os_mutex_unlock(s_pub_lock);
    }
}

static void handle_command_payload(const char *json)
{
    struct mqtt_command cmd;
    long long minutes = 0;

    memset(&cmd, 0, sizeof(cmd));
    if (app_json_get_string(json, "job_id", cmd.job_id, sizeof(cmd.job_id)) != 0) {
        os_printf(LL_WARN, "[mqtt] command without job_id ignored\r\n");
        return;
    }
    /* 丢弃 retained/旧命令：命令主题可能保留上一条命令，重连/重启后先收到
     * 的是上一次启动的 job（boot_id 不同）。实测它会让上传去处理一个已被
     * 服务器清理的任务（410 GONE）。 */
    {
        char boot[33];

        boot[0] = '\0';
        if (app_json_get_string(json, "boot_id", boot, sizeof(boot)) == 0 &&
            boot[0] != '\0' && strcmp(boot, s_boot_id) != 0) {
            os_printf(LL_WARN, "[mqtt] stale command ignored (boot=%.8s)\r\n", boot);
            return;
        }
    }
    (void)app_json_get_string(json, "recording_id", cmd.recording_id,
                              sizeof(cmd.recording_id));
    (void)app_json_get_string(json, "type", cmd.type, sizeof(cmd.type));
    if (app_json_get_int(json, "minutes", &minutes) == 0) {
        cmd.minutes = minutes;
    }
    (void)app_json_get_int(json, "execute_before_ms", &cmd.execute_before_ms);

    /* audio.configure：只更新保留时长，不进入保存任务队列。
     * 注意：当前没有给这个配置任务回 /result，服务端可能显示未确认。 */
    if (strstr(cmd.type, "audio.configure") != NULL) {
        if (cmd.minutes >= 1 && cmd.minutes <= 60) {
            s_retention_minutes = cmd.minutes;
        }
        os_printf(LL_INFO, "[mqtt] audio.configure: minutes=%lld (retention=%lld)\r\n",
                  cmd.minutes, s_retention_minutes);
        return;
    }

    os_printf(LL_INFO, "[mqtt] command job=%s rec=%s type=%s minutes=%lld\r\n",
              cmd.job_id, cmd.recording_id, cmd.type, cmd.minutes);

    if (s_cmd_lock != NULL && os_mutex_lock(s_cmd_lock, 1000) == 0) {
        s_cmd = cmd;
        s_cmd_pending = 1;
        os_mutex_unlock(s_cmd_lock);
    }
}

static int mqtt_event_handler(trs_mqtt_event_handle_t event)
{
    if (event == NULL) {
        return 0;
    }
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = 1;
        /* 服务端要求 seq 在每个连接内从 0 重新计数；connection_id 与
         * CONNECT 前设置的遗嘱保持一致，所以不在连接回调里重新生成。 */
        s_telemetry_seq = 0;
        os_printf(LL_INFO, "[mqtt] connected, subscribing %s\r\n",
                  APP_MQTT_TOPIC_COMMANDS);
        (void)trs_mqtt_client_subscribe(s_client, APP_MQTT_TOPIC_COMMANDS, 1);
        (void)trs_mqtt_client_subscribe(s_client, APP_MQTT_TOPIC_COMMANDS_ALT, 1);
        mqtt_publish_presence();
        mqtt_publish_telemetry(0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = 0;
        s_reconnect_needed = 1;
        os_printf(LL_WARN, "[mqtt] disconnected\r\n");
        break;

    case MQTT_EVENT_DATA: {
        char topic[128];
        char data[320];
        int tlen = event->topic_len < (int)sizeof(topic) - 1
                       ? event->topic_len : (int)sizeof(topic) - 1;
        int dlen = event->data_len < (int)sizeof(data) - 1
                       ? event->data_len : (int)sizeof(data) - 1;

        if (tlen < 0) tlen = 0;
        if (dlen < 0) dlen = 0;
        memcpy(topic, event->topic, (size_t)tlen);
        topic[tlen] = '\0';
        memcpy(data, event->data, (size_t)dlen);
        data[dlen] = '\0';
        os_printf(LL_INFO, "[mqtt] rx %s: %s\r\n", topic, data);
        handle_command_payload(data);
        break;
    }

    case MQTT_EVENT_ERROR:
        s_reconnect_needed = 1;
        os_printf(LL_ERR, "[mqtt] transport error\r\n");
        break;

    default:
        break;
    }
    return 0;
}

static void mqtt_task(void *arg)
{
    (void)arg;

    if (net_wait_connected(60000) != 0) {
        os_printf(LL_WARN, "[mqtt] no ip after 60s at task start\r\n");
    } else {
        os_printf(LL_INFO, "[mqtt] net up, task starting\r\n");
    }
    if (!config_ready()) {
        os_printf(LL_ERR, "[mqtt] credentials/CA are placeholders; "
                          "edit app_config.h and app_ca_cert.c\r\n");
        for (;;) {
            os_msleep(5000);
        }
    }

    make_hex_id(s_boot_id, 0x12345678u);

    /* 服务端要求每次 MQTT 重连都换 connection_id、重设遗嘱、seq 归零。
     * 因此关闭 SDK 自动重连，由本任务销毁/重建客户端。
     *
     * 注意：stop() 只置 run=false，客户端任务退出时才 destroy/free
     * （TLS 上下文、outbox、队列）。连续快速重建会把堆耗尽（实测
     * "Memory exhausted" -> client init failed），所以停止后必须等任务
     * 退出，并在连续失败时指数退避。 */
    unsigned fails = 0;
    for (;;) {
        /* 没有 IP 时不要创建 mbedTLS/MQTT 客户端：SDK 传输层会在无路由时
         * 反复 connect（errno=113），既浪费内存，也会和 Wi-Fi 关联抢资源。
         * 关联成功后再建客户端；掉线时先销毁。 */
        if (net_wait_connected(30000) != 0) {
            if (s_client != NULL) {
                (void)trs_mqtt_client_stop(s_client);
                s_client = NULL;
                s_connected = 0;
                s_reconnect_needed = 0;
                os_msleep(3000);   /* 等旧客户端任务释放资源 */
            }
            continue;
        }

        if (s_connected) {
            fails = 0u;
        }

        if (s_client == NULL || s_reconnect_needed) {
            os_printf(LL_INFO, "[mqtt] creating client (sync time first)\r\n");
            if (s_client != NULL) {
                /* stop() 只置 run=false；客户端任务退出时自行 destroy/free。 */
                (void)trs_mqtt_client_stop(s_client);
                s_client = NULL;
                os_msleep(3000);
            }
            s_reconnect_needed = 0;
            s_connected = 0;

            if (fails > 0u) {
                unsigned wait_s = 5u * (fails > 6u ? 6u : fails);

                os_printf(LL_INFO, "[mqtt] backoff %us (fails=%u heap=%u)\r\n",
                          wait_s, fails,
                          (unsigned)os_get_free_heap_size());
                os_msleep(wait_s * 1000u);
            }

            (void)mqtt_sync_time();
            make_hex_id(s_conn_id, (uint32_t)os_time_get());
            os_printf(LL_INFO, "[mqtt] session boot=%s conn=%s\r\n",
                      s_boot_id, s_conn_id);
            snprintf(s_lwt_buf, sizeof(s_lwt_buf),
                     "{\"protocol\":\"desk-recorder/v1\",\"device_id\":\"%s\","
                     "\"boot_id\":\"%s\",\"connection_id\":\"%s\","
                     "\"online\":false}",
                     APP_DEVICE_ID, s_boot_id, s_conn_id);

            trs_mqtt_client_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.event_handle = mqtt_event_handler;
            cfg.uri = "mqtts://" APP_MQTT_HOST;
            cfg.port = APP_MQTT_PORT;
            cfg.client_id = APP_DEVICE_ID;
            cfg.username = APP_MQTT_USERNAME;
            cfg.password = APP_MQTT_PASSWORD;
            cfg.cert_pem = app_ca_pem();
            /* PEM 必须 NUL 结尾；len=0 表示由客户端 strlen（显式长度只支持 DER）。 */
            cfg.cert_pem_len = 0;
            cfg.keepalive = 60;
            cfg.reconnect_timeout_ms = 5000;
            cfg.disable_auto_reconnect = true;
            cfg.task_stack = 8192;
            cfg.buffer_size = 1536;
            cfg.lwt_topic = APP_MQTT_TOPIC_PRESENCE;
            cfg.lwt_msg = s_lwt_buf;
            cfg.lwt_msg_len = (int)strlen(s_lwt_buf);
            cfg.lwt_qos = 1;
            cfg.lwt_retain = 1;

            s_client = trs_mqtt_client_init(&cfg);
            if (s_client == NULL) {
                os_printf(LL_ERR, "[mqtt] client init failed (heap=%u)\r\n",
                          (unsigned)os_get_free_heap_size());
                ++fails;
                s_reconnect_needed = 1;
                os_msleep(5000);
                continue;
            }
            if (trs_mqtt_client_start(s_client) != 0) {
                os_printf(LL_ERR, "[mqtt] client start failed\r\n");
                ++fails;
                s_reconnect_needed = 1;
            }
        }

        /* 服务端要求每 5 秒一次有效 telemetry；presence 只在 CONNECT 时发。 */
        if (s_connected) {
            mqtt_publish_telemetry(0);
        }
        os_msleep(5000);
    }
}

void app_mqtt_start(void)
{
    if (s_cmd_lock == NULL) {
        s_cmd_lock = os_mutex_create();
    }
    if (s_pub_lock == NULL) {
        s_pub_lock = os_mutex_create();
    }
    int task = os_task_create("app-mqtt", APP_TASK_NET_PRIO, APP_TASK_NET_STACK,
                              mqtt_task, NULL);
    if (task == OS_TASK_CREATE_FAILED) {
        os_printf(LL_ERR, "[mqtt] task create failed\r\n");
    }
}

int mqtt_connected(void)
{
    return s_connected;
}

int mqtt_wait_connected(uint32_t timeout_ms)
{
    long long start = os_time_get();

    while (!s_connected) {
        if ((unsigned long long)(os_time_get() - start) >= timeout_ms) {
            return -1;
        }
        os_msleep(100);
    }
    return 0;
}

int mqtt_take_command(struct mqtt_command *out)
{
    int got = 0;

    if (out == NULL || s_cmd_lock == NULL) {
        return 0;
    }
    if (os_mutex_lock(s_cmd_lock, 1000) != 0) {
        return 0;
    }
    if (s_cmd_pending) {
        *out = s_cmd;
        s_cmd_pending = 0;
        got = 1;
    }
    os_mutex_unlock(s_cmd_lock);
    return got;
}

long long mqtt_retention_minutes(void)
{
    return s_retention_minutes;
}
