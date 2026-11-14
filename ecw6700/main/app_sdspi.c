/*
 * SPI 模式 SD 卡驱动（ECW6700F，SPI1）
 *
 * 板级连线（BOARD_SPI_0 = {14,12,13}，BOARD_GPIO_4/CN13_SPI_CS = 11）：
 *   CLK  = GPIO14  (FUNC_SPI1_CLK)
 *   MOSI = GPIO12  (FUNC_SPI1_MOSI)
 *   MISO = GPIO13  (FUNC_SPI1_MISO)
 *   CS   = GPIO11  (普通 GPIO 输出，低有效)
 *
 * 注意：SDK 默认 SPI1 复用是 MISO=11 / MOSI=12 / CS=13 / CLK=14，
 * 与板级绑定不同，所以初始化后必须覆盖 pinmux（见 app_sd_pins()）。
 *
 * 传输策略：
 *   - 命令/令牌/短数据用 CPU 轮询（drv_spi_master_transmit_by_cpu）；
 *   - 512 字节块用 DMA（drv_spi_master_transmit_by_dma），避免长时间关中断。
 */
#include "app_config.h"
#include "app_sdspi.h"

#include <stddef.h>
#include <string.h>

#include "oshal.h"
#include "spi.h"
#include "gpio.h"
#include "chip_pinmux.h"

#define SD_SECTOR_SIZE      512u
#define SD_CMD_TIMEOUT_MS   500u
#define SD_TOKEN_TIMEOUT_MS 500u
#define SD_INIT_TIMEOUT_MS  3000u

#define SD_TOKEN_START_BLOCK 0xFEu
#define SD_TOKEN_START_MULTI 0xFCu
#define SD_DATA_RESP_OK      0x05u

static os_mutex_handle_t s_lock;
static os_sem_handle_t   s_spi_done;
static spi_device_interface_config_t s_cfg;
static int s_ready;
static int s_spi_inited;
static int s_block_addressed;
static uint32_t s_sectors;

static uint8_t s_tx_ff[SD_SECTOR_SIZE] __attribute__((aligned(4)));
static uint8_t s_rx_scratch[SD_SECTOR_SIZE] __attribute__((aligned(4)));
static uint8_t s_cmd_tx[8] __attribute__((aligned(4)));
static uint8_t s_cmd_rx[8] __attribute__((aligned(4)));

/* ---------------------------------------------------------------- 底层 SPI */

static unsigned s_cs_gpio = APP_SD_CS_GPIO;

static void sd_select(int on)
{
    drv_gpio_write(s_cs_gpio, on ? 0u : 1u);
}

/* 按一组候选映射配置 SPI1 四根线：CS 用 GPIO 软件片选，其余走 SPI1 复用。 */
static void sd_config_pins(unsigned cs, unsigned sck, unsigned mosi, unsigned miso)
{
    T_GPIO_CONFIG cs_cfg;

    set_pin_function(cs, FUNC_GPIO);
    /* drv_gpio_open() 是全工程唯一使能 CLK_GPIO 的入口；不调用它，
     * drv_gpio_write/set_dir 对 CS 无效。 */
    memset(&cs_cfg, 0, sizeof(cs_cfg));
    cs_cfg.num = (unsigned char)cs;
    cs_cfg.dir = DRV_GPIO_ARG_DIR_OUT;
    cs_cfg.intr_type = DRV_GPIO_ARG_INTR_MODE_NOP;
    cs_cfg.pull_type = DRV_GPIO_ARG_PULL_UP;
    if (drv_gpio_open(&cs_cfg) != GPIO_RET_SUCCESS) {
        os_printf(LL_ERR, "[sd] gpio open CS=%u failed\r\n", cs);
    }
    drv_gpio_write(cs, 1u);

    set_pin_function(sck, FUNC_SPI1_CLK);
    set_pin_function(mosi, FUNC_SPI1_MOSI);
    set_pin_function(miso, FUNC_SPI1_MISO);

    chip_io_drive_current(sck, CHIP_IO_DRIVE_CURRENT_2mA);
    chip_io_drive_current(mosi, CHIP_IO_DRIVE_CURRENT_2mA);
    chip_io_drive_current(miso, CHIP_IO_DRIVE_CURRENT_2mA);

    s_cs_gpio = cs;
}

