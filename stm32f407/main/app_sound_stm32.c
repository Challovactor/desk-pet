/*
 * STM32F407 本地音效播放：I2S2（SPI2）主发送 + DMA1 Stream4。
 *
 * 硬件：STM32F407_SHIELD 的 SPK 口
 *   PB12 = I2S_WS、PB13 = I2S_SCK、PB15 = I2S_SD（AF5）
 * 数据：g_sounds_blob[]（由 tools/build_stm32_sounds.py 生成，格式与
 *       ECW 的 animal_sounds.bin 相同：32 字节头 + 每项 40 字节目录）。
 *
 * 播放采用 DMA 直接读 flash；16 kHz、16-bit、单声道（I2S_DATAFORMAT_16B）。
 * 如果模块要求立体声帧，可改成 16B_EXTENDED 并做 L/R 复制（见注释）。
 */
#include "app_sound_stm32.h"

#include <string.h>

#include "stm32f4xx_hal.h"

#define SND_MAGIC        "MOCESND1"
#define SND_VERSION      1u
#define SND_HEADER_SIZE  32u
#define SND_ENTRY_SIZE   40u
#define SND_MAX_ENTRIES  8u

#define SND_CLASS_NEG    1u
#define SND_CLASS_POS    2u

extern const unsigned char g_sounds_blob[];
extern const unsigned int g_sounds_blob_size;

struct snd_entry {
    char name[16];
    uint8_t class_id;
    const uint8_t *data;
    uint32_t bytes;
};

static I2S_HandleTypeDef hi2s2;
static DMA_HandleTypeDef hdma_i2s2_tx;
static struct snd_entry s_entries[SND_MAX_ENTRIES];
static unsigned s_count;
static int s_ready;
static volatile int s_tx_done;
static uint32_t s_rng = 0x12345678u;

static uint32_t rd_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int blob_parse(void)
{
    const uint8_t *b = g_sounds_blob;
    uint32_t total = rd_u32(b + 24u);
    uint32_t count = rd_u16(b + 20u);

    if (g_sounds_blob_size < SND_HEADER_SIZE ||
        memcmp(b, SND_MAGIC, 8u) != 0 ||
        rd_u16(b + 8u) != SND_VERSION ||
        rd_u16(b + 10u) != SND_HEADER_SIZE ||
        count == 0u || count > SND_MAX_ENTRIES ||
        total > g_sounds_blob_size) {
        return -1;
    }
    s_count = count;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *e = b + SND_HEADER_SIZE + i * SND_ENTRY_SIZE;
        uint32_t off = rd_u32(e + 20u);
        uint32_t bytes = rd_u32(e + 24u);

        if (off > total || bytes > total - off || bytes == 0u) {
            return -1;
        }
        memcpy(s_entries[i].name, e, 16u);
        s_entries[i].name[15] = '\0';
        s_entries[i].class_id = e[16];
        s_entries[i].data = b + off;
        s_entries[i].bytes = bytes;
    }
    return 0;
}

static void i2s2_pins_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    g.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_15;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &g);
}

