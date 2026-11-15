# 08 · v2 正式服务器上传（desk-recorder/v1）

> 状态：传输层、四步 HTTP、MQTT/TLS、WAV 生成与 SHA256 已实现并通过编译。
> 2026-11-15 实机 + 服务端联调结果：
>
> - `39.105.87.87:443/8883` 可达；CA 校验通过；服务器证书 CN=`39.105.87.87`。
> - HTTPS 设备令牌、MQTT/TLS 登录均成功；设备能连上 Wi-Fi、拿到 IP、
>   连上 MQTT 并订阅 `commands/events`。
> - 服务端已部署录音核心 v0.1.1，`POST /device/v1/recordings` 路由存在，
>   但当前返回 `409 DEVICE_OFFLINE`。
> - 服务端诊断确认：`presence` 缺 `online` 布尔值（服务器报
>   `INVALID_ONLINE_BOOLEAN`），且没有收到有效 `telemetry`；设备数据库
>   `last_seen/boot_id/connection_id` 为空、`seq=0`。**不是账号/密码问题。**
> - 修正后的规则见第 7 节；固件已按此实现（`online` 布尔、每 5 秒
>   telemetry、HTTPS Date 校时、重连换 connection_id、LWT 离线遗嘱）。
> - 仍需现场验证 `POST output` / `PUT blob` / `POST result` 的字段名。

## 1. 与 v1 的区别

| | v1 | v2 |
|---|---|---|
| 触发 | 触摸 → 最近 30 s | 手势 → 最近 N 分钟 |
| 协议 | 明文 HTTP | HTTPS 443 + MQTT/TLS 8883 |
| 音频 | 32 kHz | 16 kHz（读取时 2:1 降采样） |
| 鉴权 | 无 | Bearer `device_token` + `X-Device-Id` |
| 任务 | 设备直接 POST | 设备申请保存 → 服务端 MQTT 下发 `audio.export` → 四步上传 |
| 完整性 | 无 | 标准 44 B WAV 头 + SHA256 |

v1 的 `POST /api/record` + `POST /api/audio` 代码仍保留在 `app_net.c`，
通过 `APP_UPLOAD_FORMAL=0` 可回退；默认走正式协议。

## 2. 流程

```
触摸/手势
  → upload_task 取到 clip_job（含一次性 Idempotency-Key）
  → 等 Wi-Fi + MQTT presence
  → POST /device/v1/recordings            (202, job_id/recording_id/state)
  → 等 MQTT commands/events 或轮询 GET /device/v1/jobs/{id}
  → 取 minutes（服务端 params，缺省 APP_RETENTION_MINUTES=10）
  → 第一遍读 SD：32k→16k 降采样 + 计算 WAV SHA256
  → POST /device/v1/jobs/{id}/output      (WAV 元数据)
  → PUT  /device/v1/jobs/{id}/output/blob (44 B 头 + 16k PCM，流式)
  → POST /device/v1/jobs/{id}/result      (succeeded/failed)
  → MQTT telemetry
```

### 2.1 为什么读两遍 SD

`POST /output` 必须在 `PUT blob` 之前提交 `size_bytes` 和 `sha256`，而
SHA256 只有读完整个 WAV 才能得到。第一遍只算哈希、丢弃样本；第二遍
再读同一区间并流式上传。两次使用相同的 `offset/src_bytes` 和相同的
降采样器初值，输出必须逐字节一致。

- 10 分钟：SD 读约 2 × 38.4 MB；软件 SPI 约 0.5–1 MB/s，预计 1.5–3 分钟。
- 60 分钟：WAV 恰好 115,200,044 字节，达到服务端上限。

## 3. 音频格式

- 输出：`16000 Hz / 1 ch / 16 bit PCM`，标准 44 字节 WAV 头。
- 输入：环形区 `32000 Hz / 1 ch / 16 bit`。
- `app_resample.c`：二阶 Butterworth 低通（fc≈7.2 kHz，Q=0.707）后每 2 个
  样本取 1 个；状态跨块保留，两遍处理都从同一初值开始。
