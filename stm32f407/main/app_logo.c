/*
 * 开机 LOGO 取图（板间 I2C 主机侧）。
 *
 * ECW 侧 app_link_serve_logo() 在收到 [0x20, off_lo, off_hi, len_lo, len_hi]
 * 后从 SD 读出对应字节并预装到从机 TX 缓冲；本机随后读 len 字节。
 * 每块都做 3 次重试，整幅图用块头里的 CRC32 校验。
 */
#include "app_logo.h"

#include <string.h>

/* main.c 里的 GPIO 总线恢复（9 个 SCL + STOP）；读事务失败后必须调用，
 * 否则从机在 TX 欠载时会一直拉住 SCL，后续所有事务都返回 HAL_BUSY。 */
extern void logo_bus_recover(void);

#define LOGO_I2C_ADDR8     (0x42u << 1)
#define LOGO_CMD_READ      0x20u
#define LOGO_HEADER_BYTES  32u
#define LOGO_MAGIC         "MOCELGO1"
#define LOGO_VERSION       1u
#define LOGO_XFER_TIMEOUT  120u   /* ms */
#define LOGO_CHUNK_DELAY   40u    /* 等 ECW 从 SD 取数并预装 */
#define LOGO_CHUNK_TRIES   5

struct logo_header_wire {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t header_size;
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t data_bytes;
    uint32_t data_crc32;
    uint32_t reserved2;
} __attribute__((packed));

static I2C_HandleTypeDef *s_hi2c;
static uint8_t s_frame[LOGO_MAX_DATA] __attribute__((aligned(4)));
static uint8_t s_seq;
/* 诊断：最近一次读事务的 HAL 状态与首字节 */
static volatile int s_dbg_tx, s_dbg_rx;
static volatile uint8_t s_dbg_resp;

static uint32_t crc32_add(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8u; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

/* 读 [off, off+len)：请求带序号，从机回 [seq, data]；序号不匹配说明读到
 * 的是过期/未就绪数据，重试。失败返回 -1。 */
static int logo_read_block(uint32_t off, uint8_t *dst, uint32_t len)
{
    uint8_t req[8];
    uint8_t resp[LOGO_CHUNK_BYTES + 1u];
    uint8_t seq;

    if (s_hi2c == NULL || len == 0u || len > LOGO_CHUNK_BYTES) {
        return -1;
    }
    seq = ++s_seq;
    req[0] = LOGO_CMD_READ;
    req[1] = 0x5Au;                       /* 帧同步 magic */
    req[2] = seq;
    req[3] = (uint8_t)(off & 0xffu);
    req[4] = (uint8_t)((off >> 8) & 0xffu);
    req[5] = (uint8_t)(len & 0xffu);
    req[6] = (uint8_t)((len >> 8) & 0xffu);
    req[7] = (uint8_t)(req[1] ^ req[2] ^ req[3] ^ req[4] ^ req[5] ^ req[6]);

    for (int attempt = 0; attempt < LOGO_CHUNK_TRIES; ++attempt) {
        s_dbg_tx = (int)HAL_I2C_Master_Transmit(s_hi2c, LOGO_I2C_ADDR8, req,
                                                sizeof(req), LOGO_XFER_TIMEOUT);
        if (s_dbg_tx != (int)HAL_OK) {
            (void)HAL_I2C_GetError(s_hi2c);
            HAL_Delay(3);
            continue;
        }
        HAL_Delay(LOGO_CHUNK_DELAY);
        s_dbg_rx = (int)HAL_I2C_Master_Receive(s_hi2c, LOGO_I2C_ADDR8, resp,
                                               (uint16_t)(len + 1u), LOGO_XFER_TIMEOUT);
        s_dbg_resp = resp[0];
        if (s_dbg_rx == (int)HAL_OK && resp[0] == seq) {
            memcpy(dst, &resp[1], len);
            return 0;
        }
        (void)HAL_I2C_GetError(s_hi2c);
        HAL_Delay(3);
    }
    logo_bus_recover();
    return -1;
}

int logo_fetch_start(struct logo_fetch *f, I2C_HandleTypeDef *hi2c)
{
    struct logo_header_wire h;

    if (f == NULL || hi2c == NULL) {
        return -1;
    }
    memset(f, 0, sizeof(*f));
    s_hi2c = hi2c;

    if (logo_read_block(0u, (uint8_t *)&h, LOGO_HEADER_BYTES) != 0) {
        return -1;
    }
    if (memcmp(h.magic, LOGO_MAGIC, 8u) != 0 ||
        h.version != LOGO_VERSION ||
        h.header_size != LOGO_HEADER_BYTES ||
        h.width != 800u || h.height != 480u ||
        h.bpp != 1u ||
        h.data_bytes != LOGO_MAX_DATA) {
        return -1;
    }

    /* 头部之后马上把位图前若干字节一起校验 CRC 会重复读；先只记录参数，
     * 数据在 logo_fetch_step 里逐块读取并累计 CRC。 */
    f->w = h.width;
    f->h = h.height;
    f->data_bytes = h.data_bytes;
    f->crc_want = h.data_crc32;
    f->crc_got = 0xFFFFFFFFu;
    f->next_off = LOGO_HEADER_BYTES;
    f->header_read = 1;
    return 0;
}

int logo_fetch_step(struct logo_fetch *f)
{
    uint32_t left;
    uint32_t n;

    if (f == NULL || !f->header_read || f->done || f->error) {
        return -1;
    }
    left = f->data_bytes - (f->next_off - LOGO_HEADER_BYTES);
    if (left > 0u) {
        n = left < LOGO_CHUNK_BYTES ? left : LOGO_CHUNK_BYTES;
        if (logo_read_block(f->next_off,
                            &s_frame[f->next_off - LOGO_HEADER_BYTES], n) != 0) {
            f->error = 1;
            return -1;
        }
        f->crc_got = crc32_add(f->crc_got,
                               &s_frame[f->next_off - LOGO_HEADER_BYTES], n);
        f->next_off += n;
        left -= n;
    }
    if (left == 0u) {
        f->crc_got = ~f->crc_got;
        if (f->crc_got != f->crc_want) {
            f->error = 1;
            return -1;
        }
        f->done = 1;
        return 1;
    }
    return 0;
}

int logo_last_tx_status(void) { return s_dbg_tx; }
int logo_last_rx_status(void) { return s_dbg_rx; }
unsigned logo_last_resp(void) { return (unsigned)s_dbg_resp; }

int logo_fetch_done(const struct logo_fetch *f)
{
    return f != NULL && f->done;
}

const uint8_t *logo_frame_data(const struct logo_fetch *f)
{
    (void)f;
    return s_frame;
}

uint32_t logo_frame_bytes(const struct logo_fetch *f)
{
    return f != NULL ? f->data_bytes : 0u;
}

uint32_t logo_frame_crc(const struct logo_fetch *f)
{
    return f != NULL ? f->crc_got : 0u;
}
