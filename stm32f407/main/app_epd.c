/*
 * 4.2" 墨水屏驱动（UC8176/UC8276，微雪 4.2" V2 兼容 COF，400×300 黑白）
 *
 * 4.2EPD 接口（docs/interface/e-lnk.png，8P 晶圆座，无 MISO 只发不收）：
 *   BUSY=PD8（高=忙）、RES=PA13、D/C=PC13（与 2.0TP 模组 LCD 的 WR 共用）、
 *   SPI_CS=PA15（SPI_CS0；LCD 用 SPI_CS1=PC11）、
 *   SPI_SCK=PC10 / SPI_MOSI=PC12（与 LCD 共用 SPI3，AF6）。
 *   模块自带升压电路（SI1308EDL），VCI/VDD 由 3V3 供电。
 *
 * 初始化序列取自微雪官方 EPD_4in2_V2 驱动（LUT 走 OTP）：
 *   SWRESET(0x12) -> 0x21=0x40,0x00 -> 0x3C=0x05 -> 0x11=0x03
 *   -> 窗口 0x44/0x45、游标 0x4E/0x4F
 * 刷屏：0x24 写新数据 -> 0x26 写相同数据 -> 0x22=0xF7 -> 0x20 触发 -> 等 BUSY
 * 睡眠：0x10=0x01（深度睡眠，再次刷新需硬件复位）
 *
 * 注意：RES 用了 PA13（SWDIO），烧录走 UART Bootloader，不受影响；
 *       但 SWD 调试期间该脚会被调试器占用，属预期现象。
 */
#include "stm32f4xx_hal.h"
#include "app_epd.h"

#include <stdint.h>

/* 400×300 / 8 = 15000 字节，MSB 在前，1=白 0=黑，行优先 */
#define EPD_WIDTH            400u
#define EPD_HEIGHT           300u
#define EPD_ROW_BYTES        (EPD_WIDTH / 8u)
#define EPD_FRAME_BYTES      (EPD_ROW_BYTES * EPD_HEIGHT)

extern const unsigned char epd_logo_bits[EPD_FRAME_BYTES];

/* ----------------------------------------------------------------- 引脚 */
#define EPD_CS_PORT   GPIOA
#define EPD_CS_PIN    GPIO_PIN_15
#define EPD_RST_PORT  GPIOA
#define EPD_RST_PIN   GPIO_PIN_13
#define EPD_DC_PORT   GPIOC
#define EPD_DC_PIN    GPIO_PIN_13
#define EPD_BUSY_PORT GPIOD
#define EPD_BUSY_PIN  GPIO_PIN_8

/* BUSY 超时：全刷典型 3–5s，给足余量 */
#define EPD_BUSY_TIMEOUT_MS 20000u

static SPI_HandleTypeDef hspi3;

/* ------------------------------------------------------------- 底层收发 */
static void epd_cmd(uint8_t c)
{
    uint8_t b = c;

    HAL_GPIO_WritePin(EPD_DC_PORT, EPD_DC_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_RESET);
    (void)HAL_SPI_Transmit(&hspi3, &b, 1, 100u);
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(EPD_DC_PORT, EPD_DC_PIN, GPIO_PIN_SET);
}

static void epd_data(const uint8_t *d, uint16_t len)
{
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_RESET);
    (void)HAL_SPI_Transmit(&hspi3, (uint8_t *)d, len, 2000u);
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_SET);
}

static void epd_data_byte(uint8_t b)
{
    epd_data(&b, 1u);
}

static int epd_wait_busy(void)
{
    uint32_t start = HAL_GetTick();

    while (HAL_GPIO_ReadPin(EPD_BUSY_PORT, EPD_BUSY_PIN) == GPIO_PIN_SET) {
        if ((HAL_GetTick() - start) >= EPD_BUSY_TIMEOUT_MS) {
            return -1;
        }
        HAL_Delay(10);
    }
    return 0;
}