- 时长/大小：
  `out_samples = src_samples / 2`，`wav_data = out_samples × 2`，
  `size_bytes = 44 + wav_data`。

## 4. 配置（`ecw6700/main/app_config.h`）

```c
#define APP_UPLOAD_FORMAL      1
#define APP_SERVER_HOST        "39.105.78.78"   // 或服务器域名
#define APP_SERVER_HTTPS_PORT  443
#define APP_DEVICE_ID          "moce-01"
#define APP_DEVICE_TOKEN       "YOUR_DEVICE_TOKEN"

#define APP_MQTT_HOST          APP_SERVER_HOST
#define APP_MQTT_PORT          8883
#define APP_MQTT_USERNAME      "YOUR_MQTT_USERNAME"
#define APP_MQTT_PASSWORD      "YOUR_MQTT_PASSWORD"

#define APP_RETENTION_MINUTES  10u
```

CA 证书：已把 `server/key.txt` 的 CA PEM 写入
`ecw6700/main/app_ca_cert.c` 的 `s_ca_pem[]`。服务器证书 CN 是
`39.105.87.87`，与 `APP_SERVER_HOST` 一致。

> 当前工作区的 `app_config.h` 已填入真实设备令牌和 MQTT 密码；
> `server/key.txt` 已加入 `.gitignore`。**不要把这两个文件或编译产物提交到公开仓库。**

> **主机名**：`mbedtls_ssl_set_hostname()` 使用 `APP_SERVER_HOST`。用 IP
> 连接时服务器证书必须带 IP SAN；否则请把 `APP_SERVER_HOST` 改成证书
> CN/SAN 中的域名（并确保设备能 DNS 解析）。

## 5. 代码地图

| 文件 | 职责 |
|---|---|
| `app_https.c` | 最小 TLS 客户端：CA 校验、SNI、普通/流式请求、响应解析 |
| `app_mqtt.c` | MQTT 3.1.1+TLS、订阅 commands/events、presence/telemetry、命令提取 |
| `app_upload.c` | 四步 HTTP 状态机、WAV 头、32k→16k、SHA256、流式 PUT |
| `app_sha256.c` | mbedTLS SHA256 封装 |
| `app_json.c` | 极简 JSON 取值（不是通用解析器） |
| `app_ca_cert.c` | CA 证书占位符/部署替换点 |

## 6. 服务端接口约束（来自 `server/API.md`）

- 设备 HTTP：`Authorization: Bearer <device_token>` + `X-Device-Id`。
- `POST /device/v1/recordings`：body `{}`，必须带 `Idempotency-Key`
  （8–128 字符 ASCII 字母/数字/短横线/下划线；同一次操作的所有重试复用）。
- 成功为 `202`；`state` 可能已是 `succeeded`/`failed`/`expired`。
- 服务器只允许**一台设备一个活动任务**；`409 BUSY/OFFLINE` 需要等待或检查
  MQTT 心跳。
- 保存范围是命令执行时的最近 N 分钟，不是按键瞬间的固定快照。
- `GET /device/v1/jobs/{job_id}` 同时是任务预检查；它会标记任务已被访问。
- `POST /output` 重复提交必须完全一致；配额不足返回 `507`。
- `PUT blob` 的 `Content-Length` 必须与元数据一致；服务器核对 WAV 头与 SHA256。
- WAV 上限 115,200,044 字节，固定 16 kHz/单声道/16-bit。
- 任务开始期限 55 秒、完成期限 23 小时；重复结果幂等。

## 7. MQTT 在线协议（服务端 2026-11-15 确认）

服务端用 **presence + telemetry** 判断在线；`API.md` 的摘要不够，下面是
服务端诊断给出的实际规则，固件已按此实现：

### 7.1 presence

主题 `desk/v1/devices/{device_id}/presence`，QoS1、retain=true：

```json
{
  "protocol": "desk-recorder/v1",
  "device_id": "platform-test-02",
  "boot_id": "<32位小写hex>",
  "connection_id": "<32位小写hex>",
  "online": true
}
```

