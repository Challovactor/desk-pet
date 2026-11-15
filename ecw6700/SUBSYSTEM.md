# SUBSYSTEM：ECW6700F 记忆时刻主控（rt_ecw）

## 控制职责
- **采集**：PDM 麦克风经 BOARD_I2S_0 持续采集，软件三级 64 抽头抽取为 PCM16 32kHz 单声道。
- **存储**：SPI1 模式 SD 卡（CLK=14 / MOSI=12 / MISO=13 / CS=11），从 LBA 2048 起做裸扇区
  环形缓冲区，持续写入，保留最近 N 秒（默认 30 秒）。
- **触发**：I²C0 作从机（IO24/IO23，地址 0x42），接收 STM32 写入的 1 字节命令
  （0x01 触摸 / 0x02 PING / 0x03 调试回放 / 0x11–0x14 手势）。
- **上传**：Wi-Fi STA 连指定 AP；v2 默认走 desk-recorder/v1
  （HTTPS + MQTT/TLS 四步上传），v1 明文 HTTP 保留为回退。
- **情绪音效**：4 个 16 kHz PCM 音效嵌入固件，首次启动写入 SD LBA 1024–2047；
  手势命令按类别随机选择并通过喇叭播放。
- **回放**：喇叭经模块上独立的 SPK-OUT 口出声（板载 ES8311 codec/功放）；
  回放的 I2S 数字线与麦克风共用 MIC 连接器（半双工）。播放时暂停采集，
  以 16 kHz 从 I2S TX（TXD 复用 IO8）送出，播完自动恢复 32 kHz 采集。
  ES8311 在启动时用 I2C0 主机模式、16 kHz MCLK 配置。
- **韧性**：SD 写失败丢弃当前块并继续采集；Wi-Fi 断线由 SDK 自动重连，30 秒无 IP 强制重启 STA；
  上传失败重试 2 次，采集不中断。

## 对外接口
| 通道 | 端口/引脚 | 协议 |
|---|---|---|
| 板间 I²C（从机） | BOARD_I2C_0 IO24/IO23，7 位地址 0x42 | 主机写 1 字节：0x01 TOUCH / 0x02 PING / 0x03 PLAY |
| 麦克风 | BOARD_I2S_0（IO6/7/8/3） | PDM，32kHz×64bit，软件解码 |
| 喇叭 | 独立 SPK-OUT 口（板载 codec/功放输出） | 回放数据经 MIC 连接器 I2S 线送板载 codec |
| SD 卡 | SPI1（IO14/12/13/11） | SPI 模式 SD，CMD17/24，512B 扇区 |
| 服务器 | Wi-Fi STA + lwip + mbedTLS | HTTPS 443 + MQTT/TLS 8883（desk-recorder/v1）；v1 HTTP 回退 |

## 边界
- 固件不实现蓝牙、墨水屏、TTS：蓝牙为芯片自带能力、TTS 模块保留 UART 接法，均未驱动；
- 不使用 FATFS，不提供 PC 可直接读取的文件系统；
- STM32 只作为触摸输入源，不参与音频/存储/上传。
