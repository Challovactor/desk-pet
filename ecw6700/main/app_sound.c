/*
 * 情绪音效控制（ECW6700F）
 *
 * 手势 -> 情绪映射在这里；真正的 ES8311 配置和 I2S 输出在 app_playback.c。
 * codec 必须与 16 kHz 的播放 MCLK 一起配置，所以延迟到第一次播放时执行。
 */
#include "app_config.h"
#include "app_sound.h"
#include "app_assets.h"
#include "app_playback.h"
#include "app_tts.h"

#include <stddef.h>

#include "oshal.h"

#define SOUND_CODEC_STACK  4096
#define SOUND_CODEC_WAIT_MS 5000u

static volatile int s_codec_ready;
static volatile int s_codec_ok;

static void codec_task(void *arg)
{
    (void)arg;
    s_codec_ok = (playback_ensure_codec() == 0) ? 1 : 0;
    s_codec_ready = 1;
    os_printf(s_codec_ok ? LL_INFO : LL_WARN,
              "[sound] codec task done, ok=%d\r\n", s_codec_ok);
    os_task_delete(os_task_get_running_handle());
}

void sound_start(void)
{
    int task;

    /* 与 firmware-code-ecw-1115 一样在调度器启动前打开 UART0（9600），
     * 让 TTS 模块先上电就绪；I2S 初始化后才首次发送。 */
    (void)tts_init();

    task = os_task_create("app-codec", APP_TASK_TRIGGER_PRIO + 1,
                          SOUND_CODEC_STACK, codec_task, NULL);
    if (task == OS_TASK_CREATE_FAILED) {
        os_printf(LL_ERR, "[sound] codec task create failed\r\n");
        s_codec_ok = 0;
        s_codec_ready = 1;
    }
}

int sound_ready(void)
{
    return s_codec_ready;
}

int sound_init(void)
{
    unsigned waited = 0u;

    while (!s_codec_ready && waited < SOUND_CODEC_WAIT_MS) {
        os_msleep(10);
        waited += 10u;
    }
    if (!s_codec_ready) {
        os_printf(LL_WARN, "[sound] codec task did not finish in %u ms\r\n",
                  (unsigned)SOUND_CODEC_WAIT_MS);
        return -1;
    }
    os_printf(LL_INFO, "[sound] 4 gesture sounds, codec ok=%d\r\n", s_codec_ok);
    return s_codec_ok ? 0 : -1;
}

int sound_codec_ok(void)
{
    if (!s_codec_ready) {
        return -1;
    }
    return s_codec_ok ? 1 : 0;
}

static int gesture_class(uint8_t link_cmd)
{
    switch (link_cmd) {
    case APP_LINK_CMD_GESTURE_CLICK:
    case APP_LINK_CMD_GESTURE_UP:
        return ASSET_CLASS_POSITIVE;
    case APP_LINK_CMD_GESTURE_DOUBLE:
    case APP_LINK_CMD_GESTURE_DOWN:
        return ASSET_CLASS_NEGATIVE;
    default:
        return -1;
    }
}

int sound_play_gesture(uint8_t link_cmd)
{
    int class_id = gesture_class(link_cmd);
    int index;

    if (class_id < 0) {
        return -1;
    }
    /* TTS 先出声（UART 发送立即返回，模块自己播报），随后再做 I2S 音效；
     * 两条通路互不影响，哪条硬件没接都能在日志里区分。 */
    tts_say_emotion(class_id == ASSET_CLASS_POSITIVE);
    if (!assets_ready()) {
        os_printf(LL_WARN, "[sound] assets not ready, skip gesture 0x%02x\r\n",
                  (unsigned)link_cmd);
        return -1;
    }
    index = assets_pick((uint8_t)class_id);
    if (index < 0) {
        os_printf(LL_WARN, "[sound] no %s sound asset\r\n",
                  class_id == ASSET_CLASS_POSITIVE ? "positive" : "negative");
        return -1;
    }
    os_printf(LL_INFO, "[sound] gesture 0x%02x -> %s class, asset %d\r\n",
              (unsigned)link_cmd,
              class_id == ASSET_CLASS_POSITIVE ? "positive" : "negative", index);
    return playback_play_asset(index);
}

int sound_play_random(void)
{
    struct asset_info info;
    int index;

    if (!assets_ready()) {
        os_printf(LL_WARN, "[sound] assets not ready, skip random touch\r\n");
        return -1;
    }
    index = assets_pick_any();
    if (index < 0) {
        return -1;
    }
    if (assets_get(index, &info) == 0) {
        os_printf(LL_INFO, "[sound] touch -> random asset %d (%s)\r\n",
                  index, info.name);
    }
    return playback_play_asset(index);
}
