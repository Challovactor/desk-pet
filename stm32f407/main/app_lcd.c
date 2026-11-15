/*
 * 2.0" ST7789 240×320 RGB565 彩色屏驱动（SPI3 与墨水屏共享）。
 *
 * 初始化序列取自工作区 "触摸屏可行代码"（ST7789：SLPOUT → COLMOD=0x55
 * → MADCTL=0x00 → INVON → DISPON），引脚：
 *   SCK=PC10 / MOSI=PC12（AF6，仅发送）、CS=PC11、DC=PC13、
 *   RESET=PA11、背光 PD14（高有效）。
 *
 * 开机图是 tools/build_lcd_logo.py 生成的 RLE 数据（每段 3 字节：
 * RGB565 小端 + 重复次数），解码后按 512 字节块流式写入 GRAM。
 * 与墨水屏的 CS=PA15、D/C=PC3 不冲突；SPI3 由本模块重新初始化。
 */
#include "app_lcd.h"

#include <string.h>

#include "stm32f4xx_hal.h"

#define LCD_W             240u
#define LCD_H             320u
#define SPI_TIMEOUT_MS    200u

#define LCD_CS_PORT   GPIOC
#define LCD_CS_PIN    GPIO_PIN_11
#define LCD_DC_PORT   GPIOC
#define LCD_DC_PIN    GPIO_PIN_13
#define LCD_RST_PORT  GPIOA
#define LCD_RST_PIN   GPIO_PIN_11
#define LCD_BL_PORT   GPIOD
#define LCD_BL_PIN    GPIO_PIN_14

extern const unsigned char g_lcd_logo_rle[];
extern const unsigned int g_lcd_logo_rle_size;

static SPI_HandleTypeDef hspi_lcd;

static void lcd_cs(int low)  { HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, low ? GPIO_PIN_RESET : GPIO_PIN_SET); }
static void lcd_dc(int data) { HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, data ? GPIO_PIN_SET : GPIO_PIN_RESET); }

static int lcd_cmd(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    lcd_cs(1);
    lcd_dc(0);
    if (HAL_SPI_Transmit(&hspi_lcd, &cmd, 1u, SPI_TIMEOUT_MS) != HAL_OK) {
        lcd_cs(0);
        return -1;
    }
    if (len != 0u) {
        lcd_dc(1);
        if (HAL_SPI_Transmit(&hspi_lcd, (uint8_t *)data, len, SPI_TIMEOUT_MS) != HAL_OK) {
            lcd_cs(0);
            return -1;
        }
    }
    lcd_cs(0);
    return 0;
}

static int lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t cols[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0,
                             (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t rows[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0,
                             (uint8_t)(y1 >> 8), (uint8_t)y1};

    if (lcd_cmd(0x2A, cols, 4u) != 0) {
        return -1;
    }
    if (lcd_cmd(0x2B, rows, 4u) != 0) {
        return -1;
    }
    return lcd_cmd(0x2C, NULL, 0u);   /* RAMWR */
}

static void lcd_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* SPI3 AF6：SCK=PC10 / MOSI=PC12（只发） */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF6_SPI3;
    g.Pin = GPIO_PIN_10 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &g);

    /* 控制脚：CS / DC / RESET / 背光 */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = 0;
    g.Pin = LCD_CS_PIN | LCD_DC_PIN;
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = LCD_RST_PIN;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = LCD_BL_PIN;
    HAL_GPIO_Init(GPIOD, &g);

    lcd_cs(0);
    lcd_dc(1);
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, GPIO_PIN_RESET);
}

static int lcd_spi_init(void)
{
    __HAL_RCC_SPI3_CLK_ENABLE();
    memset(&hspi_lcd, 0, sizeof(hspi_lcd));
    hspi_lcd.Instance = SPI3;
    hspi_lcd.Init.Mode = SPI_MODE_MASTER;
    hspi_lcd.Init.Direction = SPI_DIRECTION_1LINE;  /* 无 MISO */
    hspi_lcd.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi_lcd.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi_lcd.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi_lcd.Init.NSS = SPI_NSS_SOFT;
    hspi_lcd.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8; /* 42MHz/8 */
    hspi_lcd.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi_lcd.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi_lcd.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    return HAL_SPI_Init(&hspi_lcd) == HAL_OK ? 0 : -1;
}

int lcd_init(void)
{
    const uint8_t colmod = 0x55;  /* RGB565 */
    const uint8_t madctl = 0x00;  /* 竖屏 240×320，正常扫描 */

    lcd_gpio_init();
    if (lcd_spi_init() != 0) {
        return -1;
    }

    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(150);

    if (lcd_cmd(0x11, NULL, 0u) != 0) {      /* SLPOUT */
        return -1;
    }
    HAL_Delay(150);
    if (lcd_cmd(0x3A, &colmod, 1u) != 0) {   /* COLMOD */
        return -1;
    }
    if (lcd_cmd(0x36, &madctl, 1u) != 0) {   /* MADCTL */
        return -1;
    }
    if (lcd_cmd(0x21, NULL, 0u) != 0) {      /* INVON */
        return -1;
    }
    return lcd_cmd(0x29, NULL, 0u);          /* DISPON */
}

int lcd_show_logo(void)
{
    uint8_t chunk[512];
    unsigned idx = 0u;

    if (g_lcd_logo_rle_size < 3u) {
        return -1;
    }
    if (lcd_set_window(0u, 0u, LCD_W - 1u, LCD_H - 1u) != 0) {
        return -1;
    }
    lcd_cs(1);
    lcd_dc(1);
    while (idx + 3u <= g_lcd_logo_rle_size) {
        uint16_t color = (uint16_t)(g_lcd_logo_rle[idx] |
                                    ((uint16_t)g_lcd_logo_rle[idx + 1u] << 8));
        unsigned count = g_lcd_logo_rle[idx + 2u];

        idx += 3u;
        while (count > 0u) {
            unsigned pixels = count > sizeof(chunk) / 2u ? sizeof(chunk) / 2u : count;
            uint8_t *p = chunk;

            for (unsigned i = 0; i < pixels; ++i) {
                *p++ = (uint8_t)(color >> 8);
                *p++ = (uint8_t)color;
            }
            if (HAL_SPI_Transmit(&hspi_lcd, chunk, (uint16_t)(pixels * 2u),
                                 SPI_TIMEOUT_MS) != HAL_OK) {
                lcd_cs(0);
                return -1;
            }
            count -= pixels;
        }
    }
    lcd_cs(0);
    /* 绘制完成再点背光，避免开机花屏 */
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, GPIO_PIN_SET);
    return 0;
}
