/*
 * 应用配置。当前阶段按需求约定：Wi-Fi 凭据编译期写死，服务器走可配置 HTTP。
 * 改完这里需要重新编译并烧录。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ===================== Wi-Fi（必填） ===================== */
#define APP_WIFI_SSID          "YOUR_WIFI_SSID"
#define APP_WIFI_PASSWORD      "YOUR_WIFI_PASSWORD"

/* 1 = 连接前先做一次主动扫描并打印 AP 列表。
 * 实测：不做这次扫描时，sta_start 之后 supplicant 完全没有扫描/关联活动
 * （100s 无任何 wpa 日志，永远连不上）；扫描后能正常 Selected BSS 并关联。
 * 代价是首次连接多几秒，但远好于连不上。 */
#define APP_WIFI_SCAN_DEBUG    1

/* ===================== 服务器（必填） ===================== */
/* v2 正式服务器（server/key.txt）：HTTPS 443 + MQTTS 8883。
 * 旧明文 HTTP 端口在此服务器未开放，APP_UPLOAD_FORMAL=0 的回退路径不可用。 */
#define APP_SERVER_HOST        "YOUR_SERVER_IP"
#define APP_SERVER_PORT        8080

/* 1) 收到触摸后先 POST 一个"记录"事件（JSON，v1 回退） */
#define APP_SERVER_EVENT_PATH  "/api/record"
/* 2) 紧接着 POST 最近 N 秒录音（WAV，PCM16LE 单声道，v1 回退） */
#define APP_SERVER_AUDIO_PATH  "/api/audio"

#define APP_DEVICE_ID          "YOUR_DEVICE_ID"

/* ===================== desk-recorder/v1 正式上传（v2） ===================== */
/* 1 = 走 server/API.md 的设备主动保存 + MQTT/HTTPS 四步流程；
 * 0 = 保留 v1 的明文 HTTP /api/record + /api/audio（仅联调回退用）。 */
#define APP_UPLOAD_FORMAL      1

/* HTTPS/MQTTS 使用同一台服务器；设备令牌和 CA 证书必须由服务端安装程序提供。
 * TLS 主机名必须与证书 CN/SAN 一致（用 IP 连接时证书需带 IP SAN）。 */
#define APP_SERVER_HTTPS_PORT  443
#define APP_DEVICE_TOKEN       "YOUR_DEVICE_TOKEN"

#define APP_MQTT_HOST          APP_SERVER_HOST
#define APP_MQTT_PORT          8883
#define APP_MQTT_USERNAME      "YOUR_DEVICE_ID"
#define APP_MQTT_PASSWORD      "YOUR_MQTT_PASSWORD"

#define APP_MQTT_TOPIC_PREFIX  "desk/v1/devices/" APP_DEVICE_ID "/"
#define APP_MQTT_TOPIC_COMMANDS APP_MQTT_TOPIC_PREFIX "commands/events"
/* 服务器文档写 commands/events，运维口径写 commands；两个都订阅，避免主题歧义。 */
#define APP_MQTT_TOPIC_COMMANDS_ALT APP_MQTT_TOPIC_PREFIX "commands"
#define APP_MQTT_TOPIC_TELEMETRY APP_MQTT_TOPIC_PREFIX "telemetry"
#define APP_MQTT_TOPIC_PRESENCE APP_MQTT_TOPIC_PREFIX "presence"

/* 设备当前保留时长（分钟）；服务端 audio.configure 任务可覆盖（RAM 内）。 */
/* 单次上传的最大快照秒数。服务器要求"最近 N 分钟"，但本板 SD 是位翻转
 * 软件 SPI：连续读超过约 15 秒就会把 Wi-Fi RF 拖死（实测）。10 秒快照
 * 的 SD 读约 5 秒，能落在 RF 容忍窗口内，也满足服务器 10 秒首包要求。
 * 硬件改善后可调大。 */
#define APP_UPLOAD_MAX_SECONDS  10u

