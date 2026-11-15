# 黑壳松 · 美好时刻陪伴宠物（MOCE Desk Pet）

桌面记忆助手：设备持续监听环境声音；用户触摸屏幕「捕捉此刻」后，把过去 N 秒的录音
保存到 SD 并上传服务器，后续可由手机/云端生成「记忆卡片」。

当前仓库保存的是一套**已在实物上跑通主链路**的两板固件：

- **ECW6700F**：录音、SD 环形存储、Wi-Fi 上传、喇叭回放；
- **STM32F407VET6**：2.0 寸触摸屏事件桥，把触摸通过板间 I²C 通知 ECW6700；
  开机驱动 800×480 墨水屏显示一次 LOGO。

> 早期 README 里的「STM32 采音 + PWA + 卡片服务」是长期产品架构设想，不是当前代码。
> 当前版本的完整说明、引脚、构建烧录和排障记录见
> [docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md)。

---

## 一、当前状态（2026-11-15）

| 项目 | 状态 |
|---|---|
| CST816D 触摸 | ✅ 实机验证（chip id 0xB6、单击/双击/上滑/下滑） |
| STM32 → ECW 板间 I²C（0x42） | ✅ 实机验证（单次统计 link_ok=9、fail=0） |
| PDM 麦克风 / I2S 初始化 | ✅ 实机验证；落盘改为 8kHz PCM（4:1 低通抽取） |
| SD 卡（软件 SPI） | ✅ 写约 14KB/s（寄存器直写后），环形区 512MB |
| 触摸回溯片段 | ✅ 30 秒片段（480,000 字节 @8kHz） |
| Wi-Fi 2.4GHz | ✅ 上电约 5 秒拿到 IP；连接前预扫描、SNTP 30 秒保活 |
| **音频内容** | ⚠️ 采集/回放代码已跑通，人声质量未试听确认 |
| **v2 手势识别** | ✅ 实机验证（ECW 收到 0x11–0x14，单击触发音效与录音快照） |
| **v2 情绪音效** | ✅ 喇叭已转到 STM32 SHIELD（I2S2 PB12/13/15），音效数据在 STM32 flash，本地播放实测出声；ECW 侧 TTS（UART0）同时保留 |
| **v2 正式上传** | ⚠️ 202、MQTT audio.export、GET job 认领已实测通过；SD 连续读会干扰 RF，当前快照限制 10 秒，四步上传完整闭环待硬件改善 |
| HTTP 上传（v1 回退） | ⚠️ 客户端保留；服务器 8080 未开放 |
| **喇叭回放** | ⚠️ I2S TX 与音效资产已跑通（`[play] done`），听感待确认 |
| **墨水屏 LOGO** | ✅ SSD1677 系 800×480、九位 SPI，开机 3.7 秒全刷，实机确认 |
| **2.0 寸彩屏 LOGO** | ✅ ST7789 240×320 RGB565，开机显示 LOGO.png（RLE 17.7KB 在 flash）；必须在 EPD 全刷之后再显示 |
| 蓝牙 | ➖ 当前需求不做 |

---

## 二、硬件拓扑

```
2.0TP 触摸屏 ──(12P 排线)── STM32F407_SHIELD1 ── STM32F407VET6
                                                    │
                                        板间 I²C（PB8/PB9 ↔ ECW IO24/IO23）
                                                    │
ECW6700F（YL6700B）: MIC 接口 + 背面 SD 卡槽 + Wi-Fi 天线 + 喇叭（独立 SPK-OUT 口）
```

- **ECW6700F**（YL6700B）外设接口：MIC 连接器（PDM 数字麦）、独立 SPK-OUT
  （U2 WAFER-GH1.25-5P：1=NC、2=LRCK、3=BCLK、4=DIN、5=GND；**纯 I2S，无供电，
  喇叭模块需另接 3.3V/5V**）、SPI 接 SD 卡、UART0 驱动 syn6288e TTS 模块；
  蓝牙为芯片自带能力，当前固件未驱动。
- **STM32F407**（2.0TP 模组）：CST816D 触摸走 I²C3；EPD 墨水屏走 SPI3 共享总线
  （SSD1677 系 800×480、九位字、D/C 固定低），开机全刷显示一次 LOGO。
