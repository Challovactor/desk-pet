#ifndef APP_UPLOAD_H
#define APP_UPLOAD_H

#include <stdint.h>

/*
 * desk-recorder/v1 设备主动保存（正式上传）
 *
 * 流程（server/API.md v0.1.1）：
 *   1. POST /device/v1/recordings        申请保存，拿 job_id/recording_id
 *   2. MQTT commands/events (audio.export) 或 GET /jobs/{id} 取命令
 *   3. GET  /device/v1/jobs/{id}          预检查/取不可变命令
 *   4. POST /device/v1/jobs/{id}/output   提交 WAV 元数据（大小/SHA256）
 *   5. PUT  /device/v1/jobs/{id}/output/blob  流式上传 16 kHz WAV
 *   6. POST /device/v1/jobs/{id}/result   终态确认
 *
 * 上传内容是“当前保留时长”的最近 N 分钟，而不是 v1 的 30 秒片段；
 * 32 kHz 录音在读取时经 app_resample 降到 16 kHz。
 */

struct clip_job {
    uint32_t offset;        /* 仅 legacy 路径使用 */
    uint32_t bytes;
    uint32_t seconds;
    uint32_t clip_id;
    char idem[64];          /* Idempotency-Key，重试必须复用 */
};

/* 在 upload_task 里调用；返回 0 表示服务器确认成功。 */
int upload_formal_run(const struct clip_job *job);

#endif
