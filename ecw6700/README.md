# MOCE 记忆时刻主控固件（ECW6700F）

## 职责（2026-11 迭代后）

ECW6700F 是唯一承担"录音 + 存储 + 上传"的板子：

```
PDM 麦克风 --I2S--> 32kHz PCM16 --> 4:1 低通抽取 8kHz --> SPI1 SD 裸扇区环形区（持续写，16KB/s）
                                                    │
STM32 --I2C 从机 0x42 收到 0x01（触摸）───────────────┤
                                                    ▼
                                     取最近 N 秒 -> POST 事件 -> POST WAV -> 服务器
```

- 没有蓝牙、没有墨水屏；
- STM32 只发 1 字节触摸命令，ECW 作 I²C 从机接收；
- 录音持续写入 SD 环形区，触摸时回溯最近 `APP_CLIP_SECONDS` 秒；
- 喇叭经模块上独立的 SPK-OUT 口出声；该口是纯 I2S
  （U2 GH1.25-5P：1=NC、2=LRCK、3=BCLK、4=DIN、5=GND），模块需另接
  3.3V/5V，pin 1 不供电；回放的 I2S 数字线与麦克风
  共用 MIC 连接器（半双工）：STM32 发 `0x03` 时暂停采集，
  把最近 N 秒录音从 I2S TX 回放，播完自动恢复采集；
- Wi-Fi STA 连接由本应用接管（关闭 SDK 的 NV 自动连接）；
- v2 起默认走 `desk-recorder/v1`（HTTPS + MQTT/TLS），v1 明文 HTTP 保留为回退；
- 手势命令 `0x11–0x14` 同时触发录音快照/上传和情绪音效；
- 4 个音效嵌入固件，首次启动写入 SD LBA 1024–2047；
- 开机 LOGO（800×480 1bit，48032 字节块）写入资产区偏移 256 KB（LBA 1536），
  供 STM32 取图；取图走 I2C 命令 `0x20`（magic 0x5A + seq/offset/len/校验），
  但 STM32 端默认关闭该路径（SDK 从机长块 TX 会拉死总线）。
- 详见 [docs/07-v2-gesture-sound.md](../docs/07-v2-gesture-sound.md) 和
  [docs/08-v2-server-upload.md](../docs/08-v2-server-upload.md)。

## 源文件

| 文件 | 作用 |
|---|---|
| `main.c` | 入口、采集任务/触发任务编排、触摸后处理 |
| `app_config.h` | Wi-Fi、服务器、N 秒、引脚等全部配置 |
| `app_audio.c` | I2S PDM 采集 + 三级 64 抽头抽取 + DC blocker |
| `app_sdspi.c` | SPI 模式 SD 卡（CMD0/8/55/41/58/16/9/17/24） |
| `app_store.c` | 裸扇区环形存储与"最近 N 秒"读取 |
| `app_link.c` | I²C 从机 0x42，接收 STM32 触摸命令 |
| `app_playback.c` | 喇叭回放：暂停采集 -> I2S TX 16 kHz -> 恢复采集；ES8311 启动配置 |
| `app_net.c` | Wi-Fi STA + lwip socket + v1 HTTP POST（回退） |
| `app_assets.c` | 音效 blob 校验/首次入库/随机选择/读取 |
| `app_sound.c` | 手势命令 -> 情绪类别；codec 启动任务 |
| `app_resample.c` | 8 kHz -> 16 kHz 线性插值（上传/回放）；4:1 抽取在 app_audio |
| `app_sounds_blob.c` | `.incbin animal_sounds.bin` |
| `app_https.c` | mbedTLS HTTPS 客户端（普通/流式） |
| `app_mqtt.c` | MQTT 3.1.1+TLS、presence/commands |
| `app_upload.c` | desk-recorder/v1 四步上传、WAV/SHA256 |
| `app_sha256.c` / `app_json.c` / `app_ca_cert.c` | 哈希/JSON/CA |
| `board_support/` | 冻结的板级引脚绑定（I2C0 = IO24/23） |

## 必须修改的配置（`main/app_config.h`）

