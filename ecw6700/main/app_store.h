#ifndef APP_STORE_H
#define APP_STORE_H

#include <stdint.h>

/*
 * 原始音频环形存储（裸扇区，无文件系统）。
 * 写入：PCM16LE 单声道连续追加，按 4KB 对齐写 SD。
 * 读取：触摸时取"最近 N 秒"对应的字节区间，支持环形回绕。
 */
int      store_init(void);
int      store_write(const void *data, uint32_t len);
int      store_flush(void);
uint64_t store_total_bytes(void);
uint32_t store_ring_bytes(void);

/* 取最近 want_bytes 字节：返回实际可用字节数，并给出起始环偏移。 */
int store_clip_begin(uint32_t want_bytes, uint32_t *out_bytes, uint32_t *out_offset);

/* 从 cursor 指向的环偏移顺序读取 len 字节，读完后 cursor 前移。 */
int store_clip_read(uint32_t *cursor, void *dst, uint32_t len);

#endif
