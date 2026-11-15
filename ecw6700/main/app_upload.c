/*
 * desk-recorder/v1 设备主动保存（正式上传）
 *
 * 依赖：Wi-Fi（app_net）、MQTT/TLS（app_mqtt）、HTTPS/TLS（app_https）、
 * SHA256（app_sha256）、32k->16k 降采样（app_resample）。
 *
 * 注意：server/API.md 只给出了四步 HTTP 接口的语义，没有给出
 * /output 元数据和 /result 的字段名。下面的 JSON 构造集中在
 * build_output_json() / build_result_json()，服务端 HARDWARE_INTEGRATION.md
 * 提供后只需改这两个函数。
 */
#include "app_config.h"
#include "app_audio.h"
#include "app_upload.h"
#include "app_net.h"
#include "app_mqtt.h"
#include "app_https.h"
#include "app_json.h"
#include "app_sha256.h"
#include "app_store.h"
#include "app_resample.h"

#include <stdio.h>
#include <string.h>

#include "oshal.h"

#define UPLOAD_WAV_RATE       16000u
#define UPLOAD_WAV_CHANNELS   1u
#define UPLOAD_WAV_BITS       16u
#define UPLOAD_HASH_CHUNK     4096u      /* 32k PCM 字节 */
#define UPLOAD_SRC_BLOCK      512u       /* 每次处理的 32k 样本数 */
#define UPLOAD_CMD_WAIT_MS    50000u
#define UPLOAD_HTTP_RETRY     2

struct wav_stream {
    uint8_t header[44];
    uint32_t header_pos;
    uint32_t cursor;
    uint32_t src_left;       /* 剩余 8k PCM 字节 */
    struct resampler_8to16 rs;
    int16_t src_buf[UPLOAD_SRC_BLOCK];
    int16_t out_buf[UPLOAD_SRC_BLOCK * 2];   /* 8k -> 16k，输出 2 倍 */
    uint32_t out_pos;        /* 已输出的 16k PCM 字节 */
    uint32_t out_len;
};

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

static void make_wav_header(uint8_t h[44], uint32_t data_bytes)
{
    memcpy(h + 0, "RIFF", 4);
    put_u32le(h + 4, 36u + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32le(h + 16, 16u);
    put_u16le(h + 20, 1u);                            /* PCM */
    put_u16le(h + 22, UPLOAD_WAV_CHANNELS);
    put_u32le(h + 24, UPLOAD_WAV_RATE);
    put_u32le(h + 28, UPLOAD_WAV_RATE * UPLOAD_WAV_CHANNELS * (UPLOAD_WAV_BITS / 8u));
    put_u16le(h + 32, (uint16_t)(UPLOAD_WAV_CHANNELS * (UPLOAD_WAV_BITS / 8u)));
    put_u16le(h + 34, UPLOAD_WAV_BITS);
    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, data_bytes);
}

static int http_ok(const struct app_http_response *r)
{
    return r != NULL && r->status >= 200 && r->status < 300;
}

static void build_headers(char *buf, size_t cap, const char *extra)
{
    snprintf(buf, cap,
             "Authorization: Bearer %s\r\n"
             "X-Device-Id: %s\r\n"
             "%s",
             APP_DEVICE_TOKEN, APP_DEVICE_ID, extra != NULL ? extra : "");
}

static void log_http(const char *what, const struct app_http_response *r)
{
    if (r == NULL) {
        os_printf(LL_ERR, "[up] %s: no response\r\n", what);
        return;
    }
    os_printf(r->status >= 200 && r->status < 300 ? LL_INFO : LL_ERR,
              "[up] %s: http %d%s%s\r\n", what, r->status,
              r->body_len > 0 ? " body=" : "",
              r->body_len > 0 ? r->body : "");
}

/* ------------------------------------------------------------ 任务申请 */

