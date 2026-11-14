/*
 * 原始音频环形存储。
 *
 * 设计取舍：
 *   - 不用 FATFS/文件：持续 64KB/s 写入时文件系统元数据更新与掉电一致性更麻烦；
 *   - 从 APP_SD_START_LBA 起把整张卡当作环形缓冲区，写指针单调前进并回绕；
 *   - 只记录"总写入字节数"，最近 N 秒 = 最后 N*64000 字节，读取时按环偏移回绕；
 *   - 上传期间采集继续写，但 N 秒窗口远小于环容量，不会被写指针追上。
 */
#include "app_config.h"
#include "app_store.h"
#include "app_sdspi.h"

#include <stddef.h>
#include <string.h>

#include "oshal.h"

/* 环偏移/容量用 uint32，必须把可用环限制在 4GB 以内；512MB 足够存 2 小时。 */
#define STORE_MAX_BYTES (512u * 1024u * 1024u)

static os_mutex_handle_t s_lock;
static uint8_t  s_buf[APP_SD_CHUNK_BYTES] __attribute__((aligned(4)));
static uint32_t s_fill;
static uint32_t s_write_off;      /* 环内写偏移，按 CHUNK 对齐 */
static uint64_t s_total;          /* 已写入总字节数（含 flush 的补零） */
static uint32_t s_ring_bytes;
static int s_ready;

int store_init(void)
{
    uint32_t sectors;
    uint32_t ring_sectors;

    if (s_ready) {
        return 0;
    }
    if (s_lock == NULL) {
        s_lock = os_mutex_create();
        if (s_lock == NULL) {
            return -1;
        }
    }
    if (app_sd_init() != 0) {
        return -1;
    }
    sectors = app_sd_sector_count();
    if (sectors <= APP_SD_START_LBA + 8u) {
        os_printf(LL_ERR, "[store] card too small: %lu sectors\r\n",
                  (unsigned long)sectors);
        return -1;
    }
    ring_sectors = sectors - APP_SD_START_LBA;
    if (ring_sectors > STORE_MAX_BYTES / 512u) {
        ring_sectors = STORE_MAX_BYTES / 512u;
    }
    /* 写指针按 4KB 步进；环大小必须是 4KB 的整数倍，否则回绕后的
     * 最后一次写会越过卡尾并被 SD 驱动永久拒绝。 */
    s_ring_bytes = (ring_sectors * 512u) & ~(APP_SD_CHUNK_BYTES - 1u);
    if (s_ring_bytes == 0u) {
        os_printf(LL_ERR, "[store] ring size zero\r\n");
        return -1;
    }
    s_write_off = 0;
    s_total = 0;
    s_fill = 0;

    /* 至少能放下 2 个回溯窗口，否则回绕时可能覆盖正在上传的片段 */
    if (s_ring_bytes < APP_CLIP_SECONDS * APP_PCM_BYTES_PER_SEC * 2u) {
        os_printf(LL_WARN, "[store] small ring %lu KB for %lu s clip\r\n",
                  (unsigned long)(s_ring_bytes / 1024u),
                  (unsigned long)APP_CLIP_SECONDS);
    }
    s_ready = 1;
    os_printf(LL_INFO, "[store] ring %lu KB from LBA %lu\r\n",
              (unsigned long)(s_ring_bytes / 1024u),
              (unsigned long)APP_SD_START_LBA);
    return 0;
}

static int store_write_chunk(const uint8_t *chunk)
{
    uint32_t lba = APP_SD_START_LBA + s_write_off / 512u;
    uint32_t count = APP_SD_CHUNK_BYTES / 512u;

    if (app_sd_write_blocks(lba, chunk, count) != 0) {
        return -1;
    }
    s_write_off = (s_write_off + APP_SD_CHUNK_BYTES) % s_ring_bytes;
    s_total += APP_SD_CHUNK_BYTES;
    return 0;
}

