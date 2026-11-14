/*
 * 应用配置。当前阶段按需求约定：Wi-Fi 凭据编译期写死，服务器走可配置 HTTP。
 * 改完这里需要重新编译并烧录。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ===================== Wi-Fi（必填） ===================== */
#define APP_WIFI_SSID          "YOUR_WIFI_SSID"
#define APP_WIFI_PASSWORD      "YOUR_WIFI_PASSWORD"

/* ===================== 服务器（必填） ===================== */
/* 服务器地址可以是 IP 或域名；端口按 HTTP 服务实际监听填写。 */
#define APP_SERVER_HOST        "YOUR_SERVER_IP"
#define APP_SERVER_PORT        8080

/* 1) 收到触摸后先 POST 一个"记录"事件（JSON） */
#define APP_SERVER_EVENT_PATH  "/api/record"
/* 2) 紧接着 POST 最近 N 秒录音（WAV，PCM16LE 单声道） */
#define APP_SERVER_AUDIO_PATH  "/api/audio"

#define APP_DEVICE_ID          "moce-01"

/* ===================== 录音参数 ===================== */
/* 回溯时长：触摸时上传"刚刚过去"的秒数 */
#define APP_CLIP_SECONDS       30u
/* PDM 采样率（I2S 配置）与解码后的 PCM 采样率 */
#define APP_PDM_SAMPLE_RATE    32000u
#define APP_PCM_SAMPLE_RATE    32000u
#define APP_PCM_CHANNELS       1u
#define APP_PCM_BYTES_PER_SEC  (APP_PCM_SAMPLE_RATE * 2u * APP_PCM_CHANNELS)

/* 每次 I2S DMA 读取的原始 PDM 字节数。
 * 8192 字节 = 65536 个 PDM bit = 1024 个 PCM 样本（32ms 音频）。 */
#define APP_PDM_READ_BYTES     8192u
/* 8192 B × 8 bit / 64 = 1024 个 PCM 样本（32ms 音频） */
#define APP_PCM_SAMPLES_MAX    (APP_PDM_READ_BYTES * 8u / 64u)

/* MIC 连接器为 5 线：3V3 / I2S_WS / I2S_SCK / I2SEXT_SD / GND。
 * 板级端口 BOARD_I2S_0 = {6,7,8,3} 按连接器信号顺序对应：
 *   WS=IO6, SCK=IO7, SD=IO8, 第 4 脚 IO3 未接信号（保留为 MCLK）。
 * 麦克风是 PDM：时钟走 SCK（IO7），数据走 SD（IO8）；WS 由主机驱动但麦克风不使用。
 * 如果上板无数据，可尝试把 RXWS/RXSCK 两个值互换（即 WS=7/SCK=6）。 */
#define APP_I2S_RXWS_PIN       6u
#define APP_I2S_RXSCK_PIN      7u
#define APP_I2S_RXD_PIN        8u
#define APP_I2S_MCLK_PIN       3u

/* ===================== SD 卡（SPI 模式，6 线） ===================== */
/* SD 连接器：3V3 / SPI_CS / SPI_SCK / SPI_MISO / SPI_MOSI / GND。
 * 板级端口 BOARD_SPI_0 = {14,12,13} 按连接器信号顺序对应：
 *   SCK=IO14, MISO=IO12, MOSI=IO13；CS 走 BOARD_GPIO_4 = IO11。 */
#define APP_SD_CS_GPIO         11u
#define APP_SD_CLK_GPIO        14u
#define APP_SD_MISO_GPIO       12u
#define APP_SD_MOSI_GPIO       13u
/* 环形存储从该 LBA 开始，保留前 1MB 不碰（分区表/引导区） */
#define APP_SD_START_LBA       2048u
/* 环形写缓冲（必须是 512 的整数倍） */
#define APP_SD_CHUNK_BYTES     4096u

/* ===================== 板间 I2C（ECW 作从机） ===================== */
#define APP_LINK_I2C_NUM       0u
#define APP_LINK_I2C_ADDR7     0x42u
#define APP_LINK_RX_BUF        32u
#define APP_LINK_TX_BUF        32u

#define APP_LINK_CMD_TOUCH     0x01u
#define APP_LINK_CMD_PING      0x02u

/* ===================== 任务参数 ===================== */
#define APP_TASK_CAPTURE_PRIO  5
#define APP_TASK_TRIGGER_PRIO  4
#define APP_TASK_UPLOAD_PRIO   3
#define APP_TASK_NET_PRIO      3
#define APP_TASK_CAPTURE_STACK 4096
#define APP_TASK_TRIGGER_STACK 6144
#define APP_TASK_UPLOAD_STACK  8192
#define APP_TASK_NET_STACK     8192

/* 触摸事件队列：上传期间的多次触摸不会丢 */
#define APP_UPLOAD_QUEUE_LEN   4

#endif /* APP_CONFIG_H */