```c
#define APP_WIFI_SSID          "YOUR_WIFI_SSID"
#define APP_WIFI_PASSWORD      "YOUR_WIFI_PASSWORD"
#define APP_SERVER_HOST        "192.168.1.100"
#define APP_SERVER_PORT        8080
#define APP_SERVER_EVENT_PATH  "/api/record"
#define APP_SERVER_AUDIO_PATH  "/api/audio"
#define APP_DEVICE_ID          "moce-01"
#define APP_CLIP_SECONDS       30u
```

未改 SSID 时固件会打印 `[net] Wi-Fi 未配置` 并保持离线，但录音与存储仍然工作。

## 服务器 HTTP 契约（当前实现）

1) 触摸后先发记录事件：

```http
POST /api/record HTTP/1.1
Content-Type: application/json
X-Device-Id: moce-01
X-Clip-Id: 1

{"device":"moce-01","event":"record","clip_id":1,"seconds":30,
 "bytes":1920000,"sample_rate":32000,"format":"pcm_s16le"}
```

2) 紧接着上传 WAV（44 字节头 + PCM16LE 单声道 16kHz，8k 录音升采样）：

```http
POST /api/audio HTTP/1.1
Content-Type: audio/wav
Content-Length: <44 + bytes>
X-Device-Id: moce-01
X-Clip-Id: 1
X-Clip-Seconds: 30

<WAV>
```

body 从 SD 环形区流式读取，不在 RAM 里拼接整段录音。服务器返回 **2xx** 才算成功，
否则重试 2 次；`X-Clip-Id` 可用于服务端幂等去重（重试可能重复发送 body）。

## SD 存储设计

- SPI 模式 SD（6 线），引脚：SCK=GPIO14、MISO=GPIO12、MOSI=GPIO13、CS=GPIO11；
- 不使用 FATFS/文件，从 LBA 2048 起把整张卡当作环形缓冲区；
- 每 4KB 写一次（8 个扇区），64KB/s 对应约 16 次/秒；
- 只记录总写入字节数，"最近 N 秒" = 最后 `N × 16000` 字节，读取时按环偏移回绕；
- 上传期间采集继续写，N 秒窗口远小于环容量，不会被写指针追上；
- 卡容量不足时给出警告，但不会停机；
- 触摸事件进入深度 4 的队列，由独立上传任务消费；上传期间的新触摸不会丢；
- 上传任务最多等 90 秒 Wi-Fi；仍无 IP 则跳过该片段（数据仍在环里，可后续补传）。

**注意**：裸扇区写入会覆盖卡上原文件系统区域（从 LBA 2048 开始）。用于录音的卡不要
再存放其他数据；需要导出音频时按裸 LBA 提取，或在服务器侧保存。

## 构建

工程路径含中文时，SDK 的 `make` 在 `realpath` 阶段会失败。用 ASCII 目录联接运行：

```powershell
# 只需一次：C:\moce -> E:\博一 资料\黑客松\逆向开发
New-Item -ItemType Junction -Path 'C:\moce' -Target 'E:\博一 资料\黑客松\逆向开发'
```

```bash
cd /c/moce
bash ecw-build-output/build_firmware.sh
# 产物：ecw-build-output/firmware_allinone.bin / firmware_cpu.bin / firmware.elf ...
```

如果从中文路径直接运行，脚本会检测到并自动切换到 `C:\moce` 重新执行；
联接不存在时会打印创建命令并以非 0 退出，不会静默失败。

脚本会：复用/解包固定版本 SDK、应用 Windows makefile 补丁、把 `main/*.c` + `board_support`
接入 `Boards/ecw6700f/standalone`、用 ESWIN GCC 14.1.1 构建、导出镜像。

> 注意：MOCE Workbench 的 ECW 构建路径只复制单个 `main.c`，无法处理本工程的多文件结构；
> 本工程请始终使用 `ecw-build-output/build_firmware.sh`。

## 烧录