int store_write(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (!s_ready || data == NULL || len == 0u) {
        return -1;
    }
    if (os_mutex_lock(s_lock, 2000) != 0) {
        return -1;
    }
    while (len > 0u) {
        uint32_t space = APP_SD_CHUNK_BYTES - s_fill;
        uint32_t part = len < space ? len : space;

        memcpy(&s_buf[s_fill], p, part);
        s_fill += part;
        p += part;
        len -= part;

        if (s_fill == APP_SD_CHUNK_BYTES) {
            if (store_write_chunk(s_buf) != 0) {
                s_fill = 0;
                os_mutex_unlock(s_lock);
                os_printf(LL_ERR, "[store] write failed, dropping chunk\r\n");
                return -1;
            }
            s_fill = 0;
        }
    }
    os_mutex_unlock(s_lock);
    return 0;
}

int store_flush(void)
{
    if (!s_ready) {
        return -1;
    }
    if (os_mutex_lock(s_lock, 2000) != 0) {
        return -1;
    }
    if (s_fill > 0u) {
        memset(&s_buf[s_fill], 0, APP_SD_CHUNK_BYTES - s_fill);
        if (store_write_chunk(s_buf) != 0) {
            s_fill = 0;
            os_mutex_unlock(s_lock);
            return -1;
        }
        s_fill = 0;
    }
    os_mutex_unlock(s_lock);
    return 0;
}

uint64_t store_total_bytes(void)
{
    uint64_t total;
    if (!s_ready) {
        return 0;
    }
    if (os_mutex_lock(s_lock, 1000) != 0) {
        /* 拿不到锁时返回上一次已知值，避免把"正在写"误报成 0 */
        return s_total;
    }
    total = s_total;
    os_mutex_unlock(s_lock);
    return total;
}

uint32_t store_ring_bytes(void)
{
    return s_ring_bytes;
}

int store_clip_begin(uint32_t want_bytes, uint32_t *out_bytes, uint32_t *out_offset)
{
    uint64_t total;
    uint32_t avail;

    if (!s_ready || out_bytes == NULL || out_offset == NULL) {
        return -1;
    }
    if (os_mutex_lock(s_lock, 3000) != 0) {
        return -1;
    }
    total = s_total;
    avail = want_bytes;
    if (avail > total) {
        avail = (uint32_t)total;
    }
    if (avail > s_ring_bytes) {
        avail = s_ring_bytes;
    }
    *out_bytes = avail;
    *out_offset = (uint32_t)((total - avail) % s_ring_bytes);
    os_mutex_unlock(s_lock);
    return avail > 0u ? 0 : -1;
}

int store_clip_read(uint32_t *cursor, void *dst, uint32_t len)
{
    uint8_t *out = (uint8_t *)dst;
    uint8_t sector[512] __attribute__((aligned(4)));

    if (!s_ready || cursor == NULL || dst == NULL || len == 0u) {
        return -1;
    }
    while (len > 0u) {
        uint32_t off = *cursor % s_ring_bytes;
        uint32_t in_sector = off % 512u;
        uint32_t lba = APP_SD_START_LBA + off / 512u;
        uint32_t part;

        /* 对齐且剩余足够时，一次读 8 个扇区，减少协议开销 */
        if (in_sector == 0u && len >= 8u * 512u && off + 8u * 512u <= s_ring_bytes) {
            part = 8u * 512u;
            if (app_sd_read_blocks(lba, out, 8u) != 0) {
                return -1;
            }
        } else {
            part = 512u - in_sector;
            if (part > len) {
                part = len;
            }
            /* 环回绕到尾部时不能跨越结尾 */
            if (off + part > s_ring_bytes) {
                part = s_ring_bytes - off;
            }
            if (app_sd_read_blocks(lba, sector, 1u) != 0) {
                return -1;
            }
            memcpy(out, &sector[in_sector], part);
        }
        out += part;
        len -= part;
        *cursor = (uint32_t)((*cursor + part) % s_ring_bytes);
    }
    return 0;
}