static void spi_done_cb(void *arg)
{
    (void)arg;
    if (s_spi_done) {
        os_sem_post(s_spi_done);
    }
}

static int spi_xfer(const uint8_t *tx, uint8_t *rx, unsigned int len)
{
    spi_trans_t tr;
    unsigned int xlen;
    int rc;

    if (len == 0u) {
        return 0;
    }
    memset(&tr, 0, sizeof(tr));
    tr.txlength = len;
    tr.rxlength = len;
    tr.tx_buffer = (unsigned int *)tx;
    tr.rx_buffer = (unsigned int *)rx;
    tr.config = &s_cfg;

    if (len <= 32u) {
        /* CPU 路径只识别 TX/RX/TX_RX/RX_TX；FULL_DUPLEX 位在 by_cpu 里
         * 没有分支，会静默变成空操作。 */
        tr.flags = SPI_TRANS_MODE_TX_RX | SPI_TRANS_VARIABLE_CONFIG;
        rc = drv_spi_master_transmit_by_cpu(E_SPI_NUM_1, &tr);
        return rc == SPI_RET_SUCCESS ? 0 : -1;
    }

    xlen = (len + 3u) & ~3u;
    memset(&tr, 0, sizeof(tr));
    tr.flags = SPI_TRANS_MODE_FULL_DUPLEX | SPI_TRANS_VARIABLE_CONFIG;
    tr.txlength = xlen;
    tr.rxlength = xlen;
    tr.tx_buffer = (unsigned int *)tx;
    tr.rx_buffer = (unsigned int *)rx;
    tr.config = &s_cfg;

    while (os_sem_wait(s_spi_done, 0) == 0) {
        /* drain */
    }
    tr.callback.spi_callback = spi_done_cb;
    tr.callback.spi_data = NULL;
    rc = drv_spi_master_transmit_by_dma(E_SPI_NUM_1, &tr);
    if (rc != SPI_RET_SUCCESS) {
        return -1;
    }
    if (os_sem_wait(s_spi_done, 1000) != 0) {
        return -1;
    }
    return 0;
}

static uint8_t spi_byte(uint8_t out)
{
    uint8_t tx[4] __attribute__((aligned(4))) = {out, 0xFFu, 0xFFu, 0xFFu};
    uint8_t rx[4] __attribute__((aligned(4))) = {0xFFu, 0xFFu, 0xFFu, 0xFFu};

    if (spi_xfer(tx, rx, 1u) != 0) {
        return 0xFFu;
    }
    return rx[0];
}

static int spi_tx_block(const uint8_t *data, unsigned int len)
{
    return spi_xfer(data, s_rx_scratch, len);
}

static int spi_rx_block(uint8_t *data, unsigned int len)
{
    return spi_xfer(s_tx_ff, data, len);
}

/* -------------------------------------------------------------------- CRC */

static uint8_t sd_crc7(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t d = p[i];
        for (unsigned b = 0; b < 8u; ++b) {
            crc = (uint8_t)(crc << 1);
            if ((d ^ crc) & 0x80u) {
                crc ^= 0x09u;
            }
            d = (uint8_t)(d << 1);
        }
    }
    return (uint8_t)((crc << 1) | 1u);
}

static uint16_t sd_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (unsigned b = 0; b < 8u; ++b) {
            crc = (uint16_t)((crc & 0x8000u) ? ((uint32_t)crc << 1) ^ 0x1021u
                                             : (uint32_t)crc << 1);
        }
    }
    return crc;
}

/* --------------------------------------------------------------- 命令层 */

static uint8_t sd_cmd(uint8_t index, uint32_t arg, int keep_selected)
{
    uint8_t r1 = 0xFFu;

    memset(s_cmd_tx, 0xFF, sizeof(s_cmd_tx));
    s_cmd_tx[0] = (uint8_t)(0x40u | index);
    s_cmd_tx[1] = (uint8_t)(arg >> 24);
    s_cmd_tx[2] = (uint8_t)(arg >> 16);
    s_cmd_tx[3] = (uint8_t)(arg >> 8);
    s_cmd_tx[4] = (uint8_t)arg;
    s_cmd_tx[5] = sd_crc7(s_cmd_tx, 5);

    sd_select(1);
    (void)spi_byte(0xFFu);
    (void)spi_xfer(s_cmd_tx, s_cmd_rx, 6u);

    for (unsigned i = 0; i < 16u; ++i) {
        r1 = spi_byte(0xFFu);
        if ((r1 & 0x80u) == 0u) {
            break;
        }
    }
    if (!keep_selected) {
        sd_select(0);
        (void)spi_byte(0xFFu);
    }
    return r1;
}

