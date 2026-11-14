/*
 * 喇叭回放（ECW6700F，BOARD_I2S_0 / MIC_SPEAKER 接口）
 *
 * 喇叭经模块上独立的 SPK-OUT 口出声（板载 ES8311 codec + 功放）；回放的
 * I2S 数字线与 PDM 麦克风共用 MIC 连接器（WS=IO6 / SCK=IO7 / SD=IO8），
 * 数据线半双工：播放时暂停采集，把 SD 环形区里最近 APP_CLIP_SECONDS 秒的
 * PCM16LE 单声道 32kHz 从 I2S TX 送出（复制成立体声），结束后恢复采集。
 *
 * 发送流程沿用 SDK 已验证的 i2s_codec 示例（ES8311+功放实测可出声）：
 *   write_enable(STEREO, cb) -> 双 DMA 缓冲交替 -> submit(-2 表示已排队) -> drain。
 *
 * TXD 复用到共享 SD 线（IO8）：I2S init 会重映射 SDK 默认 TX 引脚
 * （TXSCK=IO1 / TXD=IO4 是电机脚，MCLK=IO11 是 SD CS），播放前先撤销，
 * 再把 IO8 切到 FUNC_I2S_TXD，最后恢复 I2C0（IO24/23）。
 */
#include "app_config.h"
#include "app_playback.h"
#include "app_audio.h"
#include "app_store.h"
#include "app_link.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "oshal.h"
#include "i2s.h"
#include "gpio.h"
#include "chip_pinmux.h"

/* 每块单声道样本数：512 样本 = 16ms 音频 = 1KB PCM = 2KB 立体声 TX */
#define PLAYBACK_BLOCK_SAMPLES   512u
#define PLAYBACK_DMA_BUFFERS     2u
#define PLAYBACK_TX_BYTES        (PLAYBACK_BLOCK_SAMPLES * 2u * 2u)
/* 采集任务可能阻塞在 1s 的 read 超时里，确认暂停最多等 2.5s */
#define PLAYBACK_ACK_TIMEOUT_MS  2500u
#define PLAYBACK_TX_DRAIN_MS     50u
#define PLAYBACK_SEM_TIMEOUT_MS  1000u

static volatile int s_pause_req;    /* 播放期间要求采集任务暂停 */
static volatile int s_paused_ack;   /* 采集任务已确认暂停（不在 audio_read 里） */

/* TX DMA 直接从用户缓冲搬运，必须放在 .dma.data 段（与 SDK 示例一致） */
static int16_t s_tx_buf[PLAYBACK_DMA_BUFFERS][PLAYBACK_TX_BYTES / sizeof(int16_t)]
    __attribute__((section(".dma.data"), aligned(4)));

static os_sem_handle_t s_tx_sem;
static volatile unsigned int s_tx_queued;
static volatile unsigned int s_tx_completed;

int playback_pause_poll(void)
{
    if (s_pause_req) {
        s_paused_ack = 1;
        return 1;
    }
    return 0;
}

/* 播放期间的引脚重绑定：撤销 SDK 默认 TX 复用（落在 SD CS / 电机脚上），
 * TXD 复用到共享 SD 线 IO8；WS/SCK 沿用采集侧的 RXWS/RXSCK（主机模式下
 * 时序由控制器驱动，与方向无关）。最后恢复 I2C0。 */
static void playback_pins(void)
{
    set_pin_function(11u, FUNC_GPIO);
    set_pin_function(4u, FUNC_GPIO);
    set_pin_function(1u, FUNC_GPIO);
    set_pin_function(APP_I2S_RXD_PIN, FUNC_I2S_TXD);
    app_link_restore_pins();
}

static void tx_callback(void *buf, unsigned int len)
{
    (void)buf;
    (void)len;
    ++s_tx_completed;
    if (s_tx_sem != NULL) {
        os_sem_post(s_tx_sem);
    }
}

static int submit_block(int16_t *buf, unsigned int len)
{
    /* 驱动在"已排队到活动 DMA 块之后"时返回 -2，视为成功 */
    int busy = s_tx_queued != s_tx_completed;
    int ret = drv_i2s_master_write(buf, len);

    if (ret != 0 && !(ret == -2 && busy)) {
        return -1;
    }
    ++s_tx_queued;
    return 0;
}

/* 暂停采集；失败返回 -1（此时已解除暂停请求）。 */
static int pause_capture(void)
{
    unsigned wait;

    s_paused_ack = 0;
    s_pause_req = 1;
    for (wait = 0u; wait < PLAYBACK_ACK_TIMEOUT_MS / 10u; ++wait) {
        if (s_paused_ack) {
            return 0;
        }
        os_msleep(10);
    }
    s_pause_req = 0;
    os_printf(LL_WARN, "[play] capture not paused, abort\r\n");
    return -1;
}

