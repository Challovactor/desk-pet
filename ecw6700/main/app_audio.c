/*
 * PDM 麦克风采集与解码（ECW6700F，BOARD_I2S_0 / MIC 接口）
 *
 * 滤波算法沿用 SDK 参考实现 msm261dgt003.c：
 *   三级 cascaded 64-tap moving sum + 64:1 抽取 + 一阶 DC blocker。
 * 32kHz × 64 bit = 2.048Mbit/s PDM，64 抽取后得到 32kHz PCM16。
 *
 * 2026-11-15 起：为降低 SD 写入与 CPU 占用，32kHz PCM 再经 4:1 低通抽取
 * 得到 8kHz PCM（4 阶 Butterworth，fc≈3.4kHz）存入环形区；上传/回放时
 * 用 2× 线性插值升到 16kHz（服务端 WAV 规格）。人声（电话带 0.3–3.4kHz）
 * 足够，SD 写入从 64KB/s 降到 16KB/s。
 *
 * 每次读取 APP_PDM_READ_BYTES=8192 字节原始 PDM：
 *   8192 B = 2048 个 uint32 = 65536 bit = 1024 个 PCM 样本（32ms 音频）。
 * 32-bit slot 是 MSB-first 左对齐，因此按 uint32 的 bit31..0 解码；
 * 用 uint16 拆半字会把每个 slot 的两个 16bit 组对调。
 */
#include "app_config.h"
#include "app_audio.h"
#include "app_link.h"

#include <stddef.h>
#include <string.h>

#include "oshal.h"
#include "i2s.h"
#include "gpio.h"
#include "chip_pinmux.h"
#include "chip_smuaon.h"

struct pdm_filter {
    int32_t history[3][64];
    int32_t sum[3];
    int32_t previous;
    int32_t highpass;
    unsigned position;
};

static struct pdm_filter s_filter;
static uint8_t s_raw[APP_PDM_READ_BYTES] __attribute__((aligned(4)));
static int s_ready;

static void pdm_filter_reset(void)
{
    memset(&s_filter, 0, sizeof(s_filter));
}

/* ---------------- 32kHz -> 8kHz：4 阶 Butterworth + 4:1 抽取 ---------------- */
/* 两个二阶节（Q=0.5412 / 1.3066），fc=3.4kHz，fs=32kHz，直流增益 1。 */
struct decim4_state {
    float b01, b11, b21, a11, a21;
    float x1a, x2a, y1a, y2a;
    float b02, b12, b22, a12, a22;
    float x1b, x2b, y1b, y2b;
    uint8_t phase;
};

static struct decim4_state s_decim;
static int16_t s_decim_tmp[APP_PCM_SAMPLES_MAX];

static void decim4_reset(void)
{
    memset(&s_decim, 0, sizeof(s_decim));
    s_decim.b01 = 0.0682847990f; s_decim.b11 = 0.1365695980f;
    s_decim.b21 = 0.0682847990f; s_decim.a11 = -0.9991511490f;
    s_decim.a21 = 0.2722903450f;
    s_decim.b02 = 0.0867815178f; s_decim.b12 = 0.1735630357f;
    s_decim.b22 = 0.0867815178f; s_decim.a12 = -1.2697972975f;
    s_decim.a22 = 0.6169233689f;
}

