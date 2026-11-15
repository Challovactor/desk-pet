#include "app_config.h"
#include "app_tts.h"

#include <stddef.h>
#include <string.h>

#include "oshal.h"
#include "uart.h"
#include "chip_pinmux.h"

#if APP_TTS_ENABLE

#define TTS_UART_NUM   E_UART_NUM_0
#define TTS_UART_TX_PIN 17u   /* 与 pincfg.h 的 UART0_TX 一致 */
#define TTS_UART_RX_PIN 2u

static int s_tts_ok;
static uint32_t s_last_ms;
static uint32_t s_seq;

/* GBK 编码的短句（由 Python 生成，避免源码编码问题） */
static const uint8_t s_pos_0[] = {0xba, 0xc3, 0xbf, 0xaa, 0xd0, 0xc4, 0xd1, 0xbd}; /* 好开心呀 */
static const uint8_t s_pos_1[] = {0xcc, 0xab, 0xb0, 0xf4, 0xc1, 0xcb};             /* 太棒了 */
static const uint8_t s_pos_2[] = {0xb9, 0xfe, 0xb9, 0xfe};                         /* 哈哈 */
static const uint8_t s_neg_0[] = {0xce, 0xd8, 0xce, 0xd8, 0xce, 0xd8};             /* 呜呜呜 */
static const uint8_t s_neg_1[] = {0xd3, 0xd0, 0xb5, 0xe3, 0xc4, 0xd1, 0xb9, 0xfd}; /* 有点难过 */
static const uint8_t s_neg_2[] = {0xb2, 0xbb, 0xbf, 0xaa, 0xd0, 0xc4};             /* 不开心 */

struct phrase {
    const uint8_t *text;
    uint32_t len;
};

static const struct phrase s_positive[] = {
    {s_pos_0, sizeof(s_pos_0)},
    {s_pos_1, sizeof(s_pos_1)},
    {s_pos_2, sizeof(s_pos_2)},
};

static const struct phrase s_negative[] = {
    {s_neg_0, sizeof(s_neg_0)},
    {s_neg_1, sizeof(s_neg_1)},
    {s_neg_2, sizeof(s_neg_2)},
};

static void tts_pins(void)
{
    (void)set_pin_function(TTS_UART_TX_PIN, FUNC_UART0_TXD);
    (void)set_pin_function(TTS_UART_RX_PIN, FUNC_UART0_RXD);
}

int tts_init(void)
{
    T_DRV_UART_CONFIG cfg;

    if (s_tts_ok) {
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.uart_baud_rate = BAUD_RATE_9600;
    cfg.uart_data_bits = UART_DATA_BITS_8;
    cfg.uart_stop_bits = UART_STOP_BITS_1;
    cfg.uart_parity_bit = UART_PARITY_BIT_NONE;
    cfg.uart_flow_ctrl = UART_FLOW_CONTROL_DISABLE;
    cfg.uart_tx_mode = ES_UART_TX_MODE_POLL;
    cfg.uart_tx_arg = 0u;
    cfg.uart_rx_mode = ES_UART_RX_MODE_INTR;
    cfg.uart_rx_buf_size = 128u;

    if (drv_uart_open(TTS_UART_NUM, &cfg) != 0) {
        os_printf(LL_WARN, "[tts] uart0 open failed\r\n");
        return -1;
    }
    tts_pins();
    s_tts_ok = 1;
    os_printf(LL_INFO, "[tts] syn6288e uart ready (9600, IO17)\r\n");
    return 0;
}

int tts_ready(void)
{
    return s_tts_ok;
}

int tts_speak_gbk(const uint8_t *text, uint32_t len)
{
    uint8_t frame[208];
    uint32_t total;
    uint8_t csum = 0u;
    uint32_t now;

    if (!s_tts_ok || text == NULL || len == 0u || len > 200u) {
        return -1;
    }
    /* 芯片帧间隔：约 9 ms（与参考固件一致） */
    now = (uint32_t)os_time_get();
    if (s_seq != 0u && now - s_last_ms < 9u) {
        os_msleep(9u - (now - s_last_ms));
    }

    frame[0] = 0xfd;
    frame[1] = 0x00;
    frame[2] = (uint8_t)(len + 3u);
    frame[3] = 0x01;
    frame[4] = 0x01;
    memcpy(&frame[5], text, len);
    total = len + 6u;
    for (uint32_t i = 0u; i + 1u < total; ++i) {
        csum ^= frame[i];
    }
    frame[total - 1u] = csum;

    /* I2S 初始化可能把 IO17 复用为 I2S 时钟，发送前重新路由 */
    tts_pins();
    (void)drv_uart_write(TTS_UART_NUM, (char *)frame, total);
    s_last_ms = (uint32_t)os_time_get();
    ++s_seq;
    return 0;
}

void tts_say_emotion(int positive)
{
    const struct phrase *list = positive ? s_positive : s_negative;
    uint32_t n = positive ? (uint32_t)(sizeof(s_positive) / sizeof(s_positive[0]))
                          : (uint32_t)(sizeof(s_negative) / sizeof(s_negative[0]));
    uint32_t pick;

    if (!s_tts_ok || n == 0u) {
        return;
    }
    /* 用时间 + 序号做轮换，避免连续两次总选同一句 */
    pick = ((uint32_t)os_time_get() / 97u + s_seq) % n;
    os_printf(LL_INFO, "[tts] speak %s phrase %u\r\n",
              positive ? "positive" : "negative", (unsigned)pick);
    (void)tts_speak_gbk(list[pick].text, list[pick].len);
}

#else /* !APP_TTS_ENABLE */

int tts_init(void) { return 0; }
int tts_ready(void) { return 1; }
int tts_speak_gbk(const uint8_t *text, uint32_t len) { (void)text; (void)len; return -1; }
void tts_say_emotion(int positive) { (void)positive; }

#endif
