/*
 * SD 情绪音效资产：校验、首次入库、随机选择与读取。
 *
 * 资产区位于 SD 的 LBA 1024..2047（512 KB），在录音环形区（LBA 2048 起）
 * 之前，因此环形写指针永远不会覆盖它。资产格式见 tools/build_sound_blob.py。
 */
#include "app_config.h"
#include "app_assets.h"
#include "app_sdspi.h"
#include "app_sounds_blob.h"
#include "app_logo_blob.h"

#include <stddef.h>
#include <string.h>

#include "oshal.h"

#define ASSET_MAGIC          "MOCESND1"
#define ASSET_VERSION        1u
#define ASSET_HEADER_SIZE    32u
#define ASSET_ENTRY_SIZE     40u
#define ASSET_MAX_ENTRIES    APP_SOUND_MAX_ASSETS
#define ASSET_SECTOR         512u
#define ASSET_CHUNK          (8u * ASSET_SECTOR)

struct asset_header {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint16_t count;
    uint16_t reserved;
    uint32_t total_bytes;
    uint32_t crc32;
} __attribute__((packed));

struct asset_entry {
    uint8_t  name[16];
    uint8_t  class_id;
    uint8_t  reserved[3];
    uint32_t offset;
    uint32_t bytes;
    uint32_t crc32;
    uint32_t duration_ms;
    uint32_t reserved2;
} __attribute__((packed));

/* 开机 LOGO 块头（与 tools/build_logo_blob.py 一致） */
#define LOGO_MAGIC          "MOCELGO1"
#define LOGO_VERSION        1u
#define LOGO_HEADER_SIZE    32u

