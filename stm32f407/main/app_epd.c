/*
 * 墨水屏驱动：SSD1677 系 COF，800×480，1bit，48000 字节全屏帧。
 *
 * 与已实机点亮（整屏黑白闪烁）的 firmware-code-stm32-1115 保持一致：
 *   - SPI3 单线发送，SCK=PC10 / MOSI=PC12（AF6），CS=PA15，RES=PA13，BUSY=PD8；
 *   - D/C 不单独翻转，固定拉低，命令/数据用 9 bit 字区分
 *     （bit8=0 命令，bit8=1 数据；每字节按 MSB-first 拼进 9 bit 流）；
 *   - 初始化序列、窗口参数、更新触发（0x22=0xF7 -> 0x20）全部照搬参考实现；
 *   - 全刷后 BUSY 高电平保持约 3–4 s，随后释放。
 *
 * 注意：此前按 400×300/UC8176 + 独立 D/C 的写法在本面板上 0x20 触发后
 * BUSY 永远不释放（实测 20 s 超时），故整体替换。
 */
#include "stm32f4xx_hal.h"
#include "app_epd.h"

#include <stdint.h>
#include <string.h>

#define EPD_WIDTH            800u
#define EPD_HEIGHT           480u
#define EPD_FRAME_BYTES      (EPD_WIDTH * EPD_HEIGHT / 8u)   /* 48000 */

extern const unsigned char epd_logo_bits[EPD_FRAME_BYTES];

#define EPD_CS_PORT   GPIOA
#define EPD_CS_PIN    GPIO_PIN_15
#define EPD_RST_PORT  GPIOA
#define EPD_RST_PIN   GPIO_PIN_13
#define EPD_DC_PORT   GPIOC
#define EPD_DC_PIN    GPIO_PIN_3
#define EPD_BUSY_PORT GPIOD
#define EPD_BUSY_PIN  GPIO_PIN_8

#define EPD_XFER_TIMEOUT_MS   200u
#define EPD_BUSY_TIMEOUT_MS   8000u

static SPI_HandleTypeDef hspi3;
static uint32_t last_refresh_ms;

uint32_t epd_last_refresh_ms(void)
{
    return last_refresh_ms;
}

int epd_busy_level(void)
{
    return HAL_GPIO_ReadPin(EPD_BUSY_PORT, EPD_BUSY_PIN) == GPIO_PIN_SET;
}

/* ------------------------------------------------------------ 9-bit 拆包 */
static int epd_wait_idle(void)
{
    uint32_t start = HAL_GetTick();

    while (HAL_GPIO_ReadPin(EPD_BUSY_PORT, EPD_BUSY_PIN) == GPIO_PIN_SET) {
        if ((HAL_GetTick() - start) >= EPD_BUSY_TIMEOUT_MS) {
            return -1;
        }
        HAL_Delay(1);
    }
    return 0;
}

static int epd_command(uint8_t cmd, const uint8_t *data, size_t length, uint8_t fill)
{
    uint8_t packed[288];
    size_t prefix = (8u - ((length + 1u) % 8u)) % 8u;
    size_t words = prefix + 1u + length;
    int rc = 0;

    if (epd_wait_idle() != 0) {
        return -1;
    }
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_RESET);
    for (size_t off = 0; off < words && rc == 0;) {
        size_t count = words - off;
        if (count > 256u) {
            count = 256u;
        }
        memset(packed, 0, sizeof(packed));
        for (size_t i = 0; i < count; ++i) {
            size_t pos = off + i;
            uint16_t word;

            if (pos < prefix) {
                word = 0x7Fu;
            } else if (pos == prefix) {
                word = cmd;
            } else {
                size_t index = pos - prefix - 1u;
                word = 0x100u | (data ? data[index] : fill);
            }
            for (unsigned b = 0; b < 9u; ++b) {
                size_t bit = i * 9u + b;

                if (word & (1u << (8u - b))) {
                    packed[bit / 8u] |= (uint8_t)(0x80u >> (bit % 8u));
                }
            }
        }
        if (HAL_SPI_Transmit(&hspi3, packed, (uint16_t)(count * 9u / 8u),
                             EPD_XFER_TIMEOUT_MS) != HAL_OK) {
            rc = -1;
        }
        off += count;
    }
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_SET);
    if (rc == 0) {
        HAL_Delay(1);
    }
    return rc;
}

static int epd_settle(uint32_t delay)
{
    HAL_Delay(delay);
    return epd_wait_idle();
}