static int post_recordings(const struct clip_job *job, char *job_id, size_t job_id_len,
                           char *rec_id, size_t rec_id_len, char *state,
                           size_t state_len)
{
    char headers[640];
    char extra[192];
    struct app_http_response resp;

    snprintf(extra, sizeof(extra),
             "Content-Type: application/json\r\n"
             "Idempotency-Key: %s\r\n", job->idem);
    build_headers(headers, sizeof(headers), extra);

    int busy_waits = 0;

    for (int attempt = 0; attempt <= UPLOAD_HTTP_RETRY; ) {
        if (app_https_request("POST", APP_SERVER_HOST, APP_SERVER_HTTPS_PORT,
                              "/device/v1/recordings", headers,
                              "{}", 2u, &resp) == 0 && http_ok(&resp)) {
            job_id[0] = rec_id[0] = state[0] = '\0';
            (void)app_json_get_string(resp.body, "job_id", job_id, job_id_len);
            (void)app_json_get_string(resp.body, "recording_id", rec_id, rec_id_len);
            (void)app_json_get_string(resp.body, "state", state, state_len);
            log_http("POST recordings", &resp);
            if (job_id[0] != '\0') {
                return 0;
            }
            /* 202 的 body 可能被 chunked/网关截断，job_id 从随后的
             * MQTT audio.export 命令里取（upload_formal_run 处理）。 */
            os_printf(LL_WARN, "[up] 202 without job_id; wait MQTT command\r\n");
            return 1;
        }
        log_http("POST recordings", &resp);
        /* 409 BUSY：设备上还有未完成的任务。用同一个 Idempotency-Key
         * 退避重试（服务器单活动任务限制，通常几秒到几十秒后释放）；
         * 超过 5 次（约 50 秒，仍在 55 秒执行期限附近）才放弃。 */
        if (resp.status == 409 && busy_waits < 5) {
            ++busy_waits;
            os_printf(LL_WARN, "[up] device BUSY, retry in 10s (%d/5)\r\n",
                      busy_waits);
            os_msleep(10000);
            continue;
        }
        ++attempt;
        os_msleep(2000);
    }
    return -1;
}

/* GET /jobs/{id} 同时充当预检查和命令读取。 */
static int get_job(const char *job_id, char *state, size_t state_len,
                   long long *minutes, char *rec_id, size_t rec_id_len,
                   int *terminal)
{
    char headers[640];
    char path[160];
    struct app_http_response resp;
    long long m = 0;

    build_headers(headers, sizeof(headers), NULL);
    snprintf(path, sizeof(path), "/device/v1/jobs/%s", job_id);
    if (app_https_request("GET", APP_SERVER_HOST, APP_SERVER_HTTPS_PORT,
                          path, headers, NULL, 0u, &resp) != 0) {
        return -1;
    }
    log_http("GET job", &resp);
    if (!http_ok(&resp)) {
        /* 410 GONE / 404：任务已被取消或清理，继续轮询没有意义 */
        if (terminal != NULL && (resp.status == 410 || resp.status == 404)) {
            *terminal = 1;
        }
        return -1;
    }
    (void)app_json_get_string(resp.body, "state", state, state_len);
    (void)app_json_get_string(resp.body, "recording_id", rec_id, rec_id_len);
    if (app_json_get_int(resp.body, "minutes", &m) == 0) {
        *minutes = m;
    }
    return 0;
}

/* ------------------------------------------------------------ WAV 生成 */