- `online` 必须是 JSON 布尔值，不能是 `"true"`/`1`/`state` 字符串；
  服务器对缺少该字段的消息报 `INVALID_ONLINE_BOOLEAN`。
- CONNECT 前设置相同 `boot_id`/`connection_id`、`online:false` 的
  **LWT（遗嘱）**，QoS1、retain=true；断线时服务器据此转离线。
- `boot_id` 每次上电生成一次；`connection_id` 每次 MQTT 重连重新生成，
  同时重设遗嘱。

### 7.2 telemetry

主题 `desk/v1/devices/{device_id}/telemetry`，**每 5 秒**、QoS0、
retain=false。字段：

```json
{
  "protocol": "desk-recorder/v1",
  "device_id": "platform-test-02",
  "boot_id": "<32位小写hex>",
  "connection_id": "<32位小写hex>",
  "seq": 1,
  "sent_at_ms": 1794685238000,
  "busy": false,
  "sd_ready": true,
  "capabilities": ["audio.export"],
  "audio": {
    "state": "recording",
    "available_seconds": 240,
    "retention_minutes": 10,
    "sample_rate": 16000,
    "channels": 1,
    "bits_per_sample": 16
  }
}
```

- `sent_at_ms` 是 UTC Unix **整数毫秒**，服务器只接受收到时刻
  **-15 秒 ~ +5 秒**。设备没有 RTC，固件用一次 `HTTPS GET /` 的
  `Date:` 头换算 offset（`app_https.c:parse_date_ms`）。
- `seq` 每个 MQTT 连接从 0 重新计数，严格递增正整数。
- `boot_id`/`connection_id` 32 位小写十六进制，不带 UUID 减号。
- `busy`/`sd_ready` 布尔，`capabilities` 数组，`audio` 对象，不能省略。
- `capabilities` 只声明已实现的命令；当前声明 `audio.export`。
- presence 与 telemetry 的 `boot_id`/`connection_id` 必须一致。

### 7.3 连接使用方式

- 同一设备身份**只能有一个程序**使用 `platform-test-02` 这个 Client ID；
  电脑诊断请用 `diag-platform02-<随机>`，且不要同时以同一 `device_id`
  发 presence/telemetry，否则服务器的设备会话会互相覆盖。
- 服务端 90 秒无有效 telemetry 转离线。

### 7.4 仍待现场确认

- `POST /output` 元数据、`PUT blob`、`POST /result` 的字段名（固件当前
  按 API.md 语义构造，见 `build_output_json()`/`build_result_json()`）；
- `audio.export` 命令是否带 `minutes`，以及 `audio.configure` 的
  `/result` 回执格式。

## 8. 已知限制

- **在线握手刚修正，待复测**：服务端 v0.1.1 已部署，设备能连 Wi-Fi/MQTT；
  之前 409 DEVICE_OFFLINE 的原因是 presence 缺 `online` 布尔、telemetry
  没有有效时间戳/5 秒心跳。固件已按第 7 节修正，复测通过前不把上传算作
  已验证。
- **协议剩余未知**：`POST output`/`PUT blob`/`POST result` 的字段名仍
  按 API.md 语义猜测；第一次成功创建 job 后按服务端回执调整。
- **音频格式**：服务器固定 16 kHz；录音仍是 32 kHz，上传时降采样。
- **任务持久化**：设备重启会丢失未完成的 save 请求；服务端的任务预检查/
  23 小时保留机制可以部分弥补，但设备侧没有持久队列。
- **`audio.configure`**：MQTT 收到后会把 `minutes` 更新到 RAM（1–60），
  但还没有给该配置任务回 `/result`；重启后回到 `APP_RETENTION_MINUTES`
  （默认 10 分钟）。
- **内存**：MQTT/TLS 与 HTTPS/TLS 各有一套 mbedTLS 上下文。若同时连接时
  堆不足，需要降低 mbedTLS 缓冲区、复用 TLS 连接或临时停止 MQTT。
