#ifndef APP_ASSETS_H
#define APP_ASSETS_H

#include <stdint.h>

/*
 * SD 预置情绪音效。
 *
 * 资产以自定义二进制块的形式烧进固件 .rodata；首次启动（或资产版本变化时）
 * 写入 SD 的保留区 LBA 1024..2047（512 KB），之后回放直接从 SD 读取。
 * 这样“烧录固件”即可完成音效入库，设备运行时不需要网络下载或文件系统。
 */

#define ASSET_CLASS_NEGATIVE 1u
#define ASSET_CLASS_POSITIVE 2u

struct asset_info {
    uint32_t offset;        /* 相对资产区起点的字节偏移（512 对齐） */
    uint32_t bytes;         /* PCM16LE 单声道字节数 */
    uint32_t sample_rate;
    uint32_t duration_ms;
    uint16_t channels;
    uint16_t bits;
    uint8_t class_id;
    char name[16];
};

/* 在 store_init() 之后、开始采集循环之前调用；会校验并必要时写入 SD。 */
int assets_init(void);
int assets_ready(void);
int assets_count(void);

/* 按情绪类别随机选一个音效；没有该类资产时返回 -1。 */
int assets_pick(uint8_t class_id);

/* 取得指定音效的元数据。 */
int assets_get(int index, struct asset_info *out);

/* 从某个音效的 byte_offset 处读取 len 字节 PCM；offset 通常 512 对齐。 */
int assets_read(int index, uint32_t byte_offset, void *dst, uint32_t len);

/* ---------------- 开机 LOGO（墨水屏图，SD 音效区尾部固定偏移） ----------------
 * 音效块从资产区起点放（当前约 190 KB），LOGO 块固定放在 256 KB 偏移，
 * 两者不重叠（APP_ASSET_MAX_BYTES = 512 KB）。STM32 开机经 I2C 分块读取。 */
#define ASSET_LOGO_OFFSET 262144u

int assets_logo_ready(void);
uint32_t assets_logo_total_bytes(void);

/* 从 LOGO 块的 byte_offset 处读取 len 字节（块头 + 位图，任意偏移）。 */
int assets_logo_read(uint32_t byte_offset, void *dst, uint32_t len);

#endif