static int i2s2_init(void)
{
    RCC_PeriphCLKInitTypeDef pclk = {0};

    /* I2S2 时钟来自 PLLI2S；HAL 的 MspInit 是空实现，必须自己配。
     * HSE 8MHz / PLLM=8 -> 1MHz；PLLI2SN=192, PLLI2SR=2 -> I2SCLK 96MHz，
     * HAL 再按 16kHz 计算 I2SDIV。不配的话 I2S 没有时钟，TX 立即下溢。 */
    pclk.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    pclk.PLLI2S.PLLI2SN = 192u;
    pclk.PLLI2S.PLLI2SR = 2u;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        return -1;
    }

    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    i2s2_pins_init();

    hdma_i2s2_tx.Instance = DMA1_Stream4;
    hdma_i2s2_tx.Init.Channel = DMA_CHANNEL_0;
    hdma_i2s2_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_i2s2_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_i2s2_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_i2s2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_i2s2_tx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_i2s2_tx.Init.Mode = DMA_NORMAL;
    hdma_i2s2_tx.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_i2s2_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_i2s2_tx) != HAL_OK) {
        return -1;
    }
    /* DMA 传输完成中断必须在 NVIC 使能，否则完成回调永远不来 */
    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
    __HAL_LINKDMA(&hi2s2, hdmatx, hdma_i2s2_tx);

    hi2s2.Instance = SPI2;
    hi2s2.Init.Mode = I2S_MODE_MASTER_TX;
    hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
    hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B;
    hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_16K;
    hi2s2.Init.CPOL = I2S_CPOL_LOW;
    hi2s2.Init.ClockSource = I2S_CLOCK_PLL;
    hi2s2.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;
    return HAL_I2S_Init(&hi2s2) == HAL_OK ? 0 : -1;
}

int sound_stm_init(void)
{
    if (s_ready) {
        return 0;
    }
    if (blob_parse() != 0) {
        return -1;
    }
    if (i2s2_init() != 0) {
        return -1;
    }
    s_rng ^= HAL_GetTick();
    s_ready = 1;
    return 0;
}

/* HAL 的 I2S 发送完成回调（DMA 传输结束） */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        s_tx_done = 1;
    }
}

/* SPI2_TX 使用 DMA1 Stream4，必须把中断转发给 HAL，否则完成回调不来 */
void DMA1_Stream4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_i2s2_tx);
}

static int play_entry(unsigned index)
{
    uint32_t samples;
    uint32_t start;

    if (!s_ready || index >= s_count) {
        return -1;
    }
    /* DMA Size 是 16-bit 样本数；单项资产 < 65536，超长时截断 */
    samples = s_entries[index].bytes / 2u;
    if (samples > 0xFFFFu) {
        samples = 0xFFFFu;
    }
    s_tx_done = 0;
    {
        HAL_StatusTypeDef st = HAL_I2S_Transmit_DMA(
            &hi2s2, (uint16_t *)(uintptr_t)s_entries[index].data,
            (uint16_t)samples);

        if (st != HAL_OK) {
            return -2 - (int)st;   /* -2: HAL_BUSY, -3: HAL_ERROR, ... */
        }
    }
    start = HAL_GetTick();
    while (!s_tx_done && (HAL_GetTick() - start) < 8000u) {
    }
    if (!s_tx_done) {
        HAL_StatusTypeDef stop = HAL_I2S_DMAStop(&hi2s2);

        (void)stop;
        /* -20: 超时；低 4 位给出 HAL 错误码，便于区分 DMA 传输错误等 */
        return -20 - (int)(hi2s2.ErrorCode & 0x0Fu);
    }
    return 0;
}

static unsigned pick(uint8_t class_id)
{
    unsigned matches[SND_MAX_ENTRIES];
    unsigned n = 0;

    for (unsigned i = 0; i < s_count; ++i) {
        if (class_id == 0u || s_entries[i].class_id == class_id) {
            matches[n++] = i;
        }
    }
    if (n == 0u) {
        return 0xFFFFFFFFu;
    }
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return matches[s_rng % n];
}

int sound_stm_play_cmd(uint8_t cmd)
{
    uint8_t class_id = 0u;
    unsigned index;

    switch (cmd) {
    case 0x11u: /* 单击 */
    case 0x13u: /* 上滑 */
        class_id = SND_CLASS_POS;
        break;
    case 0x12u: /* 双击 */
    case 0x14u: /* 下滑 */
        class_id = SND_CLASS_NEG;
        break;
    default:    /* 0x01 普通触摸：全随机 */
        class_id = 0u;
        break;
    }
    index = pick(class_id);
    if (index == 0xFFFFFFFFu) {
        return -1;
    }
    return play_entry(index);
}
