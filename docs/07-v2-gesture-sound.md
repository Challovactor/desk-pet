# 07 · v2 手势识别与情绪音效

> 状态：代码已实现并通过编译；**未上板验证**。ES8311 的 I²C 控制口在现有
> 资料中没有明确原理图依据，见第 8 节。

## 1. 目标

在 v1「触摸即录音/上传」的基础上，新增：

1. STM32 识别 4 种手势：单击、双击、上滑、下滑；
2. ECW 根据手势播放 2 类情绪音效（同类随机），同时保留一次录音快照/上传；
3. 4 个音效在烧录/首次启动时写入 SD 保留区，运行时不再依赖网络或文件系统。

## 2. 数据流

```
CST816D (I2C3) ──寄存器 0x01 GestureID──> STM32 手势状态机
                                              │ 板间 I2C 0x42，1 字节
                                              ▼
                                   ECW trigger_task
                                    ├─ handle_touch()：最近 30 s 快照 → 上传队列
                                    └─ sound_play_gesture()：暂停采集 → I2S TX 16 kHz
                                                              播放 SD 情绪音效 → 恢复采集
```

- 手势命令**同时**触发录音和音效；这保持了 v1 的“每次触摸都记录”语义。
- 手势在**释放后**才发送（单击延迟 300 ms 以区分双击），因此录音快照发生在
  手势完成时。录音环形区是持续的，触摸瞬间的音频仍在最近 30 秒窗口内。

## 3. 手势映射（`app_config.h` / `app_sound.c`）

| 手势 | STM32 → ECW 命令 | 情绪类别 | 音效文件 |
|---|---|---|---|
| 单击 | `0x11` | 正面 | 02_joy / 04_murmur |
| 上滑 | `0x13` | 正面 | 02_joy / 04_murmur |
| 双击 | `0x12` | 负面 | 01_sad / 03_cry |
| 下滑 | `0x14` | 负面 | 01_sad / 03_cry |
| 长按/横向滑动/未知 | `0x01` | 仅录音，不播音效 | — |

- 同类音效由 `assets_pick()` 用 xorshift 随机选择，并避免连续两次同一文件。
- 映射集中在 `app_sound.c:gesture_class()`；改产品语义只改这一个函数。

## 4. STM32 识别策略

### 4.1 硬件 GestureID（首选）

CST816 系列在寄存器 `0x01` 的第 0 字节给出 GestureID（Hynitron 常见编码）：

| 值 | 手势 |
|---|---|
| 0x01 | 上滑 |
| 0x02 | 下滑 |
| 0x03/0x04 | 左/右滑 |
| 0x05 | 单击 |
| 0x0B | 双击 |
| 0x0C | 长按 |

`stm32f407/main/main.c` 每 20 ms 读 6 字节，`d[0]` 即 GestureID。
同一个值连续多轮保持非 0 时只处理一次。

### 4.2 软件兜底

不同 CST816 批次/固件可能不填 GestureID。释放后等 `GESTURE_GRACE_MS=120 ms`，
仍没有硬件手势就用坐标/时长判定：

- 时长 < 400 ms、位移 < 25 px → 单击（300 ms 内第二次 → 双击）；
- 垂直位移 > 45 px 且大于水平位移 → 上/下滑；
- 水平位移 > 45 px → 仅录音；
- 时长 ≥ 600 ms 且几乎不动 → 长按（仅录音）。

### 4.3 单击延迟

单击必须先等 300 ms，确认没有第二击后才发送 `0x11`；双击在第二击到达时
取消待定单击并发送 `0x12`。这样避免“单击音效 + 双击音效”同时触发。

## 5. 音效资产

### 5.1 生成

```bash
python ecw6700/tools/build_sound_blob.py --wav-dir ecw6700/tools/preview_wav
# 产物：ecw6700/main/assets/animal_sounds.bin
```

脚本把 `animal_audio/` 的 4 个文件解码为 **16 kHz / 16-bit / 单声道 PCM**，
做静音裁剪、统一峰值（-3 dBFS）和 8 ms 淡入淡出，然后打包成自定义 blob。
`--wav-dir` 会额外输出可试听的 WAV。

依赖：`python -m pip install miniaudio`。脚本从内存解码，避免 Windows 下
miniaudio 对中文路径的兼容问题。

### 5.2 Blob 格式（小端）

```
header 32 B: "MOCESND1", version=1, header_size=32,
             sample_rate=16000, channels=1, bits=16, count=4,
             total_bytes, blob_crc32
entry 40 B × 4: name[16], class_id(1=负面,2=正面), reserved[3],
                offset(512 对齐), bytes, pcm_crc32, duration_ms, reserved
PCM: 每个 entry 从 512 字节边界开始，PCM16LE 单声道
```

### 5.3 烧录时入库 SD

- `app_sounds_blob.c` 用 GCC `.incbin "animal_sounds.bin"` 把 blob 放进
  固件 `.rodata`（约 191 KB，镜像从 1.56 MB 增至 1.76 MB）。