static int sd_wait_token(uint8_t token, uint32_t timeout_ms)
{
    long long start = os_time_get();
    for (;;) {
        uint8_t b = spi_byte(0xFFu);
        if (b == token) {
            return 0;
        }
        if (b != 0xFFu) {
            /* 数据错误令牌（bit0..3 全 0）或非法字节 */
            if ((b & 0xF0u) == 0u) {
                return -2;
            }
        }
        if ((unsigned long long)(os_time_get() - start) >= timeout_ms) {
            return -1;
        }
    }
}

static int sd_wait_ready(uint32_t timeout_ms)
{
    long long start = os_time_get();
    for (;;) {
        if (spi_byte(0xFFu) == 0xFFu) {
            return 0;
        }
        if ((unsigned long long)(os_time_get() - start) >= timeout_ms) {
            return -1;
        }
    }
}

/* --------------------------------------------------------------- 初始化 */

static int sd_read_csd(uint8_t csd[16])
{
    uint8_t r1 = sd_cmd(9u, 0u, 1);
    os_printf(LL_INFO, "[sd] CMD9 r1=0x%02x\r\n", r1);
    if (r1 != 0u) {
        sd_select(0);
        (void)spi_byte(0xFFu);
        return -1;
    }
    if (sd_wait_token(SD_TOKEN_START_BLOCK, SD_TOKEN_TIMEOUT_MS) != 0) {
        sd_select(0);
        (void)spi_byte(0xFFu);
        return -1;
    }
    if (spi_rx_block(csd, 16u) != 0) {
        sd_select(0);
        (void)spi_byte(0xFFu);
        return -1;
    }
    (void)spi_byte(0xFFu);
    (void)spi_byte(0xFFu);
    sd_select(0);
    (void)spi_byte(0xFFu);
    return 0;
}

static int sd_read_ocr(uint32_t *ocr)
{
    uint8_t r1 = sd_cmd(58u, 0u, 1);
    uint8_t buf[4] __attribute__((aligned(4)));

    if (r1 != 0u) {
        sd_select(0);
        (void)spi_byte(0xFFu);
        return -1;
    }
    if (spi_rx_block(buf, 4u) != 0) {
        sd_select(0);
        (void)spi_byte(0xFFu);
        return -1;
    }
    sd_select(0);
    (void)spi_byte(0xFFu);
    *ocr = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | buf[3];
    return 0;
}