static int prepare_clip(uint32_t minutes, uint8_t header[44], uint32_t *offset,
                        uint32_t *src_bytes, uint32_t *wav_total,
                        char sha_hex[65])
{
    uint32_t want = minutes * 60u * APP_PCM_BYTES_PER_SEC;

    /* 限制 SD 读时长：连续读超过约 15 秒会把 Wi-Fi RF 拖死（docs/08 8.10） */
    if (want > APP_UPLOAD_MAX_SECONDS * APP_PCM_BYTES_PER_SEC) {
        want = APP_UPLOAD_MAX_SECONDS * APP_PCM_BYTES_PER_SEC;
    }
    uint32_t bytes = 0u;
    uint32_t off = 0u;
    uint32_t src_samples;
    uint32_t out_samples;
    uint32_t wav_data;
    uint32_t cursor;
    uint32_t left;
    struct resampler_8to16 rs;
    int16_t src[UPLOAD_SRC_BLOCK];
    int16_t dst[UPLOAD_SRC_BLOCK * 2];
    uint8_t digest[32];

    if (store_clip_begin(want, &bytes, &off) != 0 || bytes < 4u) {
        os_printf(LL_WARN, "[up] not enough audio buffered\r\n");
        return -1;
    }
    src_samples = bytes / 2u;
    out_samples = src_samples * 2u;        /* 8k -> 16k，2× 线性插值 */
    wav_data = out_samples * (UPLOAD_WAV_BITS / 8u);
    *wav_total = 44u + wav_data;
    *offset = off;
    *src_bytes = src_samples * 2u;         /* 参与升采样的全部 8k 字节 */
    make_wav_header(header, wav_data);

    app_sha256_begin();
    app_sha256_update(header, 44u);
    resampler_8to16_init(&rs);
    cursor = off;
    left = *src_bytes;
    while (left >= 2u) {
        uint32_t samples = left / 2u;
        uint32_t out;

        if (samples > UPLOAD_SRC_BLOCK) {
            samples = UPLOAD_SRC_BLOCK;
        }
        if (store_clip_read(&cursor, src, samples * 2u) != 0) {
            os_printf(LL_ERR, "[up] SD read failed during hash\r\n");
            return -1;
        }
        out = resampler_8to16_run(&rs, src, samples, dst, UPLOAD_SRC_BLOCK * 2);
        /* dst 是 int16 小端，和 WAV 输出字节序一致，整块喂给 SHA。 */
        app_sha256_update(dst, out * 2u);
        left -= samples * 2u;
        /* SD 位翻转读写连续几十秒会让 RF 失锁（实测上传期间掉线）：
         * 每块之间留 5ms 间隙，让 Wi-Fi 收包/信标处理有喘息时间。 */
        os_msleep(5);
    }
    app_sha256_end(digest);
    app_sha256_hex(digest, sha_hex);
    os_printf(LL_INFO, "[up] clip: %u min, %u src bytes, wav %u bytes, sha=%s\r\n",
              (unsigned)minutes, (unsigned)*src_bytes, (unsigned)*wav_total, sha_hex);
    return 0;
}

static int stream_read(void *ctx, void *buf, unsigned int len)
{
    struct wav_stream *s = (struct wav_stream *)ctx;
    uint8_t *out = (uint8_t *)buf;
    uint32_t copied = 0u;

    while (copied < len) {
        if (s->header_pos < sizeof(s->header)) {
            uint32_t n = (uint32_t)sizeof(s->header) - s->header_pos;
            if (n > len - copied) {
                n = len - copied;
            }
            memcpy(out + copied, &s->header[s->header_pos], n);
            s->header_pos += n;
            copied += n;
            continue;
        }
        if (s->out_pos >= s->out_len) {
            uint32_t samples;
            uint32_t out_n;

            if (s->src_left < 2u) {
                break;
            }
            samples = s->src_left / 2u;
            if (samples > UPLOAD_SRC_BLOCK) {
                samples = UPLOAD_SRC_BLOCK;
            }
            if (store_clip_read(&s->cursor, s->src_buf, samples * 2u) != 0) {
                return -1;
            }
            s->src_left -= samples * 2u;
            out_n = resampler_8to16_run(&s->rs, s->src_buf, samples,
                                         s->out_buf, UPLOAD_SRC_BLOCK * 2);
            s->out_len = out_n * 2u;
            s->out_pos = 0u;
            if (s->out_len == 0u) {
                continue;
            }
        }
        {
            uint32_t n = s->out_len - s->out_pos;
            if (n > len - copied) {
                n = len - copied;
            }
            memcpy(out + copied,
                   (const uint8_t *)s->out_buf + s->out_pos, n);
            s->out_pos += n;
            copied += n;
        }
    }
    return (int)copied;
}

static void stream_init(struct wav_stream *s, const uint8_t header[44],
                        uint32_t offset, uint32_t src_bytes)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->header, header, sizeof(s->header));
    s->cursor = offset;
    s->src_left = src_bytes;
    resampler_8to16_init(&s->rs);
}

/* ------------------------------------------------------------ 元数据/结果 */