static int decim4_run(const int16_t *in, int n, int16_t *out, int cap)
{
    int produced = 0;

    for (int i = 0; i < n; ++i) {
        float x = (float)in[i];
        float ya = s_decim.b01 * x + s_decim.b11 * s_decim.x1a +
                   s_decim.b21 * s_decim.x2a - s_decim.a11 * s_decim.y1a -
                   s_decim.a21 * s_decim.y2a;
        float yb;

        s_decim.x2a = s_decim.x1a; s_decim.x1a = x;
        s_decim.y2a = s_decim.y1a; s_decim.y1a = ya;

        yb = s_decim.b02 * ya + s_decim.b12 * s_decim.x1b +
             s_decim.b22 * s_decim.x2b - s_decim.a12 * s_decim.y1b -
             s_decim.a22 * s_decim.y2b;
        s_decim.x2b = s_decim.x1b; s_decim.x1b = ya;
        s_decim.y2b = s_decim.y1b; s_decim.y1b = yb;

        if (++s_decim.phase >= 4u) {
            int32_t v;

            s_decim.phase = 0u;
            v = (int32_t)(yb >= 0.0f ? yb + 0.5f : yb - 0.5f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            if (produced < cap) {
                out[produced] = (int16_t)v;
            }
            ++produced;
        }
    }
    return produced > cap ? cap : produced;
}

/* SDK 默认 I2S 引脚与板级绑定不一致：MCLK=11 是 SD CS，TXWS=24/RXWS=23
 * 是 I2C0，TXSCK=1/TXD=4 是电机脚。先撤销这些默认复用，再按
 * BOARD_I2S_0 = {6,7,8,3} 路由接收侧。 */
static void app_audio_pins(void)
{
    set_pin_function(11u, FUNC_GPIO);
    set_pin_function(4u, FUNC_GPIO);
    set_pin_function(1u, FUNC_GPIO);

    set_pin_function(APP_I2S_RXSCK_PIN, FUNC_I2S_RXSCK);
    set_pin_function(APP_I2S_RXWS_PIN, FUNC_I2S_RXWS);
    set_pin_function(APP_I2S_RXD_PIN, FUNC_I2S_RXD);
    set_pin_function(APP_I2S_MCLK_PIN, FUNC_I2S_MCLK);

    chip_io_drive_current(APP_I2S_RXSCK_PIN, CHIP_IO_DRIVE_CURRENT_8mA);
    chip_io_drive_current(APP_I2S_RXWS_PIN, CHIP_IO_DRIVE_CURRENT_8mA);
    chip_io_drive_current(APP_I2S_MCLK_PIN, CHIP_IO_DRIVE_CURRENT_8mA);

    /* I2S 初始化会重映射 IO24/23；恢复 I2C0。 */
    app_link_restore_pins();
}

/* n 为 uint32 数量；I2S 是 MSB-first 左对齐，按 bit31..0 解码。
 * 每 64 个 PDM bit 输出 1 个 PCM 样本。 */
static int pdm_decode(const uint32_t *raw, size_t n, int16_t *pcm, size_t capacity)
{
    size_t output = 0;

    for (size_t i = 0; i < n; ++i) {
        for (unsigned bit = 0; bit < 32u; ++bit) {
            int32_t value = (raw[i] & (0x80000000u >> bit)) ? 1 : -1;

            for (unsigned stage = 0; stage < 3u; ++stage) {
                s_filter.sum[stage] += value - s_filter.history[stage][s_filter.position];
                s_filter.history[stage][s_filter.position] = value;
                value = s_filter.sum[stage];
            }
            if (++s_filter.position == 64u) {
                int32_t sample;

                s_filter.position = 0u;
                sample = value / 8;
                s_filter.highpass = sample - s_filter.previous +
                                    s_filter.highpass * 255 / 256;
                s_filter.previous = sample;
                sample = s_filter.highpass;
                if (sample > 32767) {
                    sample = 32767;
                }
                if (sample < -32768) {
                    sample = -32768;
                }
                if (output < capacity) {
                    pcm[output] = (int16_t)sample;
                }
                ++output;
            }
        }
    }
    return output > capacity ? (int)capacity : (int)output;
}

void audio_pins_apply(void)
{
    app_audio_pins();
}

int audio_init(void)
{
    T_DRV_I2S_CONFIG cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = APP_PDM_SAMPLE_RATE;
    cfg.slot_format = SLOT_FORMAT_LEFT;
    cfg.slot_width = SLOT_WIDTH_32BIT;
    cfg.data_width = SLOT_DATA_WIDTH_32BIT;

    if (drv_i2s_master_init(&cfg) != 0) {
        os_printf(LL_ERR, "[audio] i2s init failed\r\n");
        /* 释放可能已部分初始化的总线，否则下一次 init 会返回 1 */
        drv_i2s_master_exit();
        app_audio_pins();
        return -1;
    }
    app_audio_pins();

    /* PDM 需要每帧连续 64 bit（2×32bit slot）；MONO 会丢一半 PDM 位。 */
    if (drv_i2s_master_read_enable(SLOT_MODE_STEREO, APP_PDM_READ_BYTES) != 0) {
        os_printf(LL_ERR, "[audio] read enable failed\r\n");
        drv_i2s_master_exit();
        app_audio_pins();
        return -1;
    }
    pdm_filter_reset();
    decim4_reset();
    s_ready = 1;
    os_printf(LL_INFO, "[audio] PDM armed: %u Hz, %u B/read -> %u samples/32k, "
                       "%u samples/8k\r\n",
              (unsigned)APP_PDM_SAMPLE_RATE,
              (unsigned)APP_PDM_READ_BYTES,
              (unsigned)APP_PCM_SAMPLES_MAX,
              (unsigned)(APP_PCM_SAMPLES_MAX / 4u));
    return 0;
}

int audio_deinit(void)
{
    if (s_ready) {
        drv_i2s_master_read_disable();
        drv_i2s_master_exit();
        s_ready = 0;
    }
    return 0;
}

static volatile int s_capture_paused;

void audio_capture_pause(int on)
{
    s_capture_paused = on ? 1 : 0;
}

int audio_capture_paused(void)
{
    return s_capture_paused;
}

int audio_read(int16_t *pcm, unsigned int max_samples)
{
    int got;
    int samples;

    if (!s_ready || pcm == NULL || max_samples == 0u) {
        return -1;
    }
    got = drv_i2s_master_read_timeout(s_raw, APP_PDM_READ_BYTES, 1000);
    if (got <= 0) {
        static uint32_t err_count;
        /* 输入缺口后滤波器历史已失效，重置以免把旧状态带进下一段 */
        pdm_filter_reset();
        decim4_reset();
        if ((++err_count % 100u) == 1u) {
            os_printf(LL_WARN, "[audio] read failed got=%d (count=%u)\r\n",
                      got, (unsigned)err_count);
        }
        return got;
    }
    if ((unsigned)got > APP_PDM_READ_BYTES) {
        got = APP_PDM_READ_BYTES;
    }
    /* 先解码为 32kHz PCM，再 4:1 抽取为 8kHz 写入环形区 */
    samples = pdm_decode((const uint32_t *)s_raw, (size_t)got / 4u,
                         s_decim_tmp, APP_PCM_SAMPLES_MAX);
    if (samples <= 0) {
        return samples;
    }
    return decim4_run(s_decim_tmp, samples, pcm, (int)max_samples);
}
