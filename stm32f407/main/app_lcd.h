#ifndef APP_LCD_H
#define APP_LCD_H

/*
 * 2.0" ST7789 彩色屏（240×320，RGB565），与墨水屏共用 SPI3。
 *
 * 接线（2.0TP 模组）：
 *   SCK=PC10 / MOSI=PC12（SPI3 AF6，只发）
 *   CS=PC11、DC=PC13、RESET=PA11、背光=PD14（高有效）
 *
 * 开机显示 tools/build_lcd_logo.py 生成的彩色 LOGO（RLE 压缩在 flash）。
 * 与触摸功能互不影响：触摸走 I2C3，LCD 走 SPI3。
 */

/* 初始化屏幕并点亮背光；返回 0 成功。 */
int lcd_init(void);

/* 全屏绘制彩色 LOGO（RLE 解码 + SPI 流式发送，约几十毫秒）。 */
int lcd_show_logo(void);

#endif