- 板上拨动开关只切换 CH340 接到哪块板的 UART（ECW 或 STM32）；板间 I²C 不受影响。
- 2.0TP 与 ECW 的完整引脚表见 [docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md) 第 2 节，
  接线与检查清单见 [docs/01-wiring.md](docs/01-wiring.md)。

---

## 三、功能与数据流

```
PDM 麦克风 → I2S → PCM16LE 32kHz 单声道 → SD 裸扇区环形区（约 64KB/s）
STM32 手势识别（单击/双击/上滑/下滑）
  → 板间 I²C 写 1 字节 0x11..0x14
  → ECW 快照最近 N 秒并加入上传队列（保持 v1 语义）
  → ECW 暂停采集 → 播放对应情绪音效（16 kHz）→ 恢复采集
正式上传：POST /device/v1/recordings（设备令牌）
  → MQTT/TLS 收 audio.export / GET 预检查
  → 取最近 N 分钟 → 32k→16k + SHA256
  → POST output 元数据 → PUT WAV → POST result
v1 回退：POST /api/record + POST /api/audio（APP_UPLOAD_FORMAL=0）
调试：STM32 写 0x03 → 回放最近 N 秒录音（32k→16k）
```

- SD 使用裸扇区，从 LBA 2048 起把整张卡当环形缓冲区，不依赖 FATFS。
- 上传期间采集继续写；N 秒窗口远小于环容量，不会被写指针追上。
- 片段大小受实际已缓存音频限制：刚启动时可能只有 10 秒左右，运行满 30 秒后才是完整 1.92MB。

---

## 四、目录

| 路径 | 内容 |
|---|---|
| `ecw6700/` | ECW6700F 固件：采集、SD、环形存储、I²C 从机、音效/回放、Wi-Fi/HTTP/MQTT |
| `ecw6700/main/assets/animal_sounds.bin` | 4 个情绪音效的 16 kHz PCM blob（嵌入固件） |
| `ecw6700/tools/build_sound_blob.py` | 从 `animal_audio/` 生成音效 blob 的脚本 |
| `stm32f407/` | STM32F407 固件：CST816D 触摸/手势、I²C 主机事件、调试串口 |
| `docs/01-wiring.md` | 接线与硬件：总线分工、两板引脚表、检查清单 |
| `docs/06-ecw-bringup-2026-11-15.md` | v1 架构、引脚、构建烧录、排障记录 |
| `docs/07-v2-gesture-sound.md` | v2 手势识别与情绪音效 |
| `docs/08-v2-server-upload.md` | v2 desk-recorder/v1 正式上传 |
| `docs/README.md` | 文档索引 |
| `AGENT.md` | 给 AI/自动化代理的项目工作指南 |

工作区中与构建相关的目录（不在本仓库内）：

| 路径 | 内容 |
|---|---|
| `ecw-build-output/build_firmware.sh` | ECW6700 可复现构建脚本 |
| `ecw-build-work/sdk/` | 解包后的 ESWIN B02P12 SDK |
| `MOCEWorkbench-data/tools/ecw6700-windows/` | ESWIN 编译器、make、eswin_tool |
| `MOCEWorkbench-data/tools/st-windows/` | STM32 的 ARM GCC、BSP、CMake/Ninja |

---

## 五、快速开始

### 5.1 ECW6700F 构建

工程路径含中文时，SDK make 的 `realpath` 会失败。需要一次性建立 ASCII 目录联接：

```powershell
New-Item -ItemType Junction -Path 'C:\moce' -Target '<工作区绝对路径>'
```

```bash
cd /c/moce
# 构建仓库版本（含喇叭回放 app_playback）
MOCE_PROJECT="/c/moce/desk-pet/ecw6700" bash ecw-build-output/build_firmware.sh

# 产物：ecw-build-output/firmware_allinone.bin / firmware_cpu.bin / firmware.elf / firmware.map
```

构建脚本会自动：复用/解包固定 SDK、应用 Windows makefile 补丁、插入 `moce_app_main()`、
**注释掉 `local_ota_main()`**（避免 WPA 扫描卡死）、编译 `main/*.c` + `board_support`、导出镜像。

### 5.2 ECW6700F 烧录