- **TLS 1.2**：SDK defconfig 只启用 TLS 1.2，服务端必须允许 1.2。
- **无 RTC**：固件用 `HTTPS GET /` 的 `Date` 头换算 UTC offset；首次校时
  失败时 `sent_at_ms=0`，服务端会拒绝该轮 telemetry（日志
  `[mqtt] time sync failed`）。
- **单活动任务**：上传期间的新触摸会进入深度 4 的队列；服务器忙碌时后续
  任务可能失败，需要产品语义决定“丢弃/合并/只保留最后一次”。

## 8.1 会话 ID 必须真随机（2026-11-15 服务器排查结论）

服务器返回 `BOOT_CHANGED_WITHIN_SAME_CONNECTION`，并拒绝全部 telemetry →
设备被判离线 → `POST /device/v1/recordings` 一直 **409**（DEVICE_OFFLINE）。

- 根因：`app_mqtt.c` 的 `make_hex_id()` 原实现用 `os_time_get()`（开机计时）
  做 LCG 种子。每次启动在同一时刻调用就得到**完全相同的 32 位十六进制**，
  数据库看到"同一 connection_id 配不同 boot_id"，按协议拒绝。
- 修复：改用硬件 TRNG（`drv_trng_fill()`），再异或时间/盐兜底；
  连接前打印 `[mqtt] session boot=... conn=...` 便于与服务端日志对账。
- 协议要求（服务器方给出）：
  - `boot_id`：每次启动生成一次，运行期间不变；
  - `connection_id`：**每次 MQTT 连接前重新生成**（含同一次启动内的断线重连），
    不能写死、不能只由 device_id/MAC 派生；
  - `seq`：新 connection_id 下从 1 开始，连接内递增；
  - LWT 使用本次 boot_id/connection_id；presence 与 telemetry 同一套 ID；
  - 两者均为 32 位小写十六进制（无 UUID 减号）。
- 副作用：同一次启动内多次重连会产生多个 connection_id，这是协议要求的行为。

## 8.2 202 已接受但 job_id 读不到（2026-11-15）

- 会话 ID 改为 TRNG 后，服务器第一次返回 **`POST recordings: http 202`**，
  设备在线状态修复。
- 但 202 的 body（含 `job_id/state/recording_id`）没有解析出来：
  `[https] 202 (260 bytes)` 之后打印 `202 without job_id`。
- 根因：`https_read_response()` 在 `MBEDTLS_ERR_SSL_TIMEOUT` 处直接结束读取，
  而服务端的 body 常在后一个 TLS 记录里到达（头部 200 字节 + body 60 字节）。
- 修复：头部已收到且 `Content-Length` 尚未满足（或 chunked 未知长度）时，
  最多再等约 300ms（150×2ms）继续读；同时打印所有非空响应体
  （`[https] body: ...`），便于看到 `job_id` 和业务错误码。
- 后续行为：同一设备只允许一个活动任务，第一次 202 之后、任务完成前的
  新 `POST /recordings` 会返回 **409 BUSY**（API.md 的预期语义）。

## 8.3 首次完整解析 202 与后续网络中断（2026-11-15 11:57）

- 修复 body 读取后，202 返回 694 字节并成功解析：
  `job_id=job-eb161a7d6c404287a7b5f64a1cdc1e46`、
  `recording_id=rec-13f5ab3296f644d2b5804d8a1a5ed1c7`、`state=queued`；
- 紧接着 `GET /jobs/{id}` 返回 200，任务预检查通过；
- 随后到 `39.105.87.87:443` 的 TCP 连接持续失败（`-0x0044`），MQTT 也反复
  `transport error`，但 **Wi-Fi 链路始终在线**（`wifi=1`、ARP 心跳正常）：
  这是服务器/蜂窝网络路径中断，不是 AP 踢线；
- 上传任务在服务器不可达时走了失败路径（`POST output: http -1` →
  `POST result: http -1` → `clip failed`），属预期；
