# 黑壳松 · 美好时刻陪伴宠物（MOCE Desk Pet）

桌面记忆助手：设备持续监听环境声音；用户触摸屏幕「捕捉此刻」后，把过去 N 秒的录音
保存到 SD 并上传服务器，后续可由手机/云端生成「记忆卡片」。

当前仓库保存的是一套**已在实物上跑通主链路**的两板固件：

- **ECW6700F**：录音、SD 环形存储、Wi-Fi 上传、喇叭回放；
- **STM32F407VET6**：2.0 寸触摸屏事件桥，把触摸通过板间 I²C 通知 ECW6700。

> 早期 README 里的「STM32 采音 + PWA + 卡片服务」是长期产品架构设想，不是当前代码。
> 当前版本的完整说明、引脚、构建烧录和排障记录见
> [docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md)。

---

## 一、当前状态（2026-11-15）

| 项目 | 状态 |
|---|---|
| CST816D 触摸 | ✅ 实机验证（chip id 0xB6、坐标正常） |
| STM32 → ECW 板间 I²C（0x42） | ✅ 实机验证 |
| PDM 麦克风 / I2S 初始化 | ✅ 初始化成功 |
| SD 卡（软件 SPI） | ✅ 16GB 卡、512MB 环形区、持续写入 |
| 30 秒回溯片段 | ✅ 触摸生成 1,920,000 字节片段 |
| Wi-Fi 2.4GHz | ✅ 扫描、关联、获取 IP |
| **音频实际内容** | ⚠️ 仅验证初始化与写卡，未回放/分析 PCM |
| **HTTP 上传** | ⚠️ 客户端已实现；测试时服务器不可达 |
| **喇叭回放** | ⚠️ 代码已编译进镜像，未实机试听 |
| 墨水屏 / 蓝牙 | ➖ 当前需求不做 |

---

## 二、硬件拓扑

```
2.0TP 触摸屏 ──(12P 排线)── STM32F407_SHIELD1 ── STM32F407VET6
                                                    │
                                        板间 I²C（PB8/PB9 ↔ ECW IO24/IO23）
                                                    │
ECW6700F（YL6700B）: MIC 接口 + 背面 SPI SD 卡槽 + Wi-Fi 天线 + 喇叭
```

- 板上拨动开关只切换 CH340 接到哪块板的 UART（ECW 或 STM32）；板间 I²C 不受影响。
- 2.0TP 与 ECW 的完整引脚表见 [docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md) 第 2 节。

---

## 三、功能与数据流

```
PDM 麦克风 → I2S → PCM16LE 32kHz 单声道 → SD 裸扇区环形区（约 64KB/s）
触摸（ECW I²C 从机 0x42 收到 0x01）
  → 快照最近 N 秒（默认 30 秒）
  → 上传队列（深度 4）
  → POST /api/record（JSON 事件）
  → POST /api/audio（WAV，流式读取）
STM32 写 0x03 → ECW 暂停采集 → I2S TX 回放最近 N 秒 → 恢复采集
```

- SD 使用裸扇区，从 LBA 2048 起把整张卡当环形缓冲区，不依赖 FATFS。
- 上传期间采集继续写；N 秒窗口远小于环容量，不会被写指针追上。
- 片段大小受实际已缓存音频限制：刚启动时可能只有 10 秒左右，运行满 30 秒后才是完整 1.92MB。

---

## 四、目录

| 路径 | 内容 |
|---|---|
| `ecw6700/` | ECW6700F 固件：采集、SD、环形存储、I²C 从机、喇叭回放、Wi-Fi/HTTP |
| `stm32f407/` | STM32F407 固件：CST816D 触摸、I²C 主机事件、调试串口 |
| `docs/06-ecw-bringup-2026-11-15.md` | 当前版本的架构、引脚、构建烧录、排障记录 |
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
| `APP_PLAYBACK_VOLUME_DIV` | 喇叭回放音量衰减，默认 10 |
| `APP_UPLOAD_QUEUE_LEN` | 上传队列深度，默认 4 |

> 公开仓库中的配置应使用占位符；真实凭据不要提交。

---

## 七、板间协议

| 命令 | 值 | 方向 | 含义 |
|---|---|---|---|
| TOUCH | 0x01 | STM32 → ECW | 触摸，取最近 N 秒并上传 |
| PING | 0x02 | STM32 → ECW | 上电心跳（预留） |
| PLAY | 0x03 | STM32 → ECW | 喇叭回放最近 N 秒（回放期间暂停采集） |

- STM32 是 I²C 主机，ECW 是从机；地址 7 位 0x42（HAL 8 位 0x84）。
- 链路只做“主机写 1 字节”，不实现变长帧/应答，避免 HAL 从机时序问题。

---

## 八、服务器通信

当前固件实现的是**可配置明文 HTTP**：

1. `POST /api/record`：JSON 记录事件（`clip_id`、秒数、字节数、采样率）；
2. `POST /api/audio`：WAV（44 字节头 + PCM16LE 单声道 32kHz），body 从 SD 流式读取；
3. 只有 HTTP 2xx 才算成功，失败重试 2 次；`X-Clip-Id` 可用于服务端幂等去重。

服务器侧的正式协议（`desk-recorder/v1`）是 **MQTT 3.1.1+TLS（8883）+ HTTPS（443）**、
App 发起保存任务、16kHz/单声道/16-bit WAV、UTC 校时、SHA256、四步上传和任务持久化。
它与当前实现**不兼容**，按当前产品决定暂缓改造；接入前需要设备凭据、CA 证书、
服务端端口放行，以及音频格式/保留分钟数/校时/持久化等改动。详见
[docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md) 第 6 节。

---

## 九、文档

- [docs/README.md](docs/README.md) — 文档索引
- [docs/06-ecw-bringup-2026-11-15.md](docs/06-ecw-bringup-2026-11-15.md) — 实机落地、引脚、构建烧录、9 条关键修复、服务器协议现状、安全与限制
- [ecw6700/README.md](ecw6700/README.md) — ECW6700F 子工程说明
- [stm32f407/README.md](stm32f407/README.md) — STM32F407 子工程说明
- [AGENT.md](AGENT.md) — 给 AI/自动化代理的工作指南

---

## 十、已知限制

- 音频实际内容未验证：需导出 WAV 或服务端保存后确认不是静音/噪声。
- HTTP 上传未闭环：测试环境服务器不可达（PC 测试 80/8080 均超时）。
- 喇叭回放未实机试听；回放会暂停采集，播完恢复。
- SD 为裸扇区存储，PC 不能直接读取；LBA 2048 之后的数据会被覆盖。
- 软件 SPI 约 0.5–1MHz；提高采样率/双声道前需评估总线占用。
- 上传队列深度 4，连续快速触摸会丢；可按产品语义调整。
- 没有 RTC/墙钟，`clip_id` 为自增计数；接入正式服务器前需补 UTC 时间。
- 没有看门狗；任务卡死只能断电。
- `叫声音效/` 的音效资产当前未被代码引用；`app_playback` 回放的是刚录制的片段。

---

## 十一、安全注意

公开仓库的 `ecw6700/main/app_config.h` 当前包含真实 Wi-Fi 凭据和服务器地址，
且已存在于 git 历史。**仅修改文件不能消除历史泄露**，请优先修改手机热点密码；
如需可把配置改为占位符，但历史仍保留旧值。编译产物（`.bin/.elf/.hex/.map`、`build/`）、
厂商数据手册 PDF 和含凭据的 zip 均不应提交到公开仓库。
