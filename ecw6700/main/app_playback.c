/*
 * 喇叭回放（ECW6700F，BOARD_I2S_0 / MIC_SPEAKER 接口）
 *
 * 喇叭经模块上独立的 SPK-OUT 口出声（板载 ES8311 codec + 功放）；回放的
 * I2S 数字线与 PDM 麦克风共用 MIC 连接器（WS=IO6 / SCK=IO7 / SD=IO8），
 * 数据线半双工：播放时暂停采集，播放结束再恢复。
 *
 * v2 统一 16 kHz 输出：
 *   - 情绪音效资产是 16 kHz PCM16 单声道，直接播放；
 *   - 最近 N 秒录音是 32 kHz，经 app_resample 二阶低通 + 2:1 抽取后播放。
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
#include "app_assets.h"
#include "app_resample.h"
#include "app_store.h"
#include "app_link.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "oshal.h"
#include "i2s.h"
#include "gpio.h"
#include "i2c_master.h"
#include "chip_pinmux.h"
#include "chip_smuaon.h"

/* 每块源样本数：512 个 32k 样本 -> 约 256 个 16k 样本；16k 资产 -> 512 个 */
#define PLAYBACK_BLOCK_SAMPLES   512u
#define PLAYBACK_DMA_BUFFERS     2u
#define PLAYBACK_TX_SAMPLES      PLAYBACK_BLOCK_SAMPLES
#define PLAYBACK_TX_BYTES        (PLAYBACK_TX_SAMPLES * 2u * 2u)
/* 采集任务可能阻塞在 1s 的 read 超时里，确认暂停最多等 2.5s */
#define PLAYBACK_ACK_TIMEOUT_MS  2500u
#define PLAYBACK_TX_DRAIN_MS     50u
#define PLAYBACK_SEM_TIMEOUT_MS  1000u

typedef int (*playback_read_fn)(void *ctx, int16_t *dst, uint32_t max_samples);

struct playback_source {
    playback_read_fn read;
    void *ctx;
    uint32_t src_rate;
    uint32_t total_samples;
};

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
 * 把 MIC/SPK 连接器上的 4 根线切到 TX 方向：
 *   WS=IO6 -> TXWS, SCK=IO7 -> TXSCK, SD=IO8 -> TXD, MCLK=IO3。
 * v1 只切了 IO8，WS/SCK 仍是 RX 复用；I2S 主机 TX 不会驱动 RX 引脚，
 * 实测表现为 DMA 回调不来、tx sem timeout。 */