- 另观察到：连续多次 MQTT 重连后出现
  `MQTT:Error create mqtt message task, ret -1` / `client start failed`，
  疑似堆/任务资源在反复销毁重建客户端时耗尽，需改为退避重试并复用资源。

## 8.4 MQTT 反复重建导致堆耗尽（2026-11-15 12:25）

- 现象：网络中断后，`app_mqtt` 每 ~11 秒 stop/init/start 一次；约 8 次后
  日志出现 `Memory exhausted` → `client init failed`，之后再也连不上。
- 原因：SDK 的 `trs_mqtt_client_stop()` 只置 run=false，客户端任务退出时才
  destroy/free（TLS 上下文、outbox、队列）；我们只等 200ms 就重建，
  新旧客户端的资源重叠，堆逐步耗尽。
- 修复：
  - stop 后等待 **3 秒**，确保旧任务完成销毁；
  - 连续失败指数退避（5/10/15/20/25/30 秒），成功连接后清零；
  - 初始化/启动失败时打印剩余堆（`os_get_free_heap_size()`），便于诊断。
- 相关：Wi-Fi 静默掉线检测（150 秒无 MQTT → 强制重新扫描关联）见
  `docs/10-wifi-boot-sequencing.md`。

## 8.5 服务器 v0.1.1 的"10 秒无数据即取消"与设备快路径（2026-11-15）

- 服务器新规则（仅 platform-test-02）：任务创建后 **10 秒内没有实际音频数据**
  自动取消并释放占用；上传过程中连续 10 秒没有新数据也会清理。
- 设备侧原来在 202 之后还要：等 MQTT 命令（最多 10 秒）→ 预检查轮询 →
  从 SD 读 480 KB 快照 + SHA（约 10–20 秒）→ 才 POST output。这样很容易
  在 10 秒窗口内"什么都没发"，被服务器清理后 GET 返回 410。
- 已实现的快路径：
  - 202 body 或 MQTT 命令**一拿到 job_id 就跳过**命令等待与 GET 预检查，
    立即 prepare_clip → POST output；
  - `GET 410/404` 视为终态，立即放弃该片段，不再空轮询；
  - `409 BUSY` 用同一 Idempotency-Key 每 10 秒退避重试，最多 5 次。
- **仍未解决的时间风险**：480 KB 快照读盘 + SHA 约 10–20 秒，可能超过
  10 秒首包窗口。建议服务器把"任务创建 → 首个音频数据"的宽限提高到
  30–60 秒，或把 `POST output`（元数据）也算作"有数据"。
- 设备侧可进一步优化：保存时只快照 `APP_RETENTION_MINUTES` 内**实际可用**
  的音频（当前已是），并把 SHA 与上传合并（仍需两遍，见 8.3）。

## 8.6 上传准备期间 Wi-Fi 掉线：CPU 饿死（2026-11-15 排查）

- 现象：热点稳定 10+ 分钟后，设备一开始准备上传（SD 快照 + SHA + 连续
  TLS 握手）就掉线；日志时间线：410 轮询/快照期间 MQTT 的 TLS 写失败
  （errno=113），此后全部连接超时，热点列表无客户端。
- 判断：ECW 单核，上传任务（优先级 5）与采集任务（5）连续跑 CPU 密集代码
  （位翻转 SD 读写、SHA256、mbedTLS 加解密）时，Wi-Fi 驱动/WPA 任务拿不到
  足够时间片，错过信标后被 AP 踢掉——看起来像"信号不好"，实际是调度饿死。
- 已加的让出点：
  - `prepare_clip` 每个 512 样本块 `os_msleep(1)`；
  - PUT 流式上传每个 2KB 块 `os_msleep(1)`；
  - 快路径避免高频 GET 轮询（原来每 ~3 秒一次完整 TLS 握手）。
- 进一步可选：复用同一条 TLS 连接完成 output/blob/result（减少 3 次握手）；
  把上传任务优先级降到 Wi-Fi 任务之下并用信号量协调。
- 说明：这不是"信号不好"，环境侧（锁屏关热点）与代码侧（CPU 饿死）都会
  造成类似日志；区分方法是看掉线是否总发生在重负载开始后的数十秒内。

