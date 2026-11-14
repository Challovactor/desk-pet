#ifndef APP_EPD_H
#define APP_EPD_H

/*
 * 4.2" 墨水屏（4.2EPD 接口，UC8176/UC8276 COF 驱动，微雪 4.2 V2 兼容）。
 * 开机全刷显示一次 epd_logo_bits（tools/gen_epd_logo.py 从 logo.png 生成），
 * 显示完进入深度睡眠；失败不阻塞主流程。
 */

/* 0 = 成功；-1 = SPI/BUSY 异常（屏未接或时序失败） */
int epd_init(void);
int epd_show_logo(void);

#endif
