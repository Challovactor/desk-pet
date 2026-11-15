/*
 * MOCE「记忆时刻」ECW6700F 主控固件（2026-11 迭代版）
 *
 * 新架构：
 *   STM32 只负责触摸屏 -> I2C 写 1 字节事件；
 *   ECW6700 负责 PDM 采集、SD 环形存储、Wi-Fi 连服务器、HTTP 上传。
 *
 * 数据流：
 *   MIC(I2S) -> PDM 解码 -> PCM16 -> SD 裸扇区环形区（持续写）
 *   触摸(I2C 从机 0x42) -> 取最近 APP_CLIP_SECONDS 秒 -> POST 事件 + POST WAV
 *   回放(I2C 命令 0x03) -> 暂停采集 -> 喇叭播放最近 N 秒 -> 恢复采集
 */
#include "app_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oshal.h"
#include "auto_generate_log_common.h"

#include "app_audio.h"
#include "app_assets.h"
#include "app_store.h"
#include "app_link.h"
#include "app_net.h"
#include "app_playback.h"
#include "app_sound.h"
#include "app_upload.h"
#include "app_mqtt.h"

#define APP_STATUS_PERIOD_MS 10000u

static int16_t s_pcm[APP_PCM_SAMPLES_MAX] __attribute__((aligned(4)));

/* LOGO 取图请求计数：只打印前几次和每 50 次，避免刷屏。 */
static unsigned s_logo_reqs;

/* ------------------------------------------------------------ 采集任务 */

static void capture_task(void *arg)
{
    (void)arg;

    /* codec 任务需要 I2S 先以 16 kHz 运行；等它释放 I2S 后再切 32 kHz 采集。
     * 设上限，codec 异常时仍要保证录音链路能起来。 */
    for (unsigned waited = 0u; !sound_ready() && waited < 6000u; waited += 10u) {
        os_msleep(10);
    }
    if (!sound_ready()) {
        os_printf(LL_WARN, "[app] codec task not ready, capture continues\r\n");
    }

    /* 先占用 I2S 的 DMA 通道，再初始化 SPI/SD（DMA 通道只有 6 个）。 */
    for (;;) {
        if (audio_init() == 0) {
            break;
        }
        os_printf(LL_ERR, "[app] audio init failed, retry in 2s\r\n");
        os_msleep(2000);
    }

    for (;;) {
        if (store_init() == 0) {
            break;
        }
        os_printf(LL_ERR, "[app] storage init failed, retry in 2s\r\n");
        os_msleep(2000);
    }
    os_printf(LL_INFO, "[app] storage ready, %lu KB ring\r\n",
              (unsigned long)(store_ring_bytes() / 1024u));

    /* 资产校验/安装是 GPIO 位翻转软件 SPI 的忙等，会长时间抢占 CPU。
     * 先让 Wi-Fi 完成首次扫描和关联（最多等 25 秒），避免 supplicant 被
     * 饿死；热点不可用时也不无限等待，超时后照常入库，音效晚一点可用。
     * 顺序依据见 docs/10-wifi-boot-sequencing.md。 */
    for (unsigned waited = 0u; !net_connected() && waited < 25000u;
         waited += 100u) {
        os_msleep(100);
    }

    /* 情绪音效首次启动会从 flash 写入 SD 保留区；失败不影响录音/上传。 */
    if (assets_init() != 0) {
        os_printf(LL_WARN, "[app] sound assets unavailable\r\n");
    }

    for (;;) {
        /* 喇叭回放期间暂停采集：喇叭与麦克风共用 I2S 数据线（半双工） */
        if (playback_pause_poll()) {
            os_msleep(20);
            continue;
        }
        int samples = audio_read(s_pcm, APP_PCM_SAMPLES_MAX);
        if (samples <= 0) {
            os_msleep(5);
            continue;
        }
        if (audio_capture_paused()) {
            /* 上传期间暂停写盘（仍继续读/解码，避免 DMA 溢出）：
             * 用于验证 SD 写与 Wi-Fi 的硬件耦合。 */
            continue;
        }
        if (store_write(s_pcm, (uint32_t)samples * 2u) != 0) {
            os_printf(LL_WARN, "[app] store write failed\r\n");
        }
    }
}

/* ---------------------------------------------------- 触发队列与上传任务 */

static os_queue_handle_t s_jobs;
static uint32_t s_clip_seq;