## 8.7 对照实验：上传期间暂停写 SD（2026-11-15）

- 时间线（真实日志）：202 接受后 26 秒内 Wi-Fi/MQTT 都正常，快照持续 SD 读取
  时在 04:58:19 出现 MQTT TLS 写失败（errno=113），随后 AP 侧失联；
  **芯片未重启**（ring 数据连续增长），只是 RF/网络路径失效。
- 已排除：CPU 调度饿死（已在 SHA/SD/TLS 循环加 1ms 让出点，仍复现）；
  服务器规则（202 正常，且失联发生在 POST output 之前）。
- 当前假设：**SD 卡重度读写的电流尖峰让 RF 掉电/失锁，或位翻转 SPI 的
  1MHz 谐波干扰 2.4GHz 接收**（硬件耦合），而不是"信号不好"。
- 对照实验（本版固件）：`upload_formal_run()` 在开始前调用
  `audio_capture_pause(1)`，结束后清除。采集任务仍继续读/解码（避免 I2S
  DMA 溢出），但**不写 SD**，于是上传期间只剩"SD 读 + 网络"。
  - 如果掉线消失 → 坐实"SD 写 + Wi-Fi 同时满载"的硬件耦合；
  - 如果仍掉线 → 问题在 SD 读取本身或网络栈，继续查。
- 代价：上传期间录音出现空洞（约 30–60 秒），下次触摸的"最近 N 秒"会包含
  这段静默；若作为正式方案，需产品确认（或改为上传前锁定快照、上传后再
  恢复录音写入）。
- 硬件侧建议同时验证：换更强的 5V 电源、换一张 SD 卡（写电流差异很大）。

## 8.8 对照实验结果与最终对策（2026-11-15 13:09）

- 实验：上传期间暂停采集写 SD（`audio_capture_pause`），只剩上传的 SD 读。
- 结果：**仍然在上传开始约 32 秒后掉线**（05:09:28 开始，05:10:00 MQTT
  transport error），说明不是"读写并发"，而是**连续不断的位翻转 SD 活动**
  本身（每次读 1KB、间隔仅 1ms，持续几十秒）会让 RF 失锁；空闲采集是每
  250ms 写 4KB 的短脉冲，所以从不掉线。
- 对策（已实现）：
  - SD 快照读和上传流读改为**每块后 5ms 间隙**，给 Wi-Fi 收包/信标处理
    喘息时间；代价是快照约 10–15 秒（服务器 10 秒首包窗口仍需放宽）；
  - 硬件侧同时建议：更强 5V 电源、换 SD 卡、SD 线远离天线。
- 顺带修复的另一个真实 bug：命令主题存在 **retained 旧命令**。启动时先收到
  上一次启动的 `audio.export`（boot_id 不同），上传会拿这个旧 job 去处理，
  服务器已清理 → 410 GONE。现在 `handle_command_payload` 解析 `boot_id`，
  与本次启动不一致的命令直接丢弃（日志 `stale command ignored`）。

## 8.9 根因：SD 位翻转 SPI 只有 ~10 KB/s（2026-11-15 13:17 实测）

- 日志时间线：05:16:55 点击开始上传，`total` 冻结在 504KB（暂停写盘生效），
  480KB 快照到 **05:17:52 才完成（57 秒）**；05:17:19 网络掉线（开始后 24s）。
- 反推吞吐：480KB / 57s ≈ **8.4 KB/s**；采集侧 `total` 每 10 秒只涨约 90KB，
  即 **9 KB/s**（8kHz×16bit 应为 16KB/s），说明采集也在丢样本。
- 原因：`app_sdspi.c` 的 `sd_delay()` 固定 8 次空循环，每个 bit 两次，
  单字节约 16 次循环 + 多次 GPIO 调用。5ms 间隙在这种 ~95% 占空比下没有意义。