static void playback_pins(void)
{
    set_pin_function(11u, FUNC_GPIO);
    set_pin_function(4u, FUNC_GPIO);
    set_pin_function(1u, FUNC_GPIO);
    set_pin_function(APP_I2S_RXWS_PIN, FUNC_I2S_TXWS);
    set_pin_function(APP_I2S_RXSCK_PIN, FUNC_I2S_TXSCK);
    set_pin_function(APP_I2S_RXD_PIN, FUNC_I2S_TXD);
    set_pin_function(APP_I2S_MCLK_PIN, FUNC_I2S_MCLK);
    chip_io_drive_current(APP_I2S_RXWS_PIN, CHIP_IO_DRIVE_CURRENT_8mA);
    chip_io_drive_current(APP_I2S_RXSCK_PIN, CHIP_IO_DRIVE_CURRENT_8mA);
    chip_io_drive_current(APP_I2S_RXD_PIN, CHIP_IO_DRIVE_CURRENT_8mA);
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

static int restore_capture(void)
{
    int ret = -1;

    for (int attempt = 0; attempt < 3 && ret != 0; ++attempt) {
        ret = audio_init();
        if (ret != 0) {
            os_printf(LL_ERR, "[play] audio restore failed (%d/3)\r\n", attempt + 1);
            os_msleep(100);
        }
    }
    s_pause_req = 0;
    return ret;
}

/* ------------------------------------------------------------ 源读取器 */

struct clip_reader {
    uint32_t cursor;
    uint32_t left;
};

static int clip_read(void *ctx, int16_t *dst, uint32_t max_samples)
{
    struct clip_reader *r = (struct clip_reader *)ctx;
    uint32_t want = max_samples * 2u;

    if (r->left == 0u) {
        return 0;
    }
    if (want > r->left) {
        want = r->left;
    }
    if (store_clip_read(&r->cursor, dst, want) != 0) {
        return -1;
    }
    r->left -= want;
    return (int)(want / 2u);
}

struct asset_reader {
    int index;
    uint32_t offset;
    uint32_t left;
};

static int asset_read(void *ctx, int16_t *dst, uint32_t max_samples)
{
    struct asset_reader *r = (struct asset_reader *)ctx;
    uint32_t want = max_samples * 2u;

    if (r->left == 0u) {
        return 0;
    }
    if (want > r->left) {
        want = r->left & ~1u;          /* 保持 16-bit 对齐 */
    }
    if (want == 0u) {
        return 0;
    }
    if (assets_read(r->index, r->offset, dst, want) != 0) {
        return -1;
    }
    r->offset += want;
    r->left -= want;
    return (int)(want / 2u);
}

/* ------------------------------------------------------------ ES8311 配置 */
/*
 * 与 SDK examples/drivers/i2s/i2s_codec 完全一致的 16 kHz 配置序列。
 * 必须在 I2S 已经运行在 16 kHz 时写（MCLK 与 codec 分频器匹配），
 * 所以放在第一次播放、I2S 切到 16 kHz 之后执行，而不是启动时。
 *
 * codec 控制口与板间链路共用 I2C0：先 app_link_close() 释放从机，
 * 用主机模式配置完成后再 app_link_init() 恢复。这个窗口约 100 ms，
 * 期间 STM32 的命令可能丢失；播放只在用户手势后发生，可以接受。
 */
#if APP_CODEC_CONFIG_ENABLE
static const uint8_t s_codec_settings[][2] = {
    {0x44, 0x08}, {0x44, 0x08}, {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10},
    {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00}, {0x0b, 0x00}, {0x0c, 0x00},
    {0x10, 0x1f}, {0x11, 0x7f}, {0x00, 0x80}, {0x00, 0x80}, {0x01, 0x3f},
    {0x02, 0x00}, {0x05, 0x00}, {0x03, 0x10}, {0x04, 0x10}, {0x07, 0x00},
    {0x08, 0xff}, {0x06, 0x04}, {0x09, 0x0d}, {0x0a, 0x0d}, {0x13, 0x10},
    {0x1b, 0x0a}, {0x1c, 0x6a}, {0x0d, 0x01}, {0x0e, 0x02}, {0x12, 0x00},
    {0x14, 0x1a}, {0x15, 0x40}, {0x17, 0x50}, {0x37, 0x08},
    {0x32, APP_CODEC_VOLUME}, {0x31, 0x00}, {0x45, 0x00},
};
#endif

static int s_codec_done;
static int s_codec_ok;

#if APP_CODEC_CONFIG_ENABLE
static int codec_write(void *i2c, const uint8_t data[2])
{
    T_DRV_I2C_MASTER_CONFIG cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.addr_len = I2C_ADDR_M_LEN_7;
    cfg.addr_val = APP_CODEC_I2C_ADDR;
    cfg.clk = I2C_Speed_100K;
    cfg.buf = (unsigned char *)data;
    cfg.len = 2u;
    cfg.ms_timeout = 100u;
    return drv_i2c_master_write(i2c, &cfg);
}

static int codec_read(void *i2c, uint8_t reg, uint8_t *value)
{
    T_DRV_I2C_MASTER_CONFIG cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.addr_len = I2C_ADDR_M_LEN_7;
    cfg.addr_val = APP_CODEC_I2C_ADDR;
    cfg.clk = I2C_Speed_100K;
    cfg.buf = &reg;
    cfg.len = 1u;
    cfg.ms_timeout = 100u;
    return drv_i2c_master_write_read(i2c, &cfg, value, 1u);
}

static int codec_configure(void *i2c)
{
    int ok = 1;

    (void)drv_gpio_set_pull_type(24u, DRV_GPIO_ARG_PULL_UP);
    (void)drv_gpio_set_pull_type(23u, DRV_GPIO_ARG_PULL_UP);

    for (unsigned i = 0; i < sizeof(s_codec_settings) / sizeof(s_codec_settings[0]);
         ++i) {
        int ret = codec_write(i2c, s_codec_settings[i]);

        if (ret != 2) {
            os_printf(LL_ERR, "[play] codec write reg=0x%02x ret=%d\r\n",
                      s_codec_settings[i][0], ret);
            return -1;
        }
        os_msleep(APP_CODEC_SETTLE_MS);
    }

    {
        /* 与 SDK i2s_codec 示例的 12 个回读寄存器一致。 */
        static const uint8_t verify[][2] = {
            {0x01, 0x3f}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10},
            {0x06, 0x04}, {0x07, 0x00}, {0x08, 0xff}, {0x09, 0x0d},
            {0x0a, 0x0d}, {0x15, 0x40}, {0x17, 0x50},
            {0x32, APP_CODEC_VOLUME},
        };
        unsigned pass = 0u;
        for (unsigned i = 0; i < sizeof(verify) / sizeof(verify[0]); ++i) {
            uint8_t value = 0;
            int ret = codec_read(i2c, verify[i][0], &value);
            if (ret == 1 && value == verify[i][1]) {
                ++pass;
            } else {
                os_printf(LL_WARN, "[play] codec reg=0x%02x got=0x%02x want=0x%02x ret=%d\r\n",
                          verify[i][0], value, verify[i][1], ret);
            }
        }
        os_printf(LL_INFO, "[play] codec ES8311 @0x%02x: %u/%u readbacks ok\r\n",
                  (unsigned)APP_CODEC_I2C_ADDR, pass,
                  (unsigned)(sizeof(verify) / sizeof(verify[0])));
        if (pass != sizeof(verify) / sizeof(verify[0])) {
            ok = 0;
        }
    }
    return ok ? 0 : -1;
}
#endif