- 构建脚本在 staging 阶段把 `main/assets/*.bin` 复制到 SDK 板级目录；
  `.incbin` 以编译工作目录为基准。**修改构建脚本时必须保留这一步。**
- `app_assets.c` 在每次启动时：
  1. 校验固件内 blob 的魔数/版本/大小/CRC；
  2. 读取 SD LBA 1024 的头部并流式校验整块 CRC；
  3. 不一致时把 blob 按 4 KB 分块写到 LBA 1024，再回读校验。
- 资产区 LBA 1024..2047（512 KB），录音环形区从 LBA 2048 开始，
  两者永不重叠。首次启动写入约 3–5 秒（软件 SPI），之后启动只做校验。

## 6. 回放

- 统一 **16 kHz / 16-bit / 立体声 TX**（单声道复制到左右声道），与 SDK
  已验证的 ES8311 16 kHz 配置一致。
- `app_playback.c` 的通用播放器支持两种源：
  - 情绪音效：16 kHz，直接播放；
  - v1 调试命令 `0x03`（最近 N 秒录音）：8 kHz 落盘，经 2× 插值到 16 kHz 播放，
    `app_resample.c` 二阶 Butterworth 低通 + 2:1 抽取后播放。
- 播放前暂停采集（半双工共用 I2S 数据线），播完重新 `audio_init()` 恢复
  32 kHz PDM 采集。播放期间采集任务每 20 ms 轮询暂停标志。
- 音量：资产已归一化到 -3 dBFS；`APP_SOUND_VOLUME_DIV` 默认 1，
  `APP_CODEC_VOLUME` 默认 0xC0。

## 7. 音频输出与 SPK 接口（2026-11-15 实测）

SPK 接口是 **U2 WAFER-GH1.25-5PWB**，接线见
`docs/interface/SPK引脚接线.jpg`：

| SPK pin | 信号 | 说明 |
|---|---|---|
| 1 | NC | 板上悬空（绿叉），**不供电** |
| 2 | I2S_WS → LRCK | 16 kHz 帧同步 |
| 3 | I2S_SCK → BCLK | 16k × 32 = 512 kHz |
| 4 | I2S_SD → DIN | 音频数据 |
| 5 | GND | 必须共地 |
| shield 6/7 | GND | 固定脚 |

结论：

- 这个口是**纯 I2S 输出**，不含 3V3/VIN，也不是 ES8311 的 I²C 控制口。
  喇叭模块必须**单独接 3.3V/5V 和 GND**；pin 1 悬空是该模块不上电的
  首要嫌疑。
- 因此 `APP_CODEC_CONFIG_ENABLE=0`：本板不执行 v1 的 ES8311 37 寄存器
  序列（在 SPK 口上 0x18 始终 NACK）。`app_playback.c` 里的 ES8311
  代码保留给其它硬件，但本板不走。
- 回放统一 **标准 I2S**（`SLOT_FORMAT_I2S`）、16 kHz/16-bit、单声道复制
  到立体声；I2S TX 引脚为 IO6=TXWS、IO7=TXSCK、IO8=TXD。
- 临时诊断：长按/未知手势（`0x01`）会播放 **1 kHz 方波 1 秒**，
  日志 `[play] test tone 1000 Hz ... I2S format`。产品验收后把
  `APP_SOUND_TEST_TONE` 设为 0。

## 8. 上板核对点

1. **SPK 模块供电**：SPK pin 1 为 NC；模块的 VIN 必须接外部 3.3V/5V，
   GND 接 SPK pin 5。只接 5 根线但不给 VIN 上电，一定无声。
2. **SPK 线序**：模块的 BCLK/LRC/DIN 分别接 SPK pin 3/2/4；若模块丝印
   是 VIN/GND/BCLK/LRC/DIN，按功能对应，不要按 pin 号硬套。
3. **格式**：模块应为标准 I2S。若测试音仍无声，可把
   `APP_SOUND_TEST_TONE_I2S` 设为 0 试 left-justified，或检查模块是否
   需要 MCLK（本接口没有 MCLK）。
4. **CST816D GestureID**：若日志中 gesture 始终为 0，软件兜底仍可工作。
5. **半双工共用数据线**：回放会暂停采集约 1–2 秒；MIC 与 SPK 共用
   WS/SCK/SD，硬件上需确认没有同时驱动。

## 9. 验证清单

- [ ] ECW 启动日志：`[asset] installed`（首次）或 `[asset] ready: 4 sounds`
- [ ] ECW 启动日志：`[sound] codec task done, ok=1`（或明确的 NOT configured）
- [ ] STM32 日志：`[stm32] ... -> cmd 0x11/0x12/0x13/0x14`
- [ ] ECW 日志：`[sound] gesture 0x.. -> positive/negative class`
- [ ] ECW 日志：`[play] sound <name> (...)` 和 `[play] done`
- [ ] 喇叭听到对应音效，采集在播放后恢复（`[app] total` 继续增长）
- [ ] 每次手势仍产生 `[app] clip N queued`
- [ ] 双击只产生一条 `0x12`，不是两条 `0x11`
