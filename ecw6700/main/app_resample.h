#ifndef APP_RESAMPLE_H
#define APP_RESAMPLE_H

#include <stdint.h>

/*
 * 32 kHz -> 16 kHz 流式降采样（2:1）。
 *
 * 录音链路是 32 kHz PCM16 单声道；服务器和喇叭回放都用 16 kHz。
 * 这里用二阶 Butterworth 低通（截止约 7.2 kHz，32 kHz 采样率）后再
 * 每 2 个样本取 1 个，避免简单抽取带来的混叠。状态可跨块保留。
 */
struct resampler_32to16 {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
    uint8_t phase;
};

void resampler_32to16_init(struct resampler_32to16 *r);

/* 输入 n 个 32 kHz 样本，输出最多 cap 个 16 kHz 样本；返回输出样本数。 */
uint32_t resampler_32to16_run(struct resampler_32to16 *r,
                              const int16_t *in, uint32_t n,
                              int16_t *out, uint32_t cap);

/*
 * 8 kHz -> 16 kHz 升采样（2:1，线性插值）。
 * 录音改为 8 kHz 后，服务器要求 16 kHz WAV，这里做 2× 插值；
 * 8 kHz 录音本身已经过 3.4 kHz 低通，插值产生的镜像在 4 kHz 以上，影响很小。
 */
struct resampler_8to16 {
    int16_t prev;
    uint8_t have;
};

void resampler_8to16_init(struct resampler_8to16 *r);

/* 输入 n 个 8 kHz 样本，输出最多 cap 个 16 kHz 样本；返回输出样本数。 */
uint32_t resampler_8to16_run(struct resampler_8to16 *r,
                             const int16_t *in, uint32_t n,
                             int16_t *out, uint32_t cap);

#endif