/*
 * 启动时的 codec 配置。只在板间 I2C 从机尚未打开、采集尚未切到 32 kHz
 * 时调用（app_sound 的 codec 任务）。先让 I2S 以 16 kHz 运行并路由
 * MCLK/WS/BCLK，再通过 I2C0 主机写 ES8311；完成后释放 I2S 和 I2C0。
 */
int playback_ensure_codec(void)
{
    if (s_codec_done) {
        return s_codec_ok ? 0 : -1;
    }
#if !APP_CODEC_CONFIG_ENABLE
    s_codec_done = 1;
    s_codec_ok = 1;
    os_printf(LL_INFO, "[play] codec config disabled by APP_CODEC_CONFIG_ENABLE\r\n");
    return 0;
#else
    T_DRV_I2S_CONFIG cfg;
    void *i2c;
    int ret;
    int i2s_ok;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = APP_SOUND_SAMPLE_RATE;
    cfg.slot_format = SLOT_FORMAT_LEFT;
    cfg.slot_width = SLOT_WIDTH_16BIT;
    cfg.data_width = SLOT_DATA_WIDTH_16BIT;
    i2s_ok = (drv_i2s_master_init(&cfg) == 0);
    if (i2s_ok) {
        audio_pins_apply();
        os_msleep(20);              /* MCLK 稳定后再写 codec */
    } else {
        os_printf(LL_ERR, "[play] codec: i2s 16k init failed\r\n");
    }

    i2c = drv_i2c_master_open(APP_CODEC_I2C_NUM);
    if (i2c == NULL) {
        os_printf(LL_ERR, "[play] codec I2C%d open failed\r\n",
                  (int)APP_CODEC_I2C_NUM);
        if (i2s_ok) {
            drv_i2s_master_exit();
        }
        s_codec_done = 1;
        s_codec_ok = 0;
        return -1;
    }
    /* STM32 与 codec 共用这条总线；启动瞬间 STM32 可能在发 PING/触摸，
     * 多给几次机会（每次完整 37 寄存器序列）。 */
    os_msleep(200);
    ret = -1;
    for (int attempt = 0; attempt < 5; ++attempt) {
        ret = codec_configure(i2c);
        if (ret == 0) {
            break;
        }
        os_msleep(200);
    }
    drv_i2c_master_close(i2c);
    if (i2s_ok) {
        drv_i2s_master_exit();
    }
    app_link_restore_pins();

    s_codec_done = 1;
    s_codec_ok = (ret == 0);
    os_printf(s_codec_ok ? LL_INFO : LL_WARN, "[play] codec %s\r\n",
              s_codec_ok ? "configured" : "NOT configured (playback may be silent)");
    return s_codec_ok ? 0 : -1;
#endif
}

int playback_codec_state(void)
{
    if (!s_codec_done) {
        return -1;
    }
    return s_codec_ok ? 1 : 0;
}

/* ---------------------------------------------------------------- 播放器 */