static void handle_touch(void)
{
    uint32_t want = (uint32_t)APP_CLIP_SECONDS * APP_PCM_BYTES_PER_SEC;
    uint32_t bytes = 0;
    uint32_t offset = 0;
    uint32_t seconds;
    struct clip_job job;

    if (store_clip_begin(want, &bytes, &offset) != 0 || bytes == 0u) {
        os_printf(LL_WARN, "[app] touch ignored: no audio buffered yet\r\n");
        return;
    }
    seconds = (bytes + APP_PCM_BYTES_PER_SEC - 1u) / APP_PCM_BYTES_PER_SEC;
    job.offset = offset;
    job.bytes = bytes;
    job.seconds = seconds;
    job.clip_id = ++s_clip_seq;
    /* desk-recorder/v1：同一次保存的所有网络重试必须复用同一个编号。 */
    snprintf(job.idem, sizeof(job.idem), "%s-%08lx-%08lx",
             APP_DEVICE_ID, (unsigned long)os_time_get(),
             (unsigned long)job.clip_id);

    if (s_jobs == NULL) {
        os_printf(LL_ERR, "[app] no upload queue, clip %u dropped\r\n",
                  (unsigned)job.clip_id);
        return;
    }
    /* 只入队，不在 I2C 读取任务里做网络；上传期间的新触摸不会丢 */
    if (os_queue_send(s_jobs, (char *)&job, sizeof(job), 0) != 0) {
        os_printf(LL_WARN, "[app] upload queue full, clip %u dropped\r\n",
                  (unsigned)job.clip_id);
        return;
    }
    os_printf(LL_INFO, "[app] clip %u queued: %u bytes, %us\r\n",
              (unsigned)job.clip_id, (unsigned)bytes, (unsigned)seconds);
}

static void upload_task(void *arg)
{
    struct clip_job job;

    (void)arg;
    if (s_jobs == NULL) {
        os_printf(LL_ERR, "[app] upload task has no queue\r\n");
        for (;;) {
            os_msleep(1000);
        }
    }
    for (;;) {
        if (os_queue_receive(s_jobs, (char *)&job, sizeof(job), WAIT_FOREVER) != 0) {
            os_msleep(100);
            continue;
        }
        os_printf(LL_INFO, "[app] uploading clip %u\r\n", (unsigned)job.clip_id);
#if APP_UPLOAD_FORMAL
        /* 正式协议：内部自己等 Wi-Fi/MQTT，并完成四步上传。 */
        (void)upload_formal_run(&job);
#else
        {
            /* 旧明文 HTTP：弱网/刚上电时最多等 90 秒，仍无 IP 则放弃本片段 */
            int tries;

            for (tries = 0; tries < 3 && net_wait_connected(30000) != 0; ++tries) {
            }
            if (tries >= 3) {
                os_printf(LL_WARN, "[app] clip %u: wifi down, skipped\r\n",
                          (unsigned)job.clip_id);
                continue;
            }
            (void)net_report_record(job.bytes, job.seconds, job.clip_id);
            (void)net_upload_clip(job.offset, job.bytes, job.seconds, job.clip_id);
        }
#endif
    }
}

