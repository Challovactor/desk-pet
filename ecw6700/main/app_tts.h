/*
 * 文本转语音（syn6288e）串口输出。
 *
 * 参考已实机出声的 firmware-code-ecw-1115：UART0（IO17=TX / IO2=RX）
 * 9600 8N1，帧格式 0xFD 00 len 01 01 <GBK 文本> XOR。
 * 控制台日志走 UART1，不冲突；I2S 初始化会把 IO17 复用成 I2S 时钟，
 * 因此每次发送前重新把 IO17 切回 UART0_TXD。
 */
#ifndef APP_TTS_H
#define APP_TTS_H

#include <stdint.h>

int tts_init(void);
int tts_ready(void);

/* 直接发送 GBK 文本（不超过 200 字节）。 */
int tts_speak_gbk(const uint8_t *text, uint32_t len);

/* 按情绪类别随机播报一句（正向/负向各 3 句）。 */
void tts_say_emotion(int positive);

#endif /* APP_TTS_H */
