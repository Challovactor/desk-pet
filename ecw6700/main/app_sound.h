#ifndef APP_SOUND_H
#define APP_SOUND_H

#include <stdint.h>

/*
 * 情绪音效：ES8311 codec 初始化 + 手势命令到音效类别的映射。
 *
 * 手势 -> 情绪（用户确认的映射）：
 *   0x11 单击 -> 正面（2/4）
 *   0x12 双击 -> 负面（1/3）
 *   0x13 上滑 -> 正面（2/4）
 *   0x14 下滑 -> 负面（1/3）
 * 同类音效由 assets_pick() 随机选择，并尽量不连续重复。
 */

/* 在 moce_app_main() 里创建 codec 配置任务（必须先于采集/触发任务）。 */
void sound_start(void);

/* codec 任务是否已结束（成功或失败）。采集任务在 audio_init() 前等它，
 * 确保 codec 看到的是 16 kHz 播放时钟而不是 32 kHz 采集时钟。 */
int sound_ready(void);

/* 在 trigger_task 里、app_link_init() 之前调用：等待 codec 配置完成。
 * 返回 0 表示配置成功。 */
int sound_init(void);

/* -1 = 尚未完成，0 = 配置失败，1 = 配置成功（仅用于状态日志）。 */
int sound_codec_ok(void);

/* 处理一条手势命令；不认识的命令返回 -1。内部会阻塞到音效播完。 */
int sound_play_gesture(uint8_t link_cmd);

/* 普通触摸（0x01）：从全部音效里随机播一个（不区分类别）。
 * 阻塞到播放结束；assets 未就绪时返回 -1。 */
int sound_play_random(void);

#endif