static void build_output_json(char *buf, size_t cap, const char *rec_id,
                              uint32_t wav_total, uint32_t duration_ms,
                              const char *sha_hex)
{
    /* 字段名按 API.md 语义的合理猜测；服务端 schema 确认后在此调整。 */
    snprintf(buf, cap,
             "{\"recording_id\":\"%s\",\"format\":\"wav\","
             "\"content_type\":\"audio/wav\","
             "\"sample_rate\":%u,\"channels\":%u,\"bits_per_sample\":%u,"
             "\"size_bytes\":%u,\"duration_ms\":%u,\"sha256\":\"%s\"}",
             rec_id, (unsigned)UPLOAD_WAV_RATE, (unsigned)UPLOAD_WAV_CHANNELS,
             (unsigned)UPLOAD_WAV_BITS, (unsigned)wav_total,
             (unsigned)duration_ms, sha_hex);
}

static void build_result_json(char *buf, size_t cap, int ok,
                              const char *rec_id, uint32_t wav_total,
                              const char *sha_hex, const char *reason)
{
    if (ok) {
        snprintf(buf, cap,
                 "{\"state\":\"succeeded\",\"recording_id\":\"%s\","
                 "\"size_bytes\":%u,\"sha256\":\"%s\"}",
                 rec_id, (unsigned)wav_total, sha_hex);
    } else {
        snprintf(buf, cap,
                 "{\"state\":\"failed\",\"recording_id\":\"%s\","
                 "\"error\":{\"code\":\"DEVICE_UPLOAD_FAILED\","
                 "\"message\":\"%s\"}}",
                 rec_id, reason != NULL ? reason : "upload failed");
    }
}

static int post_output(const char *job_id, const char *json)
{
    char headers[640];
    char path[160];
    struct app_http_response resp;
    char extra[64];

    snprintf(extra, sizeof(extra), "Content-Type: application/json\r\n");
    build_headers(headers, sizeof(headers), extra);
    snprintf(path, sizeof(path), "/device/v1/jobs/%s/output", job_id);
    /* 重复提交必须完全一致（同一个 json 缓冲），因此重试只重发。 */
    for (int attempt = 0; attempt <= UPLOAD_HTTP_RETRY; ++attempt) {
        if (app_https_request("POST", APP_SERVER_HOST, APP_SERVER_HTTPS_PORT,
                              path, headers, json, (unsigned)strlen(json),
                              &resp) == 0) {
            log_http("POST output", &resp);
            if (http_ok(&resp)) {
                return 0;
            }
        } else {
            log_http("POST output", &resp);
        }
        os_msleep(2000);
    }
    return -1;
}

static int put_blob(const char *job_id, struct wav_stream *stream, uint32_t total,
                    const char *sha_hex)
{
    char headers[640];
    char path[160];
    struct app_http_response resp;
    char extra[64];
    char got_sha[65];

    snprintf(extra, sizeof(extra), "Content-Type: audio/wav\r\n");
    build_headers(headers, sizeof(headers), extra);
    snprintf(path, sizeof(path), "/device/v1/jobs/%s/output/blob", job_id);
    if (app_https_request_stream("PUT", APP_SERVER_HOST, APP_SERVER_HTTPS_PORT,
                                 path, headers, total, stream_read, stream,
                                 &resp) != 0) {
        return -1;
    }
    log_http("PUT blob", &resp);
    if (!http_ok(&resp)) {
        return -1;
    }
    /* 服务端回执若带 sha256，必须与设备计算结果一致。 */
    if (app_json_get_string(resp.body, "sha256", got_sha, sizeof(got_sha)) == 0 &&
        strcmp(got_sha, sha_hex) != 0) {
        os_printf(LL_ERR, "[up] server sha mismatch: %s vs %s\r\n",
                  got_sha, sha_hex);
        return -1;
    }
    return 0;
}

static int post_result(const char *job_id, const char *json)
{
    char headers[640];
    char path[160];
    struct app_http_response resp;
    char extra[64];

    snprintf(extra, sizeof(extra), "Content-Type: application/json\r\n");
    build_headers(headers, sizeof(headers), extra);
    snprintf(path, sizeof(path), "/device/v1/jobs/%s/result", job_id);
    for (int attempt = 0; attempt <= UPLOAD_HTTP_RETRY; ++attempt) {
        if (app_https_request("POST", APP_SERVER_HOST, APP_SERVER_HTTPS_PORT,
                              path, headers, json, (unsigned)strlen(json),
                              &resp) == 0) {
            log_http("POST result", &resp);
            if (http_ok(&resp)) {
                return 0;
            }
        } else {
            log_http("POST result", &resp);
        }
        os_msleep(2000);
    }
    return -1;
}