static void epd_hw_reset(void)
{
    HAL_GPIO_WritePin(EPD_RST_PORT, EPD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(EPD_RST_PORT, EPD_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(2);
    HAL_GPIO_WritePin(EPD_RST_PORT, EPD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(100);
}

/* ------------------------------------------------------------ 初始化 */
static void epd_gpio_spi_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* CS / RST：推挽输出，空闲高 */
    g.Pin = EPD_CS_PIN | EPD_RST_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(EPD_RST_PORT, EPD_RST_PIN, GPIO_PIN_SET);

    /* D/C：推挽输出，空闲高（数据态） */
    g.Pin = EPD_DC_PIN;
    HAL_GPIO_Init(GPIOC, &g);
    HAL_GPIO_WritePin(EPD_DC_PORT, EPD_DC_PIN, GPIO_PIN_SET);

    /* BUSY：输入 */
    g.Pin = EPD_BUSY_PIN;
    g.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOD, &g);

    /* SPI3 SCK=PC10 / MOSI=PC12，AF6；与 LCD 共总线，EPD 独占 CS */
    g.Pin = GPIO_PIN_10 | GPIO_PIN_12;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_MEDIUM;
    g.Alternate = GPIO_AF6_SPI3;
    HAL_GPIO_Init(GPIOC, &g);

    __HAL_RCC_SPI3_CLK_ENABLE();
    hspi3.Instance = SPI3;
    hspi3.Init.Mode = SPI_MODE_MASTER;
    hspi3.Init.Direction = SPI_DIRECTION_1LINE; /* 无 MISO，只发 */
    hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi3.Init.NSS = SPI_NSS_SOFT;
    hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16; /* 42/16 ≈ 2.6MHz */
    hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi3.Init.CRCPolynomial = 10;
    (void)HAL_SPI_Init(&hspi3);
}

int epd_init(void)
{
    static const uint8_t win_x[2] = {0x00, EPD_ROW_BYTES - 1u};   /* 0..49（字节） */
    static const uint8_t win_y[4] = {0x00, 0x00, 0x2B, 0x01};     /* 0..299 */
    static const uint8_t cur_x[1] = {0x00};
    static const uint8_t cur_y[2] = {0x00, 0x00};
    static const uint8_t updc[2] = {0x40, 0x00};

    epd_gpio_spi_init();
    epd_hw_reset();

    if (epd_wait_busy() != 0) {
        return -1;
    }
    epd_cmd(0x12); /* 软复位 */
    if (epd_wait_busy() != 0) {
        return -1;
    }

    epd_cmd(0x21); /* Display update control */
    epd_data(updc, sizeof(updc));
    epd_cmd(0x3C); /* Border waveform */
    epd_data_byte(0x05);
    epd_cmd(0x11); /* Data entry mode：X 增、Y 增 */
    epd_data_byte(0x03);

    epd_cmd(0x44); /* X 窗口（字节单位） */
    epd_data(win_x, sizeof(win_x));
    epd_cmd(0x45); /* Y 窗口 */
    epd_data(win_y, sizeof(win_y));
    epd_cmd(0x4E); /* X 游标 */
    epd_data(cur_x, sizeof(cur_x));
    epd_cmd(0x4F); /* Y 游标 */
    epd_data(cur_y, sizeof(cur_y));

    if (epd_wait_busy() != 0) {
        return -1;
    }
    return 0;
}

int epd_show_logo(void)
{
    int ret = 0;

    epd_cmd(0x24); /* 新数据 RAM */
    epd_data(epd_logo_bits, EPD_FRAME_BYTES);
    epd_cmd(0x26); /* 旧数据 RAM 写相同内容（全刷需要） */
    epd_data(epd_logo_bits, EPD_FRAME_BYTES);

    epd_cmd(0x22);
    epd_data_byte(0xF7); /* 全刷 */
    epd_cmd(0x20);       /* 触发 */
    if (epd_wait_busy() != 0) {
        ret = -1;
    }

    epd_cmd(0x10); /* 深度睡眠 */
    epd_data_byte(0x01);
    return ret;
}