```bash
cd C:/moce/MOCEWorkbench-data/moce-cache-ca0b65ff70a4/flash-jobs
C:/moce/MOCEWorkbench-data/tools/ecw6700-windows/flash/eswin_tool.exe \
  flash --device COM7 \
  --allinoneFile C:/moce/ecw-build-output/firmware_allinone.bin
```

烧录时板上拨动开关打到 `ECW6700 / ESP32` 一侧；串口 115200 8N1 看启动日志。

## 启动日志（预期）

```
[app] MOCE memory-moment firmware
[app] clip=30s rate=8000Hz 16000B/s
[sd] ready: xxxxx sectors, block addressing
[audio] PDM armed: 32000 Hz, 8192 B/read -> 1024 samples
[link] i2c slave ready: addr=0x42
[net] wifi stack ready
[net] connecting to <ssid> ...
[net] got ip
[app] status: wifi=1 total=... KB ring=... KB
```

## 上板必须验证的点

以下结论来自 SDK/引脚交叉核对，但尚未实机确认：

1. **SPI1 引脚映射**：SD 连接器顺序是 CS/SCK/MISO/MOSI，对应 ECW 端口
   `BOARD_SPI_0={14,12,13}` + `BOARD_GPIO_4=11`，即 **SCK=14、MISO=12、MOSI=13、CS=11**。
   若卡无响应，优先交换 MISO/MOSI。CS 使用 `drv_gpio_open()` 使能 GPIO 时钟；
   时钟：初始化 156kHz，之后 10MHz。
2. **I2S 引脚映射**：MIC 连接器顺序是 WS/SCK/SD，对应 `BOARD_I2S_0={6,7,8,3}`，
   即 **WS=6、SCK=7、SD=8**（第 4 脚 IO3 未接信号）。
   麦克风是 PDM：时钟走 SCK（IO7），数据走 SD（IO8）。
   **本配置假设 SCK 为 PDM 时钟**；若上板无数据，先把 `RXWS`/`RXSCK` 两个值互换，
   并用示波器确认 IO7 上有约 2.048MHz 的连续时钟。
3. **PDM 位序**：32-bit slot 按 MSB-first 解码；用已知音调确认波形不反转、采样率约 32kHz。
4. **回放 TXD 复用**：回放数据线与麦克风共用 MIC 连接器 SD 线（IO8），喇叭经模块上
   独立的 SPK-OUT 口出声（板载 ES8311 codec + 功放）。回放把 IO8 切到
   `FUNC_I2S_TXD`（时钟仍走 RXSCK/RXWS 引脚，主机模式下由控制器驱动）。若喇叭无声，
   先用示波器确认 IO8 上有发送数据、IO7 上有 32kHz×64 位率的连续时钟；TXD 若无法
   复用 IO8，需要改从 SDK 默认 TXD 引脚（IO15/IO25 等）单独接线到功放输入。
5. **I²C 从机地址**：ECW 0x42（7 位），STM32 0x84（8 位）。命令：0x01 触摸、
   0x02 PING、0x03 回放最近 N 秒。
6. **DMA 通道**：I2S RX/TX + SPI TX/RX（全双工两个）+ UART log 共用 6 个通道；
   若 `read_enable` 返回 -4，先关掉 UART 日志 DMA 或减少 SPI DMA。回放期间
   RX 已停、TX 复用其通道，不会超限。
7. **Wi-Fi 竞争**：SDK 启动代码会创建 NV 自动连接任务，本应用已先 `set_auto_connect(false)`
   并 `sta_stop()` 再接管；若日志出现 `sta busy`，等待其结束会自动重试。
8. **DNS**：`getaddrinfo` 在 lwip 上是阻塞调用，无法设置超时。联调阶段请把
   `APP_SERVER_HOST` 填成 **IP 地址**，避免 DNS 不可达时卡住上传任务。

## 编译环境

- Framework：ECW6700FSDK V1.0.0B02P12；目标芯片 ecw6700f；ESWIN GCC 14.1.1（rv32imfb/ilp32f）。
- 只使用 SDK 自带 BSP/HAL/组件；`board_support` 来自冻结的 board-support/v1。
- 云端 Linux 编译与 Windows 本地编译均已通过；编译通过不等于实机功能通过。