int app_sd_init(void)
{
    uint8_t r1;
    int v2 = 0;
    uint32_t ocr = 0;

    if (s_ready) {
        return 0;
    }
    if (s_lock == NULL) {
        s_lock = os_mutex_create();
        if (s_lock == NULL) {
            return -1;
        }
    }
    if (s_spi_done == NULL) {
        s_spi_done = os_sem_create(1, 0);
        if (s_spi_done == NULL) {
            return -1;
        }
    }

    if (!s_spi_inited) {
        if (drv_spi_master_init(E_SPI_NUM_1, 1) != 0) {
            os_printf(LL_ERR, "[sd] spi init failed\r\n");
            return -1;
        }
        /* init 成功即标记，避免后续步骤失败重试时二次 init 返回 EBUSY */
        s_spi_inited = 1;

        /* 初始化时钟：AHB 80MHz / (2*(255+1)) = 156kHz，满足 SD 卡 ≤400kHz */
        memset(&s_cfg, 0, sizeof(s_cfg));
        s_cfg.flags = SPI_DEVICE_CLK_AHB;
        s_cfg.command_bits = SPI_COMMAND_BIT0;
        s_cfg.address_bits = 0;
        s_cfg.dummy_bits = 0;
        s_cfg.trans_mode = SPI_TRAMODE_WRCON;
        s_cfg.mode = 1;   /* 实测 mode0 下 RX 晚 1 bit；试 CPHA 相反 */
        s_cfg.sclk_div = 255;

        /* drv_spi_master_init 只做复位/时钟/引脚，不配置时序与 SPI 模式；
         * 必须显式调用 transmit_config，否则 sclk_div 保持默认值（过快）。 */
        if (spi_master_transmit_config(E_SPI_NUM_1, &s_cfg) != SPI_RET_SUCCESS) {
            os_printf(LL_ERR, "[sd] spi config failed\r\n");
            return -1;
        }
    } else {
        /* 重试时只重做卡协议，回到慢速时钟 */
        s_cfg.sclk_div = 255;
        (void)spi_master_transmit_config(E_SPI_NUM_1, &s_cfg);
    }

    /* 板级端口表没有给出信号顺序，而数据手册允许 SPI1 复用到任意 GPIO0-24。
     * 依次尝试 4 种最可能的机械连接，CMD0 返回 0x01 即命中。 */
    {
        static const struct {
            unsigned cs, sck, mosi, miso;
            const char *name;
        } maps[] = {
            {APP_SD_CS_GPIO, APP_SD_CLK_GPIO, APP_SD_MOSI_GPIO, APP_SD_MISO_GPIO,
             "board: CS11 SCK14 MOSI13 MISO12"},
            {APP_SD_CS_GPIO, APP_SD_CLK_GPIO, APP_SD_MISO_GPIO, APP_SD_MOSI_GPIO,
             "board: CS11 SCK14 MOSI12 MISO13"},
            {13u, 14u, 12u, 11u, "vendor: CS13 SCK14 MOSI12 MISO11"},
            {13u, 14u, 11u, 12u, "vendor: CS13 SCK14 MOSI11 MISO12"},
        };
        int ok = 0;

        memset(s_tx_ff, 0xFF, sizeof(s_tx_ff));
        for (unsigned m = 0; m < sizeof(maps) / sizeof(maps[0]) && !ok; ++m) {
            sd_config_pins(maps[m].cs, maps[m].sck, maps[m].mosi, maps[m].miso);
            sd_select(0);
            for (unsigned i = 0; i < 12u; ++i) {
                (void)spi_byte(0xFFu);
            }
            r1 = 0xFFu;
            for (unsigned i = 0; i < 20u; ++i) {
                r1 = sd_cmd(0u, 0u, 0);
                if (r1 == 0x01u || r1 == 0x00u) {
                    break;
                }
                os_msleep(10);
            }
            /* CMD0 标准应答是 0x01（idle）；部分卡/已初始化状态会给 0x00。
             * 两者都说明卡已经在 MISO 上应答，先接受再继续后续命令。 */
            if (r1 == 0x01u || r1 == 0x00u) {
                os_printf(LL_INFO, "[sd] CMD0 ok with %s (r1=0x%02x)\r\n",
                          maps[m].name, r1);
                ok = 1;
            } else {
                os_printf(LL_WARN, "[sd] %s: CMD0 r1=0x%02x\r\n", maps[m].name, r1);
            }
        }
        if (!ok) {
            os_printf(LL_ERR, "[sd] CMD0 failed for all pin maps (check card/CS/power)\r\n");
            return -1;
        }
    }

    /* CMD8：区分 v1/v2，并校验 2.7-3.6V */
    r1 = sd_cmd(8u, 0x1AAu, 1);
    os_printf(LL_INFO, "[sd] CMD8 r1=0x%02x\r\n", r1);
    if (r1 == 0x01u || r1 == 0x00u) {
        uint8_t r7[4] __attribute__((aligned(4)));
        if (spi_rx_block(r7, 4u) != 0) {
            sd_select(0);
            (void)spi_byte(0xFFu);
            return -1;
        }
        sd_select(0);
        (void)spi_byte(0xFFu);
        os_printf(LL_INFO, "[sd] R7=%02x %02x %02x %02x\r\n",
                  r7[0], r7[1], r7[2], r7[3]);
        if (r7[2] == 0x01u && r7[3] == 0xAAu) {
            v2 = 1;
        }
    } else if (r1 & 0x04u) {
        /* 非法命令：v1 卡 */
        sd_select(0);
        (void)spi_byte(0xFFu);
        v2 = 0;
    } else {
        sd_select(0);
        (void)spi_byte(0xFFu);
        if (r1 != 0x01u) {
            os_printf(LL_ERR, "[sd] CMD8 failed r1=0x%02x\r\n", r1);
            return -1;
        }
    }

    /* ACMD41：等待初始化完成 */
    {
        long long start = os_time_get();
        uint32_t arg = v2 ? 0x40000000u : 0u;
        int ok = 0;

        while ((unsigned long long)(os_time_get() - start) < SD_INIT_TIMEOUT_MS) {
            if (sd_cmd(55u, 0u, 0) > 1u) {
                break;
            }
            r1 = sd_cmd(41u, arg, 0);
            if (r1 == 0u) {
                ok = 1;
                break;
            }
            os_msleep(2);
        }
        if (!ok) {
            os_printf(LL_ERR, "[sd] ACMD41 timeout, last r1=0x%02x\r\n", r1);
            return -1;
        }
        os_printf(LL_INFO, "[sd] ACMD41 ok (v2=%d)\r\n", v2);
    }

    if (sd_read_ocr(&ocr) != 0) {
        os_printf(LL_ERR, "[sd] CMD58 failed\r\n");
        return -1;
    }
    os_printf(LL_INFO, "[sd] OCR=0x%08lx\r\n", (unsigned long)ocr);
    s_block_addressed = (v2 && (ocr & 0x40000000u)) ? 1 : 0;

    if (!s_block_addressed) {
        r1 = sd_cmd(16u, SD_SECTOR_SIZE, 0);
        if (r1 != 0u) {
            os_printf(LL_ERR, "[sd] CMD16 failed r1=0x%02x\r\n", r1);
            return -1;
        }
    }

    /* CSD 解析容量 */
    {
        uint8_t csd[16] __attribute__((aligned(4)));
        if (sd_read_csd(csd) != 0) {
            os_printf(LL_ERR, "[sd] CMD9 failed\r\n");
            return -1;
        }
        unsigned structure = csd[0] >> 6;
        if (structure == 1u && s_block_addressed) {
            uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) |
                              ((uint32_t)csd[8] << 8) | csd[9];
            s_sectors = (c_size + 1u) * 1024u;
        } else if (structure == 0u) {
            unsigned read_bl_len = csd[5] & 0x0Fu;
            uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) |
                              ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
            unsigned c_size_mult = ((csd[9] & 0x03u) << 1) | (csd[10] >> 7);
            if (read_bl_len < 9u || read_bl_len > 11u) {
                os_printf(LL_ERR, "[sd] bad READ_BL_LEN=%u\r\n", read_bl_len);
                return -1;
            }
            s_sectors = (c_size + 1u) << (c_size_mult + 2u + read_bl_len - 9u);
        } else {
            os_printf(LL_ERR, "[sd] unsupported CSD structure=%u\r\n", structure);
            return -1;
        }
    }

    if (s_sectors == 0u) {
        os_printf(LL_ERR, "[sd] zero capacity\r\n");
        return -1;
    }

    /* 提速：80MHz / (2*(3+1)) = 10MHz（SPI 模式下 SD 卡通常支持 25MHz） */
    s_cfg.sclk_div = 3;
    if (spi_master_transmit_config(E_SPI_NUM_1, &s_cfg) != SPI_RET_SUCCESS) {
        os_printf(LL_ERR, "[sd] spi fast config failed\r\n");
        return -1;
    }

    s_ready = 1;
    os_printf(LL_INFO, "[sd] ready: %lu sectors, %s addressing\r\n",
              (unsigned long)s_sectors,
              s_block_addressed ? "block" : "byte");
    return 0;
}

