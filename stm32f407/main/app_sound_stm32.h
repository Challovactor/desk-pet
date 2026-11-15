#ifndef APP_SOUND_STM32_H
#define APP_SOUND_STM32_H

#include <stdint.h>

/*
 * 情绪音效本地播放（STM32F407 I2S2 + DMA）。
 *
 * 喇叭模块接在 STM32 SHIELD 的 SPK 口：PB12=I2S_WS、PB13=I2S_SCK、
 * PB15=I2S_SD（SPI2/I2S2，AF5）。音效数据由
 * tools/build_stm32_sounds.py 从 ECW 的 animal_sounds.bin 生成为 C 数组，
 * 直接放在 flash，播放不经过 ECW。
 *
 * 命令映射与 ECW 侧一致：
 *   0x11/0x13 -> positive；0x12/0x14 -> negative；0x01/其它 -> 全随机。
 */

/* 上电初始化 I2S2（16 kHz、主发送、飞利浦格式，DMA 播放）。 */
int sound_stm_init(void);

/* 按命令随机选一个音效并阻塞播放；返回 0 成功，-1 失败/无资产。 */
int sound_stm_play_cmd(uint8_t cmd);

#endif