#define APP_RETENTION_MINUTES  10u
#define APP_FIRMWARE_VERSION   "moce-v2-2026-11-15"

/* ===================== 录音参数 ===================== */
/* 回溯时长：触摸时上传"刚刚过去"的秒数 */
#define APP_CLIP_SECONDS       30u
/* PDM 采样率（I2S 配置）与落盘 PCM 采样率。
 * 2026-11-15：落盘从 32kHz 降为 8kHz（4:1 低通抽取）。人声电话带足够，
 * SD 写入 64KB/s -> 16KB/s，显著缓解 CPU 被采集任务占满的问题；
 * 上传/回放时再 2× 升采样到 16kHz（服务端 WAV 规格）。
 * 依据见 docs/11-capture-sd-load-analysis.md。 */
#define APP_PDM_SAMPLE_RATE    32000u
#define APP_PCM_SAMPLE_RATE    8000u
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
/* SD 位翻转 SPI 的延时循环数：初始化完成后从 8 切到本值。
 * 8 次空循环实测只有约 10KB/s；1 次可显著提速（需实测误码）。 */
#define APP_SD_FAST_LOOPS      0u

#define APP_SD_START_LBA       2048u
/* 环形写缓冲（必须是 512 的整数倍） */
#define APP_SD_CHUNK_BYTES     4096u

/* ===================== 板间 I2C（ECW 作从机） ===================== */
#define APP_LINK_I2C_NUM       0u
#define APP_LINK_I2C_ADDR7     0x42u
#define APP_LINK_RX_BUF        32u
/* TX 缓冲要容纳一次 LOGO 分块：1 字节序号 + 数据。 */
#define APP_LINK_TX_BUF        320u
/* STM32 开机取图：[0x20, seq, off_lo, off_hi, len_lo, len_hi]
 * 从机回：[seq, data[len]]；seq 不匹配表示读到的是过期/未就绪数据。 */
#define APP_LINK_CMD_LOGO_READ 0x20u
#define APP_LINK_LOGO_CHUNK    200u

#define APP_LINK_CMD_TOUCH     0x01u
#define APP_LINK_CMD_PING      0x02u
/* STM32 写 0x03：喇叭回放最近 N 秒录音（播放期间采集暂停） */
#define APP_LINK_CMD_PLAY      0x03u
/* v2 手势命令（STM32 识别后只写 1 字节）：
 *   0x11 单击 -> 正面情绪音效
 *   0x12 双击 -> 负面情绪音效
 *   0x13 上滑 -> 正面情绪音效
 *   0x14 下滑 -> 负面情绪音效
 * 每条手势命令同时触发一次录音快照/上传（保持 v1 的“触摸即记录”语义）。 */
#define APP_LINK_CMD_GESTURE_CLICK   0x11u
#define APP_LINK_CMD_GESTURE_DOUBLE  0x12u
#define APP_LINK_CMD_GESTURE_UP      0x13u
#define APP_LINK_CMD_GESTURE_DOWN    0x14u

/* 临时诊断：长按/未知手势（0x01）后播放 1 kHz 方波，用于确认 SPK 的
 * 5 根线/格式。产品验收后把 APP_SOUND_TEST_TONE 设为 0。 */
#define APP_SOUND_TEST_TONE      1
#define APP_SOUND_TEST_TONE_HZ   1000u
#define APP_SOUND_TEST_TONE_MS   1000u
#define APP_SOUND_TEST_TONE_I2S  1

/* ===================== 情绪音效（SD 预置资产） ===================== */
/* 音效按 16 kHz / 16-bit / 单声道 PCM 存进 SD 的保留区；回放走与麦克风
 * 共用的 I2S 线。使用 16 kHz 是因为 SDK 已验证的 ES8311 配置是 16 kHz。 */