- 修复：初始化阶段保持 8 次（兼容性），卡就绪后切到 `APP_SD_FAST_LOOPS=1`。
  预期吞吐提升 3–6 倍：快照降到 5–15 秒、采集不再丢样本、连续位翻转时间
  大幅缩短，Wi-Fi 被"踢死"的窗口随之消失。
- 风险：信号完整性/误码。验证方法：`total` 每 10 秒应接近 160KB；资产/LOGO
  的 CRC 校验会在启动时发现坏块；若出现 `[sd] CMD17 r1=...`/token 超时，
  把 `APP_SD_FAST_LOOPS` 调到 2–4。
- 这一步之后，服务器 10 秒首包窗口仍建议放宽到 30 秒（快照+哈希 5–15 秒）。

## 8.10 真正的机制：位翻转每写一次 GPIO 都关中断（2026-11-15 13:24）

- 读 SDK 源码发现：`drv_gpio_write()` / `drv_gpio_read()` 内部都会
  `system_irq_save()/restore()`。位翻转时一个字节要写 2 次 MOSI/SCK、
  读 1 次 MISO，即 **每字节 16–24 次关中断**；连续读 480KB ≈ 上千万次。
  这会让 Wi-Fi 的 ISR/信标处理被反复延迟，最终失锁——与"空闲写 SD
  （4KB/250ms，占空比 <1%）从不掉线、上传连续读必掉"完全吻合。
- 修复：`app_sdspi.c` 的 `bb_byte()` 改为**寄存器直写**
  （`GPIO_REG_BASE->DataOut/DataIn`），不再关中断；延时循环数在初始化后
  从 8 降到 `APP_SD_FAST_LOOPS=0`。
- 风险：DataOut 的读改写没有中断保护；本应用中其他 GPIO 写入都在初始化期，
  运行期只有 SD 在改这几位，可接受。若出现误码（`[sd] CMD17`/token 超时），
  先把 `APP_SD_FAST_LOOPS` 调到 2–4。
- 预期：SD 吞吐从 ~11.6KB/s 提到数十 KB/s；采集 `total` 每 10 秒回到 ~160KB；
  480KB 快照在 5–10 秒内完成，上传期间 Wi-Fi 不再被拖死。

## 8.11 上传快照限制为 10 秒（2026-11-15 13:36，临时方案）

- 实测：快照 1.9MB 时 SD 连续读 57 秒，期间 SNTP/MQTT 全部不通；SD 读一停
  （05:36:25）SNTP 立刻恢复 → 确认是持续性 SD 读把 RF 拖死，不是热点永久
  踢线（网络会自愈）。
- 软件侧最后的缓解：`APP_UPLOAD_MAX_SECONDS=10`，只上传最近 10 秒（160KB，
  SD 读约 5 秒），使 RF 中断窗口短于其容忍时间，同时尽量贴近服务器
  10 秒首包窗口。
- 代价：上传内容从"最近 N 分钟"变为"最近 10 秒"，产品行为需确认；
  录音环形区本身仍保留 30 秒回放。
- 根治仍需硬件：换 SD 卡/缩短 SD 走线/远离天线/加强供电；硬件改善后可把
  `APP_UPLOAD_MAX_SECONDS` 调回更大的值。

## 9. 验证清单

- [ ] `app_config.h` / `app_ca_cert.c` 已替换所有 `YOUR_`/占位符
- [ ] ECW 日志：`sta associated`、`got ip`
- [ ] ECW 日志：`[https] connected ... TLSv1.2 ...`
- [ ] ECW 日志：`[mqtt] connected, subscribing ...`
- [ ] 触摸后：`[mqtt] command job=... type=audio.export`
- [ ] `[up] clip: N min, ... wav ... sha=...`
- [ ] `[up] POST output: http 2xx`
- [ ] `[up] PUT blob: http 2xx`，服务端 `verified` 且 SHA 一致
- [ ] `[up] POST result: http 2xx`
- [ ] 服务器历史里出现记录，能下载并播放 16 kHz WAV
- [ ] 断网/重启/重复触摸时，`X-Clip-Id` 之外的 `Idempotency-Key` 行为符合文档