```bash
cd C:/moce/MOCEWorkbench-data/moce-cache-ca0b65ff70a4/flash-jobs
C:/moce/MOCEWorkbench-data/tools/ecw6700-windows/flash/eswin_tool.exe \
  flash --device COM7 --allinoneFile C:/moce/ecw-build-output/firmware_allinone.bin
```

- 板上开关拨到 `ECW6700/ESP32` 一侧；
- ECW 只有 RST 键，下载模式由 CH340 的 RTS 脉冲控制；
- 烧录日志最后出现 `chip start_up` 即成功。

### 5.3 STM32F407 构建

```bash
cd /c/moce
bash desk-pet/stm32f407/build_firmware.sh
# 产物：stm32f407/build/firmware.bin / .hex / .elf / .map
```

脚本会在 `C:\temp\armgcc` 建立指向**工具链根目录**的目录联接，构建目录放在 `C:\temp\moce_stm32_build`。

### 5.4 STM32F407 烧录

- UART Bootloader：115200 **8E1**；
- **BOOT0=1、BOOT1=0，并在 BOOT0 保持高电平期间复位/上电**；
- 写入 `firmware.bin` 到 `0x08000000`；
- 烧完 BOOT0=0，复位运行；串口 115200 8N1 看 USART3 日志。

可用 MOCE Workbench 的 `backend/stm32_uart.py` 做带芯片身份校验的烧录。

---

## 六、配置

所有配置在 `ecw6700/main/app_config.h`，修改后需重新编译烧录：

| 宏 | 说明 |
|---|---|
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | Wi-Fi 凭据（联调时建议固定 2.4GHz 热点） |
| `APP_SERVER_HOST` / `APP_SERVER_PORT` | 服务器地址/端口 |
| `APP_SERVER_EVENT_PATH` / `APP_SERVER_AUDIO_PATH` | HTTP 路径 |
| `APP_CLIP_SECONDS` | 回溯秒数，默认 30 |
| `APP_PDM_SAMPLE_RATE` | I2S 采样率，默认 32000 |
| `APP_SD_START_LBA` | SD 环形区起始 LBA，默认 2048 |
| `APP_PLAYBACK_VOLUME_DIV` | 调试回放音量衰减，默认 10 |
| `APP_SOUND_VOLUME_DIV` | 情绪音效音量衰减，默认 1（资产已归一化） |
| `APP_CODEC_CONFIG_ENABLE` | 本板 SPK 是纯 I2S，默认 0（不执行 ES8311 I2C） |
| `APP_CODEC_I2C_NUM` / `APP_CODEC_I2C_ADDR` | 其它硬件用的 ES8311 控制口（默认 I2C0/0x18） |
| `APP_CODEC_VOLUME` | codec 音量寄存器 0x32（仅启用 codec 时） |
| `APP_SOUND_TEST_TONE` | 1=长按播 1 kHz 测试音（诊断用，验收后设 0） |
| `APP_UPLOAD_FORMAL` | 1=desk-recorder/v1；0=v1 明文 HTTP |
| `APP_DEVICE_TOKEN` | 设备令牌（必填，当前占位符） |
| `APP_MQTT_HOST` / `APP_MQTT_PORT` | MQTT/TLS，默认服务器 IP:8883 |
| `APP_MQTT_USERNAME` / `APP_MQTT_PASSWORD` | MQTT 账号（必填，当前占位符） |
| `APP_RETENTION_MINUTES` | 正式上传的最近分钟数，默认 10 |
| `APP_UPLOAD_QUEUE_LEN` | 上传队列深度，默认 4 |

> 公开仓库中的配置应使用占位符；真实凭据不要提交。

---

## 七、板间协议

| 命令 | 值 | 方向 | 含义 |
|---|---|---|---|
| TOUCH | 0x01 | STM32 → ECW | 仅录音/上传（长按、横向滑动、未知手势） |
| PING | 0x02 | STM32 → ECW | 上电心跳（预留） |
| PLAY | 0x03 | STM32 → ECW | 调试：回放最近 N 秒录音（32k→16k） |
| GESTURE_CLICK | 0x11 | STM32 → ECW | 单击 → 录音/上传 + 正面音效 |
| GESTURE_DOUBLE | 0x12 | STM32 → ECW | 双击 → 录音/上传 + 负面音效 |
| GESTURE_UP | 0x13 | STM32 → ECW | 上滑 → 录音/上传 + 正面音效 |
| GESTURE_DOWN | 0x14 | STM32 → ECW | 下滑 → 录音/上传 + 负面音效 |