static int play_source(const struct playback_source *src, unsigned volume_div,
                       E_DRV_I2S_SLOT_FORMAT slot_format)
{
    struct resampler_32to16 rs;
    struct resampler_8to16 rs8;
    int16_t mono_buf[PLAYBACK_BLOCK_SAMPLES];
    unsigned int block = 0u;
    uint32_t fed = 0u;
    int failed = 0;
    int ret = -1;
    T_DRV_I2S_CONFIG cfg;

    if (s_pause_req) {
        os_printf(LL_WARN, "[play] already playing\r\n");
        return -1;
    }
    if (src == NULL || src->read == NULL) {
        return -1;
    }
    if (pause_capture() != 0) {
        return -1;
    }

    /* 采集已停：释放 RX，把 I2S 重配为 16 kHz / 16bit 立体声 TX。
     * 先让 DMA/时钟完全停下，再重新 init；init 会重置 pinmux，
     * 所以 TX 引脚要在 init 前后各路由一次。 */
    drv_i2s_master_read_disable();
    drv_i2s_master_exit();
    os_msleep(50);
    playback_pins();
    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = APP_SOUND_SAMPLE_RATE;
    /* 参考 firmware-code-ecw-1115：I2S 主模式用 SLOT_FORMAT_LEFT + 32-bit
     * 槽；该配置在本板 SPK 模块上实测出声。数据仍是 16-bit（PCM16），
     * 硬件把 16-bit 样本左对齐放进 32-bit 槽。 */
    cfg.slot_format = slot_format;
#if APP_SOUND_TX_SLOT32
    cfg.slot_width = SLOT_WIDTH_32BIT;
    cfg.data_width = SLOT_DATA_WIDTH_16BIT;
#else
    cfg.slot_width = SLOT_WIDTH_16BIT;
    cfg.data_width = SLOT_DATA_WIDTH_16BIT;
#endif
    if (drv_i2s_master_init(&cfg) != 0) {
        os_printf(LL_ERR, "[play] i2s tx init failed\r\n");
        goto restore;
    }
    playback_pins();
    os_msleep(20); /* 时钟稳定（与 SDK 示例一致） */

    s_tx_queued = s_tx_completed = 0;
    s_tx_sem = os_sem_create(PLAYBACK_DMA_BUFFERS, PLAYBACK_DMA_BUFFERS);
    if (s_tx_sem == NULL ||
        drv_i2s_master_write_enable(SLOT_MODE_STEREO, tx_callback) != 0) {
        os_printf(LL_ERR, "[play] tx enable failed\r\n");
        goto restore;
    }

    resampler_32to16_init(&rs);
    resampler_8to16_init(&rs8);
    if (volume_div == 0u) {
        volume_div = 1u;
    }
    os_printf(LL_INFO, "[play] start: %u samples @%u Hz -> 16 kHz, vol/%u\r\n",
              (unsigned)src->total_samples, (unsigned)src->src_rate,
              volume_div);

    while (fed < src->total_samples) {
        uint32_t want = src->total_samples - fed;
        int got;
        uint32_t out_samples;
        int16_t *out = s_tx_buf[block % PLAYBACK_DMA_BUFFERS];
        unsigned int i;

        if (want > PLAYBACK_BLOCK_SAMPLES) {
            want = PLAYBACK_BLOCK_SAMPLES;
        }
        /* 8 kHz 源要 2× 升采样，输出不能超过 TX 块样本数 */
        if (src->src_rate == APP_PCM_SAMPLE_RATE &&
            want > PLAYBACK_TX_SAMPLES / 2u) {
            want = PLAYBACK_TX_SAMPLES / 2u;
        }
        got = src->read(src->ctx, mono_buf, want);
        if (got <= 0) {
            if (got < 0) {
                os_printf(LL_ERR, "[play] source read failed\r\n");
                failed = 1;
            }
            break;
        }
        fed += (uint32_t)got;

        if (src->src_rate == APP_SOUND_SAMPLE_RATE) {
            out_samples = (uint32_t)got;
            memcpy(out, mono_buf, out_samples * sizeof(int16_t));
        } else if (src->src_rate == APP_PCM_SAMPLE_RATE) {
            /* 8 kHz 录音 -> 16 kHz 播放 */
            out_samples = resampler_8to16_run(&rs8, mono_buf, (uint32_t)got,
                                              out, PLAYBACK_TX_SAMPLES);
        } else {
            out_samples = resampler_32to16_run(&rs, mono_buf, (uint32_t)got,
                                               out, PLAYBACK_TX_SAMPLES);
        }
        if (out_samples == 0u) {
            continue;
        }
        if (os_sem_wait(s_tx_sem, PLAYBACK_SEM_TIMEOUT_MS) != 0) {
            os_printf(LL_ERR, "[play] tx sem timeout\r\n");
            failed = 1;
            break;
        }
        /* 单声道复制成立体声，并按音量分母衰减。
         * 必须从后往前展开：out 同时是源缓冲，正序会把尚未读取的样本覆盖。 */
        for (i = out_samples; i-- > 0u;) {
            int16_t sample = (int16_t)(out[i] / (int)volume_div);
            out[i * 2u] = sample;
            out[i * 2u + 1u] = sample;
        }
        if (submit_block(out, out_samples * 4u) != 0) {
            os_printf(LL_ERR, "[play] tx submit failed\r\n");
            failed = 1;
            break;
        }
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
    (void)restore_capture();
    return ret;
}

int playback_play_clip(void)
{
    uint32_t want = (uint32_t)APP_CLIP_SECONDS * APP_PCM_BYTES_PER_SEC;
    uint32_t bytes = 0u;
    uint32_t offset = 0u;
    struct clip_reader reader;
    struct playback_source src;

    if (store_clip_begin(want, &bytes, &offset) != 0 || bytes == 0u) {
        os_printf(LL_WARN, "[play] no audio buffered yet\r\n");
        return -1;
    }
    reader.cursor = offset;
    reader.left = bytes;
    src.read = clip_read;
    src.ctx = &reader;
    src.src_rate = APP_PCM_SAMPLE_RATE;
    src.total_samples = bytes / 2u;
    os_printf(LL_INFO, "[play] clip %u bytes, %us\r\n", (unsigned)bytes,
              (unsigned)((bytes + APP_PCM_BYTES_PER_SEC - 1u) /
                         APP_PCM_BYTES_PER_SEC));
    return play_source(&src, APP_PLAYBACK_VOLUME_DIV, SLOT_FORMAT_LEFT);
}

int playback_play_asset(int asset_index)
{
    struct asset_info info;
    struct asset_reader reader;
    struct playback_source src;

    if (!assets_ready()) {
        os_printf(LL_WARN, "[play] sound assets not ready\r\n");
        return -1;
    }
    if (assets_get(asset_index, &info) != 0) {
        os_printf(LL_WARN, "[play] bad asset %d\r\n", asset_index);
        return -1;
    }
    reader.index = asset_index;
    reader.offset = 0u;
    reader.left = info.bytes;
    src.read = asset_read;
    src.ctx = &reader;
    src.src_rate = info.sample_rate;
    src.total_samples = info.bytes / 2u;

    os_printf(LL_INFO, "[play] sound %s (%u ms, class %u)\r\n",
              info.name, (unsigned)info.duration_ms, (unsigned)info.class_id);
    return play_source(&src, APP_SOUND_VOLUME_DIV, SLOT_FORMAT_LEFT);
}

/* ------------------------------------------------------------ 测试音 */
struct tone_reader {
    uint32_t hz;
    uint32_t phase;
    uint32_t left;
    int16_t level;
};

static int tone_read(void *ctx, int16_t *dst, uint32_t max_samples)
{
    struct tone_reader *r = (struct tone_reader *)ctx;
    uint32_t n = 0u;

    while (n < max_samples && r->left > 0u) {
        r->phase += r->hz;
        if (r->phase >= APP_SOUND_SAMPLE_RATE) {
            r->phase -= APP_SOUND_SAMPLE_RATE;
            r->level = (int16_t)-r->level;
        }
        dst[n++] = r->level;
        --r->left;
    }
    return (int)n;
}

int playback_play_tone(uint32_t hz, uint32_t ms, int use_i2s_format)
{
    struct tone_reader reader;
    struct playback_source src;

    if (hz == 0u || ms == 0u) {
        return -1;
    }
    reader.hz = hz;
    reader.phase = 0u;
    reader.left = (ms * APP_SOUND_SAMPLE_RATE) / 1000u;
    reader.level = 12000;
    src.read = tone_read;
    src.ctx = &reader;
    src.src_rate = APP_SOUND_SAMPLE_RATE;
    src.total_samples = reader.left;
    os_printf(LL_INFO, "[play] test tone %u Hz %u ms, %s format\r\n",
              (unsigned)hz, (unsigned)ms,
              use_i2s_format ? "I2S" : "left");
#if APP_SOUND_TX_SLOT32
    /* 32-bit 槽配置下 I2S/左对齐的差别只在 BCK 相位，SPK 模块按左对齐
     * 出声，所以诊断音也统一走 LEFT。 */
    (void)use_i2s_format;
    return play_source(&src, 1u, SLOT_FORMAT_LEFT);
#else
    return play_source(&src, 1u,
                       use_i2s_format ? SLOT_FORMAT_I2S : SLOT_FORMAT_LEFT);
#endif
}