int playback_play_clip(void)
{
    uint32_t want = (uint32_t)APP_CLIP_SECONDS * APP_PCM_BYTES_PER_SEC;
    uint32_t bytes = 0u;
    uint32_t offset = 0u;
    uint32_t cursor = 0u;
    unsigned int block = 0u;
    T_DRV_I2S_CONFIG cfg;
    int failed = 0;
    int ret = -1;

    if (s_pause_req) {
        os_printf(LL_WARN, "[play] already playing\r\n");
        return -1;
    }
    if (pause_capture() != 0) {
        return -1;
    }

    /* 采集已停：释放 RX，把 I2S 重配为 16bit 立体声 TX */
    drv_i2s_master_read_disable();
    drv_i2s_master_exit();
    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = APP_PCM_SAMPLE_RATE;
    cfg.slot_format = SLOT_FORMAT_LEFT;
    cfg.slot_width = SLOT_WIDTH_16BIT;
    cfg.data_width = SLOT_DATA_WIDTH_16BIT;
    if (drv_i2s_master_init(&cfg) != 0) {
        os_printf(LL_ERR, "[play] i2s tx init failed\r\n");
        goto restore;
    }
    playback_pins();
    os_msleep(20); /* 时钟稳定（与 SDK 示例一致） */

    s_tx_sem = os_sem_create(PLAYBACK_DMA_BUFFERS, PLAYBACK_DMA_BUFFERS);
    if (s_tx_sem == NULL ||
        drv_i2s_master_write_enable(SLOT_MODE_STEREO, tx_callback) != 0) {
        os_printf(LL_ERR, "[play] tx enable failed\r\n");
        goto restore;
    }

    if (store_clip_begin(want, &bytes, &offset) != 0 || bytes == 0u) {
        os_printf(LL_WARN, "[play] no audio buffered yet\r\n");
        goto restore;
    }
    os_printf(LL_INFO, "[play] clip %u bytes, %us\r\n", (unsigned)bytes,
              (unsigned)((bytes + APP_PCM_BYTES_PER_SEC - 1u) /
                         APP_PCM_BYTES_PER_SEC));

    cursor = offset;
    while (bytes > 0u) {
        uint32_t mono_bytes = bytes > PLAYBACK_BLOCK_SAMPLES * 2u
                                  ? PLAYBACK_BLOCK_SAMPLES * 2u
                                  : bytes;
        uint32_t n = mono_bytes / 2u;
        int16_t mono_buf[PLAYBACK_BLOCK_SAMPLES];
        int16_t *out = s_tx_buf[block % PLAYBACK_DMA_BUFFERS];
        unsigned int i;

        if (os_sem_wait(s_tx_sem, PLAYBACK_SEM_TIMEOUT_MS) != 0) {
            os_printf(LL_ERR, "[play] tx sem timeout\r\n");
            failed = 1;
            break;
        }
        if (store_clip_read(&cursor, mono_buf, mono_bytes) != 0) {
            os_printf(LL_ERR, "[play] sd read failed\r\n");
            failed = 1;
            break;
        }
        for (i = 0u; i < n; ++i) {
            /* 必须按有符号除：宏是 unsigned，负样本会被提升成大正数 */
            int16_t sample = (int16_t)(mono_buf[i] / (int)APP_PLAYBACK_VOLUME_DIV);
            out[i * 2u] = sample;
            out[i * 2u + 1u] = sample;
        }
        if (submit_block(out, n * 4u) != 0) {
            os_printf(LL_ERR, "[play] tx submit failed\r\n");
            failed = 1;
            break;
        }
        bytes -= mono_bytes;
        ++block;
    }

    /* 等全部块发完（每个缓冲一个信号量），再留出最后一块的 DMA 时间 */
    if (!failed) {
        unsigned int i;

        for (i = 0u; i < PLAYBACK_DMA_BUFFERS; ++i) {
            if (os_sem_wait(s_tx_sem, PLAYBACK_SEM_TIMEOUT_MS) != 0) {
                failed = 1;
                break;
            }
        }
        os_msleep(PLAYBACK_TX_DRAIN_MS);
        for (i = 0u; i < PLAYBACK_DMA_BUFFERS; ++i) {
            os_sem_post(s_tx_sem);
        }
    }
    ret = failed ? -1 : 0;
    os_printf(failed ? LL_ERR : LL_INFO, "[play] %s\r\n",
              failed ? "failed" : "done");

restore:
    /* 带着未能完成的 TX 退出会丢失 DMA 资源；先给在途块一点时间 */
    if (s_tx_sem != NULL) {
        unsigned int guard;

        for (guard = 0u; guard < 10u && s_tx_queued != s_tx_completed; ++guard) {
            os_msleep(10);
        }
        drv_i2s_master_write_disable();
        os_sem_destroy(s_tx_sem);
        s_tx_sem = NULL;
    }
    drv_i2s_master_exit();

    /* 恢复 32kHz PDM 采集配置与引脚，全部就绪后才放行采集任务 */
    if (audio_init() != 0) {
        os_printf(LL_ERR, "[play] audio restore failed\r\n");
    }
    s_pause_req = 0;
    return ret;
}
