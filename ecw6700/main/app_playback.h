#ifndef APP_PLAYBACK_H
#define APP_PLAYBACK_H

#include <stdint.h>

/*
 * 喇叭回放（BOARD_I2S_0 / MIC_SPEAKER 接口）。
 *
 * 喇叭经模块上独立的 SPK-OUT 口出声（板载 codec/功放）；回放的 I2S 数字线
 * 与 PDM 麦克风共用 MIC 连接器（WS/SCK/SD），数据线半双工：
 * 播放时必须暂停采集，播完再恢复。入口只有一个：回放最近 N 秒录音。
 */

/* 采集任务每轮调用一次：返回 1 表示播放进行中（本轮不要读音频/写 SD）。 */
int playback_pause_poll(void);

/* 回放最近 APP_CLIP_SECONDS 秒录音：
 * 暂停采集 -> I2S 重新配置为 TX -> 流式送出 -> 恢复采集。
 * 阻塞时长约等于片段秒数；失败也会恢复采集。 */
int playback_play_clip(void);

#endif