static void trigger_task(void *arg)
{
    uint8_t cmd;
    long long last_status = 0;

    (void)arg;

    /* 等 app-codec 任务在 I2S 16 kHz 下完成 ES8311 配置，再打开板间 I2C 从机。 */
    (void)sound_init();

    for (;;) {
        if (app_link_init() == 0) {
            break;
        }
        os_printf(LL_ERR, "[app] link init failed, retry in 2s\r\n");
        os_msleep(2000);
    }

    for (;;) {
        if (app_link_read_byte(&cmd, 20) == 0) {
            switch (cmd) {
            case APP_LINK_CMD_TOUCH:
                /* 任意触摸（长按/未知手势）：随机播一个音效，再取录音快照 */
                (void)sound_play_random();
                handle_touch();
#if APP_SOUND_TEST_TONE
                /* 诊断用：再播 1 kHz 方波，便于定位 SPK（默认已关闭） */
                (void)playback_play_tone(APP_SOUND_TEST_TONE_HZ,
                                         APP_SOUND_TEST_TONE_MS,
                                         APP_SOUND_TEST_TONE_I2S);
#endif
                break;
            case APP_LINK_CMD_GESTURE_CLICK:
            case APP_LINK_CMD_GESTURE_DOUBLE:
            case APP_LINK_CMD_GESTURE_UP:
            case APP_LINK_CMD_GESTURE_DOWN:
                /* 先播情绪音效（暂停采集、只读音效区），播完再快照/入队。
                 * 这样播放与上传不会同时抢 SD；触摸瞬间仍在环形区的最近
                 * 30 秒窗口内（窗口末尾包含播放期的静默）。 */
                (void)sound_play_gesture(cmd);
                handle_touch();
                break;
            case APP_LINK_CMD_PLAY:
                /* 调试回放最近 N 秒；阻塞约 N 秒，期间新触摸进 I2C 从机缓冲 */
                (void)playback_play_clip();
                break;
            case APP_LINK_CMD_LOGO_READ: {
                /* STM32 开机取图：magic 0x5A + seq + offset u16 + len u16 + chk，
                 * chk = 前 6 字节异或；从机回 [seq, data]，seq 不匹配主机重试。 */
                uint8_t args[7];

                if (app_link_read_args(args, sizeof(args), 50u) == 0) {
                    uint8_t chk = 0u;

                    for (unsigned i = 0; i < 6u; ++i) {
                        chk ^= args[i];
                    }
                    if (args[0] != 0x5Au || chk != args[6]) {
                        if (s_logo_reqs < 5u) {
                            os_printf(LL_WARN, "[link] logo bad frame %02x %02x %02x %02x %02x %02x %02x\r\n",
                                      args[0], args[1], args[2], args[3], args[4],
                                      args[5], args[6]);
                        }
                    } else {
                        uint32_t off = (uint32_t)args[2] | ((uint32_t)args[3] << 8);
                        uint32_t len = (uint32_t)args[4] | ((uint32_t)args[5] << 8);

                        if (s_logo_reqs < 10u || (s_logo_reqs % 50u) == 0u) {
                            os_printf(LL_INFO, "[link] logo req seq=%u off=%lu len=%lu\r\n",
                                      (unsigned)args[1], (unsigned long)off,
                                      (unsigned long)len);
                        }
                        app_link_request_logo(args[1], off, len);
                    }
                    ++s_logo_reqs;
                }
                break;
            }
            case APP_LINK_CMD_PING:
                os_printf(LL_INFO, "[app] link ping from stm32\r\n");
                break;
            default:
                os_printf(LL_WARN, "[app] unknown link cmd 0x%02x\r\n", cmd);
                break;
            }
        }

        if ((unsigned long long)(os_time_get() - last_status) >= APP_STATUS_PERIOD_MS) {
            last_status = os_time_get();
            os_printf(LL_INFO,
                      "[app] status: wifi=%d total=%lu KB ring=%lu KB sounds=%d codec=%d\r\n",
                      net_connected(),
                      (unsigned long)(store_total_bytes() / 1024u),
                      (unsigned long)(store_ring_bytes() / 1024u),
                      assets_ready() ? assets_count() : 0,
                      sound_codec_ok());
        }
    }
}

/* ---------------------------------------------------------------- 入口 */

void moce_app_main(void)
{
    int task;

    /* SDK 的 PS_wifi/PS_rf 每秒多行 INFO 会淹没 115200 日志，并把应用侧
     * 的 [net] 断线/重连事件挤掉。只保留 WARN 及以上。 */
    g_log_map[LM_PS_WIFI].level = LL_WARN;
    g_log_map[LM_PS_RF].level = LL_WARN;
    g_log_map[LM_PS_PSM].level = LL_WARN;
    g_log_map[LM_PS_BLE_5_1].level = LL_WARN;

    os_printf(LL_INFO, "\r\n[app] MOCE memory-moment firmware\r\n");
    os_printf(LL_INFO, "[app] clip=%us rate=%uHz %uB/s\r\n",
              (unsigned)APP_CLIP_SECONDS,
              (unsigned)APP_PCM_SAMPLE_RATE,
              (unsigned)APP_PCM_BYTES_PER_SEC);

    /* 事件订阅必须在调度器启动前完成（与 SDK 示例一致）。 */
    net_init();

    s_jobs = os_queue_create("app-jobs", APP_UPLOAD_QUEUE_LEN,
                             sizeof(struct clip_job), 0);
    if (s_jobs == NULL) {
        os_printf(LL_ERR, "[app] upload queue create failed\r\n");
    }

    /* codec 任务最先启动：它独占 I2S/I2C0 完成 ES8311 配置。 */
    sound_start();

    task = os_task_create("app-capture", APP_TASK_CAPTURE_PRIO,
                          APP_TASK_CAPTURE_STACK, capture_task, NULL);
    if (task == OS_TASK_CREATE_FAILED) {
        os_printf(LL_ERR, "[app] capture task create failed\r\n");
    }

    task = os_task_create("app-trigger", APP_TASK_TRIGGER_PRIO,
                          APP_TASK_TRIGGER_STACK, trigger_task, NULL);
    if (task == OS_TASK_CREATE_FAILED) {
        os_printf(LL_ERR, "[app] trigger task create failed\r\n");
    }

    task = os_task_create("app-upload", APP_TASK_UPLOAD_PRIO,
                          APP_TASK_UPLOAD_STACK, upload_task, NULL);
    if (task == OS_TASK_CREATE_FAILED) {
        os_printf(LL_ERR, "[app] upload task create failed\r\n");
    }

    if (net_start() != 0) {
        os_printf(LL_ERR, "[app] net task create failed\r\n");
    }

#if APP_UPLOAD_FORMAL
    /* MQTT/TLS presence + commands/events；任务内部先等 Wi-Fi。 */
    app_mqtt_start();
#endif
}
