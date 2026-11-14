# SUBSYSTEM：ECW6700F 记忆时刻主控（rt_ecw）

## 控制职责
- **采集**：PDM 麦克风经 BOARD_I2S_0 持续采集，软件三级 64 抽头抽取为 PCM16 32kHz 单声道。
- **存储**：SPI1 模式 SD 卡（CLK=14 / MOSI=12 / MISO=13 / CS=11），从 LBA 2048 起做裸扇区
  环形缓冲区，持续写入，保留最近 N 秒（默认 30 秒）。
- **触发**：I²C0 作从机（IO24/IO23，地址 0x42），接收 STM32 写入的 1 字节触摸命令。
- **上传**：Wi-Fi STA 连指定 AP，触摸后先 POST JSON 记录事件，再以 WAV 流式 POST 最近 N 秒。
- **韧性**：SD 写失败丢弃当前块并继续采集；Wi-Fi 断线由 SDK 自动重连，30 秒无 IP 强制重启 STA；
  上传失败重试 2 次，采集不中断。

## 对外接口
| 通道 | 端口/引脚 | 协议 |
|---|---|---|
| 板间 I²C（从机） | BOARD_I2C_0 IO24/IO23，7 位地址 0x42 | 主机写 1 字节：0x01 TOUCH / 0x02 PING |
| 麦克风 | BOARD_I2S_0（IO6/7/8/3） | PDM，32kHz×64bit，软件解码 |
| SD 卡 | SPI1（IO14/12/13/11） | SPI 模式 SD，CMD17/24，512B 扇区 |
| 服务器 | Wi-Fi STA + lwip | HTTP POST（JSON 事件 + WAV） |

## 边界
- 不做蓝牙、墨水屏、TTS；
- 不使用 FATFS，不提供 PC 可直接读取的文件系统；
- STM32 只作为触摸输入源，不参与音频/存储/上传。
