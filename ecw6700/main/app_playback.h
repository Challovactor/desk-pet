#ifndef APP_PLAYBACK_H
#define APP_PLAYBACK_H

#include <stdint.h>

/*
 * 喇叭回放（BOARD_I2S_0 / MIC_SPEAKER 接口）。
 *
 * 喇叭经模块上独立的 SPK-OUT 口出声（板载 codec/功放）；回放的 I2S 数字线
 * 与 PDM 麦克风共用 MIC 连接器（WS/SCK/SD），数据线半双工：
 * 播放时必须暂停采集，播完再恢复。
 *
 * v2 起统一按 16 kHz 输出（与 ES8311 已验证配置一致）：
 *   - 情绪音效本身是 16 kHz，直接播放；
 *   - “回放最近 N 秒录音”是 32 kHz，播放前经 app_resample 降采样到 16 kHz。
 */

/* 采集任务每轮调用一次：返回 1 表示播放进行中（本轮不要读音频/写 SD）。 */
int playback_pause_poll(void);

/* 回放最近 APP_CLIP_SECONDS 秒录音（32k -> 16k 后输出）。 */
int playback_play_clip(void);

/* 回放一个 SD 情绪音效资产；index 由 assets_pick() 返回。 */
int playback_play_asset(int asset_index);

/* 临时诊断：播放指定频率/时长的方波测试音。
 * use_i2s_format=1 用标准 I2S，0 用 left-justified；用于确认 SPK 格式。 */
int playback_play_tone(uint32_t hz, uint32_t ms, int use_i2s_format);

/* 第一次播放时配置 ES8311（16 kHz MCLK 稳定后）；返回 0 表示配置成功。 */
int playback_ensure_codec(void);
/* -1 = 尚未尝试，0 = 配置失败，1 = 配置成功 */
int playback_codec_state(void);

#endif
