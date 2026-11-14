#ifndef APP_NET_H
#define APP_NET_H

#include <stdint.h>

/* 在调度器启动前调用：注册 Wi-Fi 事件订阅。 */
void net_init(void);
/* 创建网络任务（连 Wi-Fi / 断线重连）。 */
int  net_start(void);

int  net_connected(void);
int  net_wait_connected(uint32_t timeout_ms);

/* 触摸后：先上报记录事件，再上传最近 clip_bytes 字节的 WAV。
 * clip_id 用于把 record 事件与 audio 上传关联起来。 */
int  net_report_record(uint32_t clip_bytes, uint32_t seconds, uint32_t clip_id);
int  net_upload_clip(uint32_t start_offset, uint32_t clip_bytes, uint32_t seconds,
                     uint32_t clip_id);

#endif
