#ifndef APP_MQTT_H
#define APP_MQTT_H

#include <stdint.h>

/*
 * desk-recorder/v1 MQTT 通道（MQTT 3.1.1 + TLS 8883）。
 *
 * 主题：desk/v1/devices/{device_id}/commands/events  QoS1（订阅）
 *       desk/v1/devices/{device_id}/telemetry       QoS0（发布）
 *       desk/v1/devices/{device_id}/presence        QoS1 retained（发布）
 *
 * 服务器要求的 presence/telemetry/命令 JSON 字段以服务端
 * HARDWARE_INTEGRATION.md 为准；本实现把收到的命令原样记录到日志，
 * 并从中提取 job_id/recording_id/type/minutes 供上传任务使用。
 */

struct mqtt_command {
    char job_id[64];
    char recording_id[64];
    char type[32];
    long long minutes;
    long long execute_before_ms;
};

void app_mqtt_start(void);
int  mqtt_connected(void);
int  mqtt_wait_connected(uint32_t timeout_ms);

/* 非阻塞取一条服务端命令：返回 1 表示取到，0 表示没有。 */
int  mqtt_take_command(struct mqtt_command *out);

/* 心跳/状态发布；由 MQTT 任务周期调用，也可在上传完成后调用。 */
void mqtt_publish_presence(void);
void mqtt_publish_telemetry(uint32_t bytes_sent);

/* 服务端 audio.configure 下发的保留分钟数（1–60，RAM 内；默认 10）。 */
long long mqtt_retention_minutes(void);

#endif