/* ---------------------------------------------------------------- 主流程 */

static int upload_formal_run_inner(const struct clip_job *job)
{
    char job_id[64] = {0};
    char rec_id[64] = {0};
    char state[32] = {0};
    char header[44];
    char sha_hex[65];
    char json[384];
    char reason[96] = "unknown";
    uint32_t offset = 0u;
    uint32_t src_bytes = 0u;
    uint32_t wav_total = 0u;
    uint32_t duration_ms;
    long long minutes = 0;
    long long cmd_minutes = 0;
    int have_job = 0;
    struct mqtt_command cmd;
    struct wav_stream stream;
    int tries;

    if (job == NULL) {
        return -1;
    }
    if (strstr(APP_DEVICE_TOKEN, "YOUR_") != NULL) {
        os_printf(LL_ERR, "[up] APP_DEVICE_TOKEN is a placeholder\r\n");
        return -1;
    }

    /* 1. 等 Wi-Fi + MQTT presence。 */
    for (tries = 0; tries < 3 && net_wait_connected(30000) != 0; ++tries) {
    }
    if (tries >= 3) {
        os_printf(LL_WARN, "[up] clip %u: wifi down, skipped\r\n",
                  (unsigned)job->clip_id);
        return -1;
    }
    if (mqtt_wait_connected(30000) != 0) {
        os_printf(LL_ERR, "[up] MQTT not connected; server will reject the job\r\n");
        return -1;
    }

    /* 2. 申请保存。202 可能不带 body/job_id，此时用随后 MQTT 的
     *    audio.export 命令补齐 job_id/recording_id。 */
    {
        int pr = post_recordings(job, job_id, sizeof(job_id), rec_id, sizeof(rec_id),
                                 state, sizeof(state));
        if (pr < 0) {
            return -1;
        }
        if (pr == 1) {
            int got = 0;

            for (int i = 0; i < 50; ++i) {
                if (mqtt_take_command(&cmd) && cmd.job_id[0] != '\0') {
                    got = 1;
                    break;
                }
                os_msleep(200);
            }
            if (!got) {
                os_printf(LL_ERR, "[up] no MQTT command after 202\r\n");
                return -1;
            }
            snprintf(job_id, sizeof(job_id), "%s", cmd.job_id);
            if (cmd.recording_id[0] != '\0') {
                snprintf(rec_id, sizeof(rec_id), "%s", cmd.recording_id);
            }
            cmd_minutes = cmd.minutes;
            state[0] = '\0';
            have_job = 1;   /* 命令已经给了 job_id，直接开始准备音频 */
            os_printf(LL_INFO, "[up] command job=%s rec=%s\r\n", job_id, rec_id);
        } else {
            /* 202 已经给了 job_id：直接进入快照/上传，跳过等待 MQTT 命令和
             * GET 轮询。服务器 v0.1.1 有"任务创建后 10 秒无音频数据即取消"
             * 的规则，慢吞吞地轮询会把自己轮死。 */
            have_job = 1;
            os_printf(LL_INFO, "[up] job=%s rec=%s state=%s\r\n", job_id, rec_id, state);
            if (strcmp(state, "succeeded") == 0) {
                os_printf(LL_INFO, "[up] job already succeeded, nothing to do\r\n");
                mqtt_publish_telemetry(0);
                return 0;
            }
            if (strcmp(state, "failed") == 0 || strcmp(state, "expired") == 0) {
                os_printf(LL_WARN, "[up] job %s is %s\r\n", job_id, state);
                return -1;
            }
        }
    }

    /* 3. 等 MQTT 命令 / 轮询 GET；GET 同时是任务预检查。 */
    memset(&cmd, 0, sizeof(cmd));
    /* have_job=1（202 带 job_id）时直接跳过预检查，尽快 POST output，
     * 避免触发服务器"10 秒无音频数据即取消"的清理规则。 */
    for (uint32_t waited = have_job ? UPLOAD_CMD_WAIT_MS : 0u;
         waited < UPLOAD_CMD_WAIT_MS; waited += 2000u) {
        if (mqtt_take_command(&cmd) && cmd.job_id[0] != '\0' &&
            strcmp(cmd.job_id, job_id) == 0) {
            cmd_minutes = cmd.minutes;
            if (cmd.recording_id[0] != '\0') {
                snprintf(rec_id, sizeof(rec_id), "%s", cmd.recording_id);
            }
            break;
        }
        {
            int gone = 0;

        if (get_job(job_id, state, sizeof(state), &minutes,
                    rec_id, sizeof(rec_id), &gone) == 0) {
            if (minutes > 0) {
                cmd_minutes = minutes;
            }
            if (strcmp(state, "succeeded") == 0) {
                os_printf(LL_INFO, "[up] job already succeeded\r\n");
                mqtt_publish_telemetry(0);
                return 0;
            }
            if (strcmp(state, "failed") == 0 || strcmp(state, "expired") == 0) {
                os_printf(LL_WARN, "[up] job %s is %s\r\n", job_id, state);
                return -1;
            }
            if (strcmp(state, "queued") == 0 || strcmp(state, "running") == 0) {
                break;
            }
        } else if (gone) {
            /* 任务已被服务器取消/清理（410/404）：立即放弃，别空轮询 */
            os_printf(LL_WARN, "[up] job %s is gone; drop this clip\r\n", job_id);
            return -1;
        }
        }
        os_msleep(2000);
    }
    if (cmd_minutes <= 0) {
        cmd_minutes = mqtt_retention_minutes();
    }
    if (cmd_minutes < 1) {
        cmd_minutes = 1;
    }
    if (cmd_minutes > 60) {
        cmd_minutes = 60;
    }

    /* 4. 生成 16 kHz WAV 并计算 SHA256（第一遍）。 */
    if (prepare_clip((uint32_t)cmd_minutes, (uint8_t *)header, &offset,
                     &src_bytes, &wav_total, sha_hex) != 0) {
        snprintf(reason, sizeof(reason), "snapshot/hash failed");
        goto fail;
    }
    duration_ms = (src_bytes / 2u) * 1000u / APP_PCM_SAMPLE_RATE;

    /* 5. 提交 WAV 元数据。 */
    build_output_json(json, sizeof(json), rec_id, wav_total, duration_ms, sha_hex);
    if (post_output(job_id, json) != 0) {
        snprintf(reason, sizeof(reason), "output metadata rejected");
        goto fail;
    }

    /* 6. 流式上传 WAV（第二遍，必须与第一遍完全一致）。失败时重置流再重试；
     * 服务端按 job_id 幂等，不会产生第二条历史。 */
    {
        int blob_ok = 0;

        for (int attempt = 0; attempt <= UPLOAD_HTTP_RETRY; ++attempt) {
            stream_init(&stream, (const uint8_t *)header, offset, src_bytes);
            if (put_blob(job_id, &stream, wav_total, sha_hex) == 0) {
                blob_ok = 1;
                break;
            }
            os_msleep(2000);
        }
        if (!blob_ok) {
            snprintf(reason, sizeof(reason), "blob upload failed");
            goto fail;
        }
    }

    /* 7. 终态确认。 */
    build_result_json(json, sizeof(json), 1, rec_id, wav_total, sha_hex, NULL);
    if (post_result(job_id, json) != 0) {
        snprintf(reason, sizeof(reason), "result confirmation failed");
        goto fail;
    }

    mqtt_publish_telemetry(wav_total);
    os_printf(LL_INFO, "[up] clip %u saved: %s (%u bytes, %u ms)\r\n",
              (unsigned)job->clip_id, rec_id, (unsigned)wav_total,
              (unsigned)duration_ms);
    return 0;

fail:
    os_printf(LL_ERR, "[up] clip %u failed: %s\r\n",
              (unsigned)job->clip_id, reason);
    if (job_id[0] != '\0') {
        build_result_json(json, sizeof(json), 0, rec_id, wav_total, sha_hex, reason);
        (void)post_result(job_id, json);
    }
    return -1;
}

/* 对外入口：上传期间暂停采集写 SD，避免"SD 写 + Wi-Fi"同时满载
 * （实测这种组合会让热点把设备踢掉，见 docs/08 8.6/8.7）。 */
int upload_formal_run(const struct clip_job *job)
{
    int ret;

    audio_capture_pause(1);
    ret = upload_formal_run_inner(job);
    audio_capture_pause(0);
    return ret;
}