int app_sd_is_ready(void) { return s_ready; }
uint32_t app_sd_sector_count(void) { return s_sectors; }
uint32_t app_sd_sector_size(void) { return SD_SECTOR_SIZE; }

/* --------------------------------------------------------------- 块读写 */

int app_sd_read_blocks(uint32_t lba, void *dst, uint32_t count)
{
    uint8_t *out = (uint8_t *)dst;

    if (!s_ready || dst == NULL || count == 0u) {
        return -1;
    }
    if ((uint64_t)lba + count > s_sectors) {
        return -1;
    }
    if (os_mutex_lock(s_lock, 2000) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i, ++lba, out += SD_SECTOR_SIZE) {
        uint32_t addr = s_block_addressed ? lba : lba * SD_SECTOR_SIZE;
        uint8_t r1 = sd_cmd(17u, addr, 1);
        if (r1 != 0u) {
            os_printf(LL_ERR, "[sd] CMD17 r1=0x%02x lba=%lu\r\n", r1, (unsigned long)lba);
            goto fail;
        }
        if (sd_wait_token(SD_TOKEN_START_BLOCK, SD_TOKEN_TIMEOUT_MS) != 0) {
            os_printf(LL_ERR, "[sd] read token timeout lba=%lu\r\n", (unsigned long)lba);
            goto fail;
        }
        if (spi_rx_block(out, SD_SECTOR_SIZE) != 0) {
            os_printf(LL_ERR, "[sd] block read failed lba=%lu\r\n", (unsigned long)lba);
            goto fail;
        }
        {
            /* CPU 路径按 32-bit 字访问，缓冲必须是 4 字节 */
            uint8_t crc[4] __attribute__((aligned(4)));
            if (spi_rx_block(crc, 2u) != 0) {
                goto fail;
            }
        }
        sd_select(0);
        (void)spi_byte(0xFFu);
    }
    os_mutex_unlock(s_lock);
    return 0;

fail:
    sd_select(0);
    (void)spi_byte(0xFFu);
    os_mutex_unlock(s_lock);
    return -1;
}

