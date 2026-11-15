#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <stdint.h>

int audio_init(void);
int audio_deinit(void);

/* 把 I2S 的 MCLK/WS/SCK/数据脚路由到板级 MIC/SPK 连接器。
 * codec 启动配置时需要在 I2S 16 kHz 初始化后单独调用。 */
void audio_pins_apply(void);

/* 读一块 PDM 并解码为 PCM16LE 单声道。
 * 返回 PCM 样本数；负值表示读失败/超时。 */
int audio_read(int16_t *pcm, unsigned int max_samples);

/* 暂停"写 SD"（读取/解码照常，避免 I2S DMA 溢出）。
 * 上传期间调用，用来验证 SD 写+Wi-Fi 的硬件耦合问题。 */
void audio_capture_pause(int on);
int  audio_capture_paused(void);

#endif
