#ifndef APP_LOGO_H
#define APP_LOGO_H

#include <stdint.h>

#include "stm32f4xx_hal.h"

/*
 * 开机 LOGO：经板间 I2C（ECW 从机 0x42）从 SD 分块读取。
 *
 * 协议：主机写 [0x20, off_lo, off_hi, len_lo, len_hi]，等待约 15 ms 后
 * 读 len 字节（len <= 128）。块格式见 tools/build_logo_blob.py。
 *
 * 用法（主循环里分步执行，避免长时间阻塞触摸）：
 *   struct logo_fetch f;
 *   if (logo_fetch_start(&f, &hi2c1) == 0)
 *       while (logo_fetch_step(&f) == 0) { ... }
 *   if (logo_fetch_done(&f)) epd_show_frame(logo_frame_data(&f));
 */

#define LOGO_CHUNK_BYTES   200u
#define LOGO_MAX_DATA      48000u
#define LOGO_FRAME_BYTES   48000u

struct logo_fetch {
    uint16_t w;
    uint16_t h;
    uint32_t data_bytes;
    uint32_t crc_want;
    uint32_t crc_got;
    uint32_t next_off;      /* 下一个要读的偏移（含头部） */
    uint8_t  header_read;
    uint8_t  done;
    uint8_t  error;
};

/* 读取并校验 32 字节块头；ECW 未就绪时返回 -1，可稍后重试。 */
int  logo_fetch_start(struct logo_fetch *f, I2C_HandleTypeDef *hi2c);

/* 读一个 128 字节分块。返回 0=还要继续，1=完成，-1=失败；单次约 20-30 ms。 */
int  logo_fetch_step(struct logo_fetch *f);

int  logo_fetch_done(const struct logo_fetch *f);

/* 诊断：最近一次 I2C 读事务的 HAL 状态与从机回传首字节。 */
int logo_last_tx_status(void);
int logo_last_rx_status(void);
unsigned logo_last_resp(void);
const uint8_t *logo_frame_data(const struct logo_fetch *f);
uint32_t logo_frame_bytes(const struct logo_fetch *f);
uint32_t logo_frame_crc(const struct logo_fetch *f);

#endif
