#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <stdint.h>

int audio_init(void);
int audio_deinit(void);

/* 读一块 PDM 并解码为 PCM16LE 单声道。
 * 返回 PCM 样本数；负值表示读失败/超时。 */
int audio_read(int16_t *pcm, unsigned int max_samples);

#endif