int app_sd_write_blocks(uint32_t lba, const void *src, uint32_t count)
{
    const uint8_t *in = (const uint8_t *)src;

    if (!s_ready || src == NULL || count == 0u) {
        return -1;
    }
    if ((uint64_t)lba + count > s_sectors) {
        return -1;
    }
    if (os_mutex_lock(s_lock, 2000) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i, ++lba, in += SD_SECTOR_SIZE) {
        uint32_t addr = s_block_addressed ? lba : lba * SD_SECTOR_SIZE;
        uint8_t r1 = sd_cmd(24u, addr, 1);
        if (r1 != 0u) {
            os_printf(LL_ERR, "[sd] CMD24 r1=0x%02x lba=%lu\r\n", r1, (unsigned long)lba);
            goto fail;
        }
        (void)spi_byte(0xFFu);
        (void)spi_byte(SD_TOKEN_START_BLOCK);
        if (spi_tx_block(in, SD_SECTOR_SIZE) != 0) {
            os_printf(LL_ERR, "[sd] block write failed lba=%lu\r\n", (unsigned long)lba);
            goto fail;
        }
        {
            uint16_t crc = sd_crc16(in, SD_SECTOR_SIZE);
            (void)spi_byte((uint8_t)(crc >> 8));
            (void)spi_byte((uint8_t)crc);
        }
        /* 数据响应：xxx0_0101；卡可能先返回若干 0xFF */
        {
            uint8_t resp = 0xFFu;
            for (unsigned k = 0; k < 8u; ++k) {
                resp = spi_byte(0xFFu);
                if (resp != 0xFFu) {
                    break;
                }
            }
            if ((resp & 0x1Fu) != SD_DATA_RESP_OK) {
                os_printf(LL_ERR, "[sd] write resp=0x%02x lba=%lu\r\n",
                          resp, (unsigned long)lba);
                goto fail;
            }
        }
        if (sd_wait_ready(SD_CMD_TIMEOUT_MS) != 0) {
            os_printf(LL_ERR, "[sd] write busy timeout lba=%lu\r\n", (unsigned long)lba);
            goto fail;
        }
        sd_select(0);
        (void)spi_byte(0xFFu);
        (void)spi_byte(0xFFu);
    }
    os_mutex_unlock(s_lock);
    return 0;

fail:
    sd_select(0);
    for (unsigned i = 0; i < 2u; ++i) {
        (void)spi_byte(0xFFu);
    }
    os_mutex_unlock(s_lock);
    return -1;
}
