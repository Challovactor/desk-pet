#include "app_resample.h"

#include <stddef.h>

/*
 * 二阶 Butterworth 低通，fs=32 kHz，fc=7.2 kHz，Q=1/sqrt(2)。
 * 系数由标准双线性变换得到，直流增益为 1。
 */
void resampler_32to16_init(struct resampler_32to16 *r)
{
    if (r == NULL) {
        return;
    }
    r->b0 = 0.2483410790f;
    r->b1 = 0.4966821579f;
    r->b2 = 0.2483410790f;
    r->a1 = -0.1842138031f;
    r->a2 = 0.1775781189f;
    r->x1 = r->x2 = 0.0f;
    r->y1 = r->y2 = 0.0f;
    r->phase = 0u;
}

uint32_t resampler_32to16_run(struct resampler_32to16 *r,
                              const int16_t *in, uint32_t n,
                              int16_t *out, uint32_t cap)
{
    uint32_t produced = 0u;

    if (r == NULL || in == NULL || out == NULL) {
        return 0u;
    }
    for (uint32_t i = 0; i < n; ++i) {
        float x = (float)in[i];
        float y = r->b0 * x + r->b1 * r->x1 + r->b2 * r->x2
                  - r->a1 * r->y1 - r->a2 * r->y2;

        r->x2 = r->x1;
        r->x1 = x;
        r->y2 = r->y1;
        r->y1 = y;

        if (++r->phase >= 2u) {
            int32_t s;

            r->phase = 0u;
            s = (int32_t)(y >= 0.0f ? y + 0.5f : y - 0.5f);
            if (s > 32767) {
                s = 32767;
            }
            if (s < -32768) {
                s = -32768;
            }
            if (produced < cap) {
                out[produced] = (int16_t)s;
            }
            ++produced;
        }
    }
    return produced > cap ? cap : produced;
}

/* ---------------- 8 kHz -> 16 kHz（2× 线性插值） ---------------- */

void resampler_8to16_init(struct resampler_8to16 *r)
{
    if (r == NULL) {
        return;
    }
    r->prev = 0;
    r->have = 0u;
}

uint32_t resampler_8to16_run(struct resampler_8to16 *r,
                             const int16_t *in, uint32_t n,
                             int16_t *out, uint32_t cap)
{
    uint32_t produced = 0u;

    if (r == NULL || in == NULL || out == NULL) {
        return 0u;
    }
    for (uint32_t i = 0; i < n; ++i) {
        int16_t x = in[i];

        if (!r->have) {
            /* 首样本补一次，保证输出样本数恰为输入的 2 倍 */
            r->prev = x;
            r->have = 1u;
            if (produced < cap) out[produced] = x;
            ++produced;
            if (produced < cap) out[produced] = x;
            ++produced;
        } else {
            int32_t mid = ((int32_t)r->prev + (int32_t)x) / 2;

            if (produced < cap) out[produced] = (int16_t)mid;
            ++produced;
            if (produced < cap) out[produced] = x;
            ++produced;
            r->prev = x;
        }
    }
    return produced > cap ? cap : produced;
}