- STM32 是 I²C 主机，ECW 是从机；地址 7 位 0x42（HAL 8 位 0x84）。
- 链路只做“主机写 1 字节”，不实现变长帧/应答，避免 HAL 从机时序问题。

---

## 八、服务器通信

v2 默认实现 `desk-recorder/v1` 的**设备主动保存**（`APP_UPLOAD_FORMAL=1`）：

1. `POST /device/v1/recordings`（Bearer 设备令牌 + `Idempotency-Key`）申请保存；
2. MQTT/TLS `commands/events` 收 `audio.export`，或 `GET /device/v1/jobs/{id}` 预检查；
3. 取最近 `APP_RETENTION_MINUTES` 分钟，32k→16k 并计算 SHA256；
4. `POST .../output` 元数据 → `PUT .../output/blob` 流式 WAV → `POST .../result`。

v1 的明文 HTTP（`POST /api/record` + `POST /api/audio`，32kHz/30 秒）保留在
`app_net.c`，设 `APP_UPLOAD_FORMAL=0` 可回退。

正式协议仍需现场提供：`APP_DEVICE_TOKEN`、MQTT 用户名/密码、CA PEM，
以及服务端 presence/telemetry/output/result JSON 的字段定义。详见
[docs/08-v2-server-upload.md](docs/08-v2-server-upload.md)。

---

## 九、文档

- [docs/README.md](docs/README.md) — 文档索引
- [docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md) — v1 实机落地、引脚、构建烧录、关键修复
- [docs/07-v2-gesture-sound.md](docs/07-v2-gesture-sound.md) — v2 手势/音效/资产入库/ES8311 顺序
- [docs/08-v2-server-upload.md](docs/08-v2-server-upload.md) — v2 desk-recorder/v1 上传与待确认 schema
- [ecw6700/README.md](ecw6700/README.md) — ECW6700F 子工程说明
- [stm32f407/README.md](stm32f407/README.md) — STM32F407 子工程说明
- [AGENT.md](AGENT.md) — 给 AI/自动化代理的工作指南

---

## 十、已知限制

- 音频实际内容未验证：需导出 WAV 或服务端保存后确认不是静音/噪声。
- v2 手势已上板验证（STM32 手势/链路/墨水屏正常）；ECW I2S TX 也已完成
  DMA（`[play] done`），但 SPK 模块仍无声：SPK pin 1 是 NC，模块必须
  另接 3.3V/5V；详见 [docs/07](docs/07-v2-gesture-sound.md) 第 7–8 节。
- v2 正式上传已用真实凭据联调：Wi-Fi/MQTT/TLS 均通，服务端 v0.1.1
  `POST /device/v1/recordings` 已存在；在线握手（presence/telemetry）
  刚按服务端规则修正，待复测。详见 [docs/08](docs/08-v2-server-upload.md)。
- 喇叭回放会暂停采集约 1–2 秒，播完自动恢复。
- SD 为裸扇区存储，PC 不能直接读取；LBA 1024–2047 是音效区，
  LBA 2048 之后是录音环形区，都会被覆盖。
- 软件 SPI 约 0.5–1MHz；提高采样率/双声道前需评估总线占用。
- 上传队列深度 4，连续快速触摸会丢；可按产品语义调整。
- 没有 RTC/墙钟，`clip_id` 为自增计数；正式协议的时间戳由服务器给出。
- 没有看门狗；任务卡死只能断电。
- `animal_audio/` 已通过 `build_sound_blob.py` 生成 `animal_sounds.bin` 并被
  固件引用；`app_playback` 同时支持情绪音效和 v1 的片段回放。

---

## 十一、安全注意

公开仓库的 `ecw6700/main/app_config.h` 当前包含真实 Wi-Fi 凭据和服务器地址，
且已存在于 git 历史。**仅修改文件不能消除历史泄露**，请优先修改手机热点密码；
如需可把配置改为占位符，但历史仍保留旧值。编译产物（`.bin/.elf/.hex/.map`、`build/`）、
厂商数据手册 PDF 和含凭据的 zip 均不应提交到公开仓库。