struct logo_header {
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

static int s_ready;
static int s_logo_ready;
static uint32_t s_logo_total;
static uint16_t s_count;
static uint32_t s_sample_rate;
static uint16_t s_channels;
static uint16_t s_bits;
static uint32_t s_total_bytes;
static struct asset_entry s_entries[ASSET_MAX_ENTRIES];
static uint32_t s_rng;
static int s_last_pick[3];
/* 校验/写入共用的大块缓冲放 BSS；采集任务栈只有 4 KB，不能放栈上。 */
static uint8_t s_chunk[ASSET_CHUNK] __attribute__((aligned(4)));

/* ------------------------------------------------------------------ CRC32 */

static uint32_t crc32_add(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    for (uint32_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8u; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

static uint32_t blob_crc(const uint8_t *blob, uint32_t total)
{
    uint8_t head[ASSET_HEADER_SIZE];
    uint32_t crc = 0xFFFFFFFFu;

    memcpy(head, blob, sizeof(head));
    memset(&head[28], 0, 4u);            /* crc32 字段自身不参与计算 */
    crc = crc32_add(crc, head, sizeof(head));
    crc = crc32_add(crc, blob + sizeof(head), total - sizeof(head));
    return ~crc;
}

/* ------------------------------------------------------- SD 头/资产读取 */

static int read_sd_bytes(uint32_t abs_offset, void *dst, uint32_t len)
{
    uint8_t *out = (uint8_t *)dst;

    while (len > 0u) {
        uint32_t in_sector = abs_offset % ASSET_SECTOR;
        uint32_t lba = APP_ASSET_START_LBA + abs_offset / ASSET_SECTOR;

        if (in_sector == 0u && len >= ASSET_SECTOR) {
            uint32_t sectors = len / ASSET_SECTOR;
            uint32_t part;

            if (sectors > 8u) {
                sectors = 8u;
            }
            part = sectors * ASSET_SECTOR;
            if (app_sd_read_blocks(lba, out, sectors) != 0) {
                return -1;
            }
            out += part;
            abs_offset += part;
            len -= part;
        } else {
            uint8_t sector[ASSET_SECTOR] __attribute__((aligned(4)));
            uint32_t part = ASSET_SECTOR - in_sector;

            if (part > len) {
                part = len;
            }
            if (app_sd_read_blocks(lba, sector, 1u) != 0) {
                return -1;
            }
            memcpy(out, &sector[in_sector], part);
            out += part;
            abs_offset += part;
            len -= part;
        }
    }
    return 0;
}

static int sd_header_matches(const struct asset_header *want)
{
    uint8_t sector[ASSET_SECTOR] __attribute__((aligned(4)));
    struct asset_header got;

    if (app_sd_read_blocks(APP_ASSET_START_LBA, sector, 1u) != 0) {
        return 0;
    }
    memcpy(&got, sector, sizeof(got));
    if (memcmp(got.magic, ASSET_MAGIC, 8u) != 0 ||
        got.version != ASSET_VERSION ||
        got.header_size != ASSET_HEADER_SIZE ||
        got.total_bytes < ASSET_HEADER_SIZE + ASSET_ENTRY_SIZE ||
        got.total_bytes > APP_ASSET_MAX_BYTES ||
        got.total_bytes != want->total_bytes ||
        got.crc32 != want->crc32 ||
        got.count != want->count ||
        got.count > ASSET_MAX_ENTRIES) {
        return 0;
    }

    /* 只比较头部（含整块 CRC 字段）。
     *
     * 不再每次启动都流式复算 191 KB：那是 GPIO 位翻转 SPI 的忙等，实测要
     * 12–14 秒，会把 Wi-Fi supplicant 饿死（扫描/关联停在原地）。
     * 头部与内嵌 blob 完全一致即认为已正确入库——安装路径 write_blob_at()
     * 之后会立即做一次完整校验，静态资产之后不会再被写。
     * 代价：若写入中断后头部恰好完整而数据残缺，本次启动不会发现；
     * 重新烧录或清 SD 资产区即可恢复。依据见 docs/10-wifi-boot-sequencing.md。 */
    return 1;
}

/* 完整流式校验（只在安装后调用一次，不参与每次启动）。 */
static int sd_blob_verify(const struct asset_header *want)
{
    uint8_t sector[ASSET_SECTOR] __attribute__((aligned(4)));
    uint8_t head[ASSET_HEADER_SIZE];
    struct asset_header got;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t left;
    uint32_t off;

    if (app_sd_read_blocks(APP_ASSET_START_LBA, sector, 1u) != 0) {
        return 0;
    }
    memcpy(&got, sector, sizeof(got));
    if (memcmp(got.magic, ASSET_MAGIC, 8u) != 0 ||
        got.total_bytes != want->total_bytes || got.crc32 != want->crc32) {
        return 0;
    }
    memcpy(head, sector, sizeof(head));
    memset(&head[28], 0, 4u);
    crc = crc32_add(crc, head, sizeof(head));
    off = sizeof(head);
    left = got.total_bytes - sizeof(head);
    while (left > 0u) {
        uint32_t n = left < sizeof(s_chunk) ? left : (uint32_t)sizeof(s_chunk);

        if (read_sd_bytes(off, s_chunk, n) != 0) {
            return 0;
        }
        crc = crc32_add(crc, s_chunk, n);
        off += n;
        left -= n;
    }
    return (~crc == got.crc32) ? 1 : 0;
}

static int write_blob_at(uint32_t abs_offset, const uint8_t *blob, uint32_t total)
{
    uint32_t off = 0u;

    while (off < total) {
        uint32_t n = total - off;
        uint32_t sectors;

        if (n > sizeof(s_chunk)) {
            n = sizeof(s_chunk);
        }
        memcpy(s_chunk, blob + off, n);
        if (n < sizeof(s_chunk)) {
            memset(&s_chunk[n], 0, sizeof(s_chunk) - n);
        }
        sectors = (n + ASSET_SECTOR - 1u) / ASSET_SECTOR;
        if (app_sd_write_blocks(APP_ASSET_START_LBA +
                                    (abs_offset + off) / ASSET_SECTOR,
                                s_chunk, sectors) != 0) {
            return -1;
        }
        off += sectors * ASSET_SECTOR;
    }
    return 0;
}

static int write_blob_to_sd(const uint8_t *blob, uint32_t total)
{
    return write_blob_at(0u, blob, total);
}

/* -------------------------------------------------------------- 开机 LOGO */

static uint32_t logo_data_crc(const uint8_t *data, uint32_t len)
{
    return ~crc32_add(0xFFFFFFFFu, data, len);
}

static int logo_install(void)
{
    static uint8_t sector[ASSET_SECTOR] __attribute__((aligned(4)));
    const uint8_t *blob = app_logo_blob();
    uint32_t size = app_logo_blob_size();
    struct logo_header hdr;
    int need = 1;

    if (size < LOGO_HEADER_SIZE + 1u) {
        os_printf(LL_WARN, "[asset] logo blob missing (%u bytes)\r\n",
                  (unsigned)size);
        return -1;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    if (memcmp(hdr.magic, LOGO_MAGIC, 8u) != 0 ||
        hdr.version != LOGO_VERSION ||
        hdr.header_size != LOGO_HEADER_SIZE ||
        hdr.data_bytes == 0u ||
        size != LOGO_HEADER_SIZE + hdr.data_bytes ||
        ASSET_LOGO_OFFSET + size > APP_ASSET_MAX_BYTES) {
        os_printf(LL_WARN, "[asset] logo blob header invalid\r\n");
        return -1;
    }
    if (logo_data_crc(blob + LOGO_HEADER_SIZE, hdr.data_bytes) != hdr.data_crc32) {
        os_printf(LL_WARN, "[asset] logo blob CRC mismatch\r\n");
        return -1;
    }

    if (read_sd_bytes(ASSET_LOGO_OFFSET, sector, ASSET_SECTOR) == 0) {
        struct logo_header got;

        memcpy(&got, sector, sizeof(got));
        /* 只比较头部（含 data_crc32）：每次启动复算 48 KB 会长时间占用
         * 位翻转 SPI，影响 Wi-Fi 关联。安装后另行完整校验。 */
        if (memcmp(got.magic, LOGO_MAGIC, 8u) == 0 &&
            got.version == LOGO_VERSION &&
            got.header_size == LOGO_HEADER_SIZE &&
            got.width == hdr.width && got.height == hdr.height &&
            got.bpp == hdr.bpp &&
            got.data_bytes == hdr.data_bytes &&
            got.data_crc32 == hdr.data_crc32) {
            need = 0;
        }
    }

    if (need) {
        os_printf(LL_INFO, "[asset] installing logo %u bytes to LBA %u...\r\n",
                  (unsigned)size,
                  (unsigned)(APP_ASSET_START_LBA + ASSET_LOGO_OFFSET / ASSET_SECTOR));
        if (write_blob_at(ASSET_LOGO_OFFSET, blob, size) != 0) {
            os_printf(LL_ERR, "[asset] logo install failed\r\n");
            return -1;
        }
        /* 安装后完整校验一次（只在首次/换图时发生）。 */
        {
            uint32_t crc = 0xFFFFFFFFu;
            uint32_t off = LOGO_HEADER_SIZE;
            uint32_t left = hdr.data_bytes;
            int bad = 0;

            while (left > 0u) {
                uint32_t n = left < sizeof(s_chunk) ? left : (uint32_t)sizeof(s_chunk);

                if (read_sd_bytes(ASSET_LOGO_OFFSET + off, s_chunk, n) != 0) {
                    bad = 1;
                    break;
                }
                crc = crc32_add(crc, s_chunk, n);
                off += n;
                left -= n;
            }
            if (bad || ~crc != hdr.data_crc32) {
                os_printf(LL_ERR, "[asset] logo verify failed\r\n");
                return -1;
            }
        }
    }
    s_logo_total = size;
    s_logo_ready = 1;
    os_printf(LL_INFO, "[asset] logo ready: %ux%u %ubpp, %u bytes%s\r\n",
              (unsigned)hdr.width, (unsigned)hdr.height, (unsigned)hdr.bpp,
              (unsigned)size, need ? " (installed)" : "");
    return 0;
}

int assets_logo_ready(void)
{
    return s_logo_ready;
}

uint32_t assets_logo_total_bytes(void)
{
    return s_logo_ready ? s_logo_total : 0u;
}

int assets_logo_read(uint32_t byte_offset, void *dst, uint32_t len)
{
    if (!s_logo_ready || dst == NULL ||
        byte_offset > s_logo_total || len > s_logo_total - byte_offset) {
        return -1;
    }
    return read_sd_bytes(ASSET_LOGO_OFFSET + byte_offset, dst, len);
}

/* ------------------------------------------------------------------ 解析 */

static int parse_blob(const uint8_t *blob, uint32_t size,
                      struct asset_header *hdr)
{
    if (size < ASSET_HEADER_SIZE + ASSET_ENTRY_SIZE) {
        os_printf(LL_ERR, "[asset] blob too small: %u\r\n", (unsigned)size);
        return -1;
    }
    memcpy(hdr, blob, sizeof(*hdr));
    if (memcmp(hdr->magic, ASSET_MAGIC, 8u) != 0 ||
        hdr->version != ASSET_VERSION ||
        hdr->header_size != ASSET_HEADER_SIZE) {
        os_printf(LL_ERR, "[asset] bad blob header\r\n");
        return -1;
    }
    if (hdr->total_bytes != size || size > APP_ASSET_MAX_BYTES) {
        os_printf(LL_ERR, "[asset] bad blob size %u/%u (max %u)\r\n",
                  (unsigned)hdr->total_bytes, (unsigned)size,
                  (unsigned)APP_ASSET_MAX_BYTES);
        return -1;
    }
    if (hdr->sample_rate != APP_SOUND_SAMPLE_RATE ||
        hdr->channels != APP_SOUND_CHANNELS || hdr->bits != APP_SOUND_BITS) {
        os_printf(LL_ERR, "[asset] format mismatch: %u/%u/%u\r\n",
                  (unsigned)hdr->sample_rate, (unsigned)hdr->channels,
                  (unsigned)hdr->bits);
        return -1;
    }
    if (hdr->count == 0u || hdr->count > ASSET_MAX_ENTRIES) {
        os_printf(LL_ERR, "[asset] bad count %u\r\n", (unsigned)hdr->count);
        return -1;
    }
    if (blob_crc(blob, size) != hdr->crc32) {
        os_printf(LL_ERR, "[asset] embedded blob CRC mismatch\r\n");
        return -1;
    }

    for (unsigned i = 0; i < hdr->count; ++i) {
        struct asset_entry e;

        memcpy(&e, blob + ASSET_HEADER_SIZE + i * ASSET_ENTRY_SIZE, sizeof(e));
        if (e.offset % ASSET_SECTOR != 0u || e.bytes == 0u ||
            e.offset > size || e.bytes > size - e.offset ||
            (e.class_id != ASSET_CLASS_NEGATIVE &&
             e.class_id != ASSET_CLASS_POSITIVE)) {
            os_printf(LL_ERR, "[asset] bad entry %u\r\n", i);
            return -1;
        }
        if (~crc32_add(0xFFFFFFFFu, blob + e.offset, e.bytes) != e.crc32) {
            os_printf(LL_ERR, "[asset] entry %u CRC mismatch\r\n", i);
            return -1;
        }
        s_entries[i] = e;
    }
    return 0;
}

int assets_init(void)
{
    static uint8_t header_sector[ASSET_SECTOR] __attribute__((aligned(4)));
    const uint8_t *blob = app_sound_blob();
    uint32_t size = app_sound_blob_size();
    struct asset_header hdr;
    int need_install;

    if (s_ready) {
        return 0;
    }
    if (!app_sd_is_ready()) {
        os_printf(LL_ERR, "[asset] SD not ready\r\n");
        return -1;
    }
    if (parse_blob(blob, size, &hdr) != 0) {
        return -1;
    }

    need_install = !sd_header_matches(&hdr);
    if (need_install) {
        os_printf(LL_INFO, "[asset] installing %u bytes to LBA %u...\r\n",
                  (unsigned)size, (unsigned)APP_ASSET_START_LBA);
        if (write_blob_to_sd(blob, size) != 0 ||
            !sd_blob_verify(&hdr)) {
            os_printf(LL_ERR, "[asset] install failed\r\n");
            return -1;
        }
        os_printf(LL_INFO, "[asset] installed\r\n");
    }

    /* 再用 SD 上的头部填充一次，确保运行时元数据来自实际存储。 */
    if (app_sd_read_blocks(APP_ASSET_START_LBA, header_sector, 1u) != 0) {
        return -1;
    }
    memcpy(&hdr, header_sector, sizeof(hdr));
    s_count = hdr.count;
    s_sample_rate = hdr.sample_rate;
    s_channels = hdr.channels;
    s_bits = hdr.bits;
    s_total_bytes = hdr.total_bytes;
    s_rng = 0x9E3779B9u ^ (uint32_t)os_time_get() ^ hdr.crc32;
    s_last_pick[0] = s_last_pick[1] = s_last_pick[2] = -1;
    s_ready = 1;
    os_printf(LL_INFO, "[asset] ready: %u sounds, %u Hz, %u bytes%s\r\n",
              (unsigned)s_count, (unsigned)s_sample_rate,
              (unsigned)s_total_bytes, need_install ? " (installed)" : "");

    /* 音效块必须落在 LOGO 固定偏移之前，否则会互相覆盖。 */
    if (s_total_bytes > ASSET_LOGO_OFFSET) {
        os_printf(LL_ERR, "[asset] sound blob (%u) overlaps logo offset %u\r\n",
                  (unsigned)s_total_bytes, (unsigned)ASSET_LOGO_OFFSET);
    }
    /* LOGO 失败不影响音效与录音 */
    (void)logo_install();
    return 0;
}

int assets_ready(void) { return s_ready; }
int assets_count(void) { return s_ready ? (int)s_count : 0; }

int assets_get(int index, struct asset_info *out)
{
    if (!s_ready || out == NULL || index < 0 || index >= (int)s_count) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->offset = s_entries[index].offset;
    out->bytes = s_entries[index].bytes;
    out->sample_rate = s_sample_rate;
    out->duration_ms = s_entries[index].duration_ms;
    out->channels = s_channels;
    out->bits = s_bits;
    out->class_id = s_entries[index].class_id;
    memcpy(out->name, s_entries[index].name, sizeof(out->name));
    out->name[sizeof(out->name) - 1u] = '\0';
    return 0;
}

int assets_pick(uint8_t class_id)
{
    int matches[ASSET_MAX_ENTRIES];
    int n = 0;
    uint32_t r;

    if (!s_ready || class_id > 2u) {
        return -1;
    }
    for (int i = 0; i < (int)s_count; ++i) {
        if (s_entries[i].class_id == class_id) {
            matches[n++] = i;
        }
    }
    if (n == 0) {
        return -1;
    }
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    r = s_rng % (uint32_t)n;

    /* 同类里尽量不连续播同一条。 */
    if (n > 1 && matches[r] == s_last_pick[class_id]) {
        r = (r + 1u) % (uint32_t)n;
    }
    s_last_pick[class_id] = matches[r];
    return matches[r];
}

int assets_read(int index, uint32_t byte_offset, void *dst, uint32_t len)
{
    if (!s_ready || dst == NULL || index < 0 || index >= (int)s_count) {
        return -1;
    }
    if (byte_offset > s_entries[index].bytes ||
        len > s_entries[index].bytes - byte_offset) {
        return -1;
    }
    return read_sd_bytes(s_entries[index].offset + byte_offset, dst, len);
}
