#ifndef APP_EPD_H
#define APP_EPD_H

/*
 * 墨水屏（SSD1677 系，800×480 1bit，整帧 48000 字节）。
 * SPI3 单线发送，D/C 固定低，命令/数据用 9-bit 字区分。
 * 开机全刷显示一次 epd_logo_bits（tools/build_logo_blob.py 从 LOGO.png 生成，
 * 800×480、alpha 合白底、8×8 Bayer 抖动）；失败不阻塞主流程。
 */

/* 0 = 成功；-1 = SPI/BUSY 异常（屏未接或时序失败） */
int epd_init(void);
int epd_show_logo(void);

/* 显示任意 800×480/1bpp 帧（48000 字节，MSB 在前，1=白）。
 * 失败时会自动重新 epd_init()，下一次刷新可恢复。 */
int epd_show_frame(const uint8_t *frame);

/* 诊断：上次全刷耗时（ms，真实全刷约 3000–5000；≈0 表示屏没响应）与 BUSY 电平 */
uint32_t epd_last_refresh_ms(void);
int epd_busy_level(void);

#endif
