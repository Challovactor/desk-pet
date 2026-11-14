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
#include <string.h>

#include "oshal.h"

#include "app_audio.h"
#include "app_store.h"
#include "app_link.h"
#include "app_net.h"
#include "app_playback.h"

#define APP_STATUS_PERIOD_MS 10000u

static int16_t s_pcm[APP_PCM_SAMPLES_MAX] __attribute__((aligned(4)));

/* ------------------------------------------------------------ 采集任务 */

static void capture_task(void *arg)
{
    (void)arg;

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
        if (store_write(s_pcm, (uint32_t)samples * 2u) != 0) {
            os_printf(LL_WARN, "[app] store write failed\r\n");
        }
    }
}

/* ---------------------------------------------------- 触发队列与上传任务 */

struct clip_job {
    uint32_t offset;
    uint32_t bytes;
    uint32_t seconds;
    uint32_t clip_id;
};

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
        int tries;
        if (os_queue_receive(s_jobs, (char *)&job, sizeof(job), WAIT_FOREVER) != 0) {
            os_msleep(100);
            continue;
        }
        os_printf(LL_INFO, "[app] uploading clip %u\r\n", (unsigned)job.clip_id);
        /* 弱网/刚上电时最多等 90 秒，仍无 IP 则放弃本片段 */
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
}

static void trigger_task(void *arg)
{
    uint8_t cmd;
    long long last_status = 0;

    (void)arg;

    for (;;) {
        if (app_link_init() == 0) {
            break;
        }
        os_printf(LL_ERR, "[app] link init failed, retry in 2s\r\n");
        os_msleep(2000);
    }

    for (;;) {
        if (app_link_read_byte(&cmd, 20) == 0) {
            if (cmd == APP_LINK_CMD_TOUCH) {
                handle_touch();
            } else if (cmd == APP_LINK_CMD_PLAY) {
                /* 回放最近 N 秒；阻塞约 N 秒，期间新触摸进 I2C 从机缓冲 */
                (void)playback_play_clip();
            } else if (cmd == APP_LINK_CMD_PING) {
                os_printf(LL_INFO, "[app] link ping from stm32\r\n");
            } else {
                os_printf(LL_WARN, "[app] unknown link cmd 0x%02x\r\n", cmd);
            }
        }

        if ((unsigned long long)(os_time_get() - last_status) >= APP_STATUS_PERIOD_MS) {
            last_status = os_time_get();
            os_printf(LL_INFO, "[app] status: wifi=%d total=%lu KB ring=%lu KB\r\n",
                      net_connected(),
                      (unsigned long)(store_total_bytes() / 1024u),
                      (unsigned long)(store_ring_bytes() / 1024u));
        }
    }
}

/* ---------------------------------------------------------------- 入口 */

void moce_app_main(void)
{
    int task;

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
}