#define APP_SOUND_SAMPLE_RATE  16000u
#define APP_SOUND_CHANNELS     1u
#define APP_SOUND_BITS         16u
#define APP_SOUND_MAX_ASSETS   8u
/* 音效区从 LBA 1024 开始，占用到环形区起点（LBA 2048）之前，共 512 KB。 */
#define APP_ASSET_START_LBA    1024u
#define APP_ASSET_MAX_BYTES    ((APP_SD_START_LBA - APP_ASSET_START_LBA) * 512u)
/* 资产 PCM 已经归一化到 -3 dBFS，这里默认不再额外衰减；可按喇叭音量调整。 */
#define APP_SOUND_VOLUME_DIV   1u

/* ES8311 codec 控制口。板级 I2C0 = IO24/IO23，与板间链路共用总线；
 * 启动时先用主机模式配置 codec，再关闭主机、打开从机接收 STM32 命令。
 * 若实机确认 codec 不在 I2C0/0x18，可在此关闭并改用外部配置。 */
/* SPK 接口实测是 WAFER-GH1.25-5P（NC/LRCK/BCLK/DIN/GND），不是 ES8311
 * 控制口；喇叭模块是纯 I2S 功放，不需要 I2C 配置。设为 0 可跳过启动时
 * 对 0x18 的失败写入（约 1.5 s）。 */
#define APP_CODEC_CONFIG_ENABLE 0
#define APP_CODEC_I2C_NUM      0u
#define APP_CODEC_I2C_ADDR     0x18u
#define APP_CODEC_VOLUME       0xC0u
/* codec 写寄存器后的稳定延时（SDK 示例值） */
#define APP_CODEC_SETTLE_MS    2u

/* ===================== 喇叭回放 ===================== */
/* 音量衰减分母：越大越轻。SDK i2s_codec 示例的验证值是 10 */
#define APP_PLAYBACK_VOLUME_DIV 10u

/* TX 槽格式：1 = 32-bit 槽 + 16-bit 数据（左对齐）。
 * 参考已实机出声的 firmware-code-ecw-1115：其 I2S 主模式配置是
 * SLOT_FORMAT_LEFT + 32-bit 槽；本板 SPK 模块按该格式才出声。
 * 0 = 16-bit I2S 槽（旧配置，实测无声）。 */
#define APP_SOUND_TX_SLOT32    1

/* ===================== UART TTS（syn6288e） ===================== */
/* 与 firmware-code-ecw-1115 相同的 UART0（IO17 TX / IO2 RX，9600）文本播报。
 * 手势后先播报一句中文提示音，再做 I2S 音效回放。 */
#define APP_TTS_ENABLE         1

/* ===================== 任务参数 ===================== */
#define APP_TASK_CAPTURE_PRIO  5
/* 触发任务要处理板间命令（含 LOGO 分块读取）。它大多数时间阻塞在 20ms
 * 的读超时上，占用极小；必须高于采集任务(5)，否则 SD 位翻转写会把
 * LOGO 取图的响应拖到 40ms 之后，主机只能读到占位数据。 */
#define APP_TASK_TRIGGER_PRIO  6
/* 采集任务（SD 位翻转写）常年处于可运行状态，优先级 3 的网络任务会
 * 拿不到时间片（实测 MQTT 的 TLS 校时 6 分钟无进展）。网络任务与采集
 * 同级，靠时间片轮转共享 CPU。 */
#define APP_TASK_UPLOAD_PRIO   5
#define APP_TASK_NET_PRIO      5
#define APP_TASK_CAPTURE_STACK 4096
#define APP_TASK_TRIGGER_STACK 6144
/* 正式上传要跑 HTTPS/TLS + WAV 降采样 + SHA256，栈给足。 */
#define APP_TASK_UPLOAD_STACK  16384
#define APP_TASK_NET_STACK     8192

/* 触摸事件队列：上传期间的多次触摸不会丢 */
#define APP_UPLOAD_QUEUE_LEN   4

#endif /* APP_CONFIG_H */