static int epd_window(void)
{
    static const uint8_t x[] = {0, 0, 0x1F, 3};
    static const uint8_t y[] = {0xDF, 1, 0, 0};
    static const uint8_t zero[] = {0, 0};
    static const uint8_t top[] = {0xDF, 1};
    int s = epd_command(0x44, x, sizeof(x), 0);

    if (s == 0) {
        s = epd_command(0x45, y, sizeof(y), 0);
    }
    if (s == 0) {
        s = epd_command(0x4E, zero, sizeof(zero), 0);
    }
    if (s == 0) {
        s = epd_command(0x4F, top, sizeof(top), 0);
    }
    return s;
}

static void epd_hw_reset(void)
{
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_SET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(EPD_RST_PORT, EPD_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(EPD_RST_PORT, EPD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(10);
}

static void epd_gpio_spi_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    g.Pin = EPD_CS_PIN | EPD_RST_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &g);
    HAL_GPIO_WritePin(EPD_CS_PORT, EPD_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(EPD_RST_PORT, EPD_RST_PIN, GPIO_PIN_SET);

    /* 9-bit 模式：D/C 固定低电平 */
    g.Pin = EPD_DC_PIN;
    HAL_GPIO_Init(GPIOC, &g);
    HAL_GPIO_WritePin(EPD_DC_PORT, EPD_DC_PIN, GPIO_PIN_RESET);

    g.Pin = EPD_BUSY_PIN;
    g.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOD, &g);

    g.Pin = GPIO_PIN_10 | GPIO_PIN_12;
    g.Mode = GPIO_MODE_AF_PP;
    g.Alternate = GPIO_AF6_SPI3;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOC, &g);

    __HAL_RCC_SPI3_CLK_ENABLE();
    memset(&hspi3, 0, sizeof(hspi3));
    hspi3.Instance = SPI3;
    hspi3.Init.Mode = SPI_MODE_MASTER;
    hspi3.Init.Direction = SPI_DIRECTION_1LINE;
    hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi3.Init.NSS = SPI_NSS_SOFT;
    hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi3.Init.CRCPolynomial = 10;
    (void)HAL_SPI_Init(&hspi3);
}

int epd_init(void)
{
    static const uint8_t gate[] = {0xDF, 0x01, 0x02};
    static const uint8_t softstart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
    static const uint8_t update[] = {0x40, 0x00};
    int s;

    epd_gpio_spi_init();
    epd_hw_reset();
    s = epd_wait_idle();
    if (s == 0) {
        s = epd_command(0x12, NULL, 0, 0);      /* soft reset */
    }
    if (s == 0) {
        s = epd_settle(10);
    }
    if (s == 0) {
        s = epd_command(0x18, NULL, 1, 0x80);   /* temperature */
    }
    if (s == 0) {
        s = epd_command(0x0C, softstart, sizeof(softstart), 0);
    }
    if (s == 0) {
        s = epd_command(0x01, gate, sizeof(gate), 0);
    }
    if (s == 0) {
        s = epd_command(0x11, NULL, 1, 0x01);   /* data entry */
    }
    if (s == 0) {
        s = epd_command(0x3C, NULL, 1, 0x01);   /* border */
    }
    if (s == 0) {
        s = epd_command(0x21, update, sizeof(update), 0);
    }
    if (s == 0) {
        s = epd_window();
    }
    return s;
}

static int epd_display(const uint8_t *frame, uint8_t fill)
{
    int s;
    uint32_t start;

    s = epd_window();
    if (s == 0) {
        s = epd_command(0x24, frame, EPD_FRAME_BYTES, fill);
    }
    if (s == 0) {
        s = epd_window();
    }
    if (s == 0) {
        s = epd_command(0x26, NULL, EPD_FRAME_BYTES, 0xFF);
    }
    if (s == 0) {
        s = epd_command(0x22, NULL, 1, 0xF7);   /* master activate */
    }
    if (s == 0) {
        start = HAL_GetTick();
        s = epd_command(0x20, NULL, 0, 0);      /* trigger */
        if (s == 0) {
            s = epd_settle(1);
        }
        last_refresh_ms = HAL_GetTick() - start;
    }
    return s;
}

int epd_show_frame(const uint8_t *frame)
{
    int ret;

    if (frame == NULL) {
        return -1;
    }
    ret = epd_display(frame, 0);
    if (ret != 0) {
        /* 一次失败后重新初始化，下一次刷新还能恢复 */
        (void)epd_init();
    }
    return ret;
}

int epd_show_logo(void)
{
    return epd_show_frame(epd_logo_bits);
}
