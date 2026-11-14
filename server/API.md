# App 与设备 API · desk-recorder/v1

服务器 v0.1.1，兼容固件 v0.3.0 的现有任务/上传通道。新设备主动申请入口需设备端后续调用，本次没有修改固件。所有公网请求走 HTTPS；个人部署使用安装程序生成的 CA，App 客户端需信任这张证书，不能通过关闭主机名/证书验证绕过。浏览器管理页与 API 同源，默认不开放跨域访问。

## 用户登录

`POST /api/v1/auth/login`

```json
{"username":"owner","password":"安装时生成的管理密码"}
```

返回 `access_token`、`token_type:"bearer"`、`expires_in:3600`。之后 App 请求使用 `Authorization: Bearer <access_token>`。不应将设备令牌当用户令牌。`DELETE /api/v1/auth/session` 注销当前令牌。

通用失败结构：

```json
{"error":{"code":"BUSY","message":"Device already has an active task"}}
```

## 设备和任务

| 方法和路径 | 内容 |
|---|---|
| `GET /api/v1/devices` | `{"items":[...]}`，只含当前用户的设备 |
| `GET /api/v1/devices/{device_id}` | 单台设备在线情况、最近心跳和原始 telemetry |
| `POST /api/v1/devices/{device_id}/jobs` | `device.ping` 或 `audio.configure` |
| `POST /api/v1/devices/{device_id}/recordings` | 保存当前最近录音，body 为 `{}` |
| `GET /api/v1/jobs/{job_id}` | 原始命令、服务器状态及结果 |
| `DELETE /api/v1/jobs/{job_id}` | 取消未完成任务；不撤销已执行的设备设置 |

两个创建接口均要求 `Idempotency-Key`：8～128 个 ASCII 字母、数字、短横线或下划线。一次用户操作生成一次；同一操作的网络重试必须复用该值。相同键不同参数返回 409。

修改时长：

```json
{"type":"audio.configure","params":{"minutes":10}}
```

测试设备：

```json
{"type":"device.ping","params":{}}
```

成功创建 HTTP 202，返回 `job_id` 及 `state`；录音任务还包含 `recording_id`。不要将 202、MQTT 已发送或 telemetry 的 `bytes_sent` 当作保存成功。轮询任务直到 `succeeded`、`failed`、`expired`；取消/删除后的查询可能返回 410。

新保存被拒绝的主要情况：服务器 MQTT 未连接（503）、设备离线/忙碌（409）、越权或不存在（404）。已归档历史的查询和下载不依赖设备在线。

任务开始期限为 55 秒，完成期限为 23 小时。设备首次鉴权预检查 GET 会标记任务已被访问，但仍返回 queued 兼容现有固件；此后服务器保留任务至完成期限，避免预检查后断网导致封存文件无法重试。这不证明设备已开始采集。客户端可以取消长时间无进展的任务。

## 历史、下载、删除和容量

`GET /api/v1/recordings?limit=50&offset=0`，limit 1～100。返回 `items/limit/offset`。记录包含：

- `id/device_id/job_id/created_ms/state/size_bytes/sha256`。
- `source`：`app` 为用户登录后创建，`device` 为设备令牌主动申请。旧记录迁移为 `app`。
- `metadata`：录音的实际起止时间、时长、格式、设备启动编号等。
- `state=ready` 可以下载；`deleting` 表示已禁止访问，文件清理尚未完成。

`GET /api/v1/recordings/{recording_id}/audio` 返回 WAV，支持 HTTP Range。全文件大小和 SHA256 可以与记录核对。日期显示使用 `metadata.end_at_ms`，为空时应显示时间未知/以服务器归档时间辅助，不用上传完成时间伪装实际采集时间。

`DELETE /api/v1/recordings/{recording_id}` 同时取消该记录对应的在途保存任务并建立删除标记，防止迟到的 PUT 恢复数据。返回：

```json
{"recording_id":"rec-...","state":"deleted","quota_released":true}
```

若 `state:"deleting", quota_released:false`，界面应显示正在清理，不应显示已释放容量。后台会重试清理，也可以重复 DELETE。取消尚未完成的录音也可使用该接口。

`GET /api/v1/storage` 返回 `quota_bytes/used_bytes/reserved_bytes/available_bytes`。逻辑配额包含已归档与已预留数据；磁盘空间还需为系统、临时重传、数据库和备份预留，二者不是同一个数值。

## AI 预留

- `POST /api/v1/recordings/{recording_id}/analysis-jobs`
- `GET /api/v1/analysis-jobs/{analysis_id}`

均需用户登录；创建前还要校验记录归属。目前返回 HTTP 501 和 `AI_NOT_CONFIGURED`，不会创建假任务，也不会自动调用任何模型。后续由 App 决定是否显式发起分析，分析失败不能删除原录音。

## ESP32 通道

### 设备主动申请保存（v0.1.1 新增）

```http
POST /device/v1/recordings
Authorization: Bearer <device_token>
X-Device-Id: <device_id>
Idempotency-Key: <本次操作唯一编号>
Content-Type: application/json

{}
```

不使用 App 登录令牌。设备归属用户由服务器数据库决定，不接受 owner_id、device_id、minutes、文件路径或其他请求体字段。保存范围使用设备当前保留时长。

成功返回 HTTP 202，响应结构与 App 保存接口一致。例如（编号和时间仅示意）：

```json
{
  "protocol": "desk-recorder/v1",
  "job_id": "job-0123456789abcdef0123456789abcdef",
  "recording_id": "rec-0123456789abcdef0123456789abcdef",
  "device_id": "xiao-test",
  "boot_id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "connection_id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  "type": "audio.export",
  "params": {},
  "execute_before_ms": 1800000055000,
  "complete_before_ms": 1800082800000,
  "created_ms": 1800000000000,
  "updated_ms": 1800000000000,
  "state": "queued"
}
```

服务器提交数据库任务后，由现有 MQTT 调度器发送 `audio.export`。调用方只负责申请；设备执行端沿用现有 MQTT 接收、任务预检查、快照、上传和结果确认流程，不要因 HTTP 202 再额外启动第二次导出。MQTT 命令字段和协议版本没有改变。

- 每次操作生成一个 8～128 字符的 ASCII 字母/数字/短横线/下划线编号；同次操作的超时、断线、重启后重试均复用它。可使用 UUID 去掉短横线后的值。设备与 App 的编号命名空间隔离。
- 同编号返回原 job_id/recording_id 及当前状态，不重新导出或增加历史。即使此时 MQTT 断开，也可查询已创建的请求。HTTP 202 可能返回已经完成或失败的原任务，必须检查 state。
- 网络超时无法判断是否创建成功，应使用原编号重试。旧任务失败/过期也不会因相同编号重新创建；用户明确重新操作时才生成新编号。
- 新任务要求服务器 MQTT 已连接、设备心跳在线、声明支持 audio.export 且没有活动任务。收到申请不等于证明设备 MQTT 在线。
- 每台设备每 60 秒最多创建 6 个设备来源任务；同编号重试不计数，计数存于数据库，重启不清零。App 与设备共享单活动任务限制。
- 保存的是命令执行时的最近 N 分钟。此接口不提供按键瞬间固定快照、离线触发排队或任意已有文件上传能力。
- 用 `GET /device/v1/jobs/{job_id}` 查询进度。202 不是归档成功；上传校验和结果确认完成后 state 才为 succeeded。App 通过原有历史接口查看和下载，AI 不会自动启动。

| HTTP / 错误码 | 含义与处理 |
|---|---|
| 400 BAD_REQUEST / BAD_IDEMPOTENCY_KEY | 请求体或编号不合法，修正请求 |
| 401 UNAUTHENTICATED | 设备编号、设备令牌不匹配；App 令牌不能用于这里 |
| 409 DEVICE_OFFLINE / BUSY / UNSUPPORTED | 等待有效心跳、当前任务完成，或核对设备能力 |
| 429 RATE_LIMITED | 新任务过于频繁，等待后重试；同编号已存在任务仍可返回 |
| 503 COMMAND_CHANNEL_OFFLINE | 新任务暂不可创建，恢复 MQTT 后以原编号重试 |
| 410 GONE | 原请求对应任务/录音已取消或删除，不会恢复它 |

容量沿用现有机制：申请不承诺已预留存储，提交 WAV 元数据时才按实际文件大小预留，超额返回 507。设备应按现有结果接口报告失败，不能把文件上传失败显示为保存成功。历史删除和文件校验规则均不变。

### 原有任务与上传接口

MQTT 主题前缀 `desk/v1/devices/{device_id}/`，`commands/events` QoS1，`telemetry` QoS0，`presence` QoS1且保留。仅当前会话的非保留、有效心跳可以证明在线；90 秒超时。新 broker 连接之后需要重新收到有效心跳才能投递命令。

设备 HTTP 使用 `Authorization: Bearer <device_token>` 与 `X-Device-Id`：

1. `GET /device/v1/jobs/{job_id}`：不可变原始命令＋state；成功录音另含 output 校验回执。
2. `POST /device/v1/jobs/{job_id}/output`：提交固定 WAV 元数据，预留容量；重复提交必须完全一致。
3. `PUT /device/v1/jobs/{job_id}/output/blob`：原始完整 WAV，Content-Length 与元数据一致。逐块写入、核对标准44字节WAV头和 SHA256 后原子归档，返回 verified/recording_id/size_bytes/sha256。
4. `POST /device/v1/jobs/{job_id}/result`：提交终态结果。成功保存要求文件已完整归档；重复结果幂等。

最大 WAV 为 115,200,044 字节，格式固定为16kHz、单声道、16-bit PCM。重传相同 job_id/recording_id 不产生第二条历史，不重复扣配额。若归档后最后结果回执丢失，文件继续保留；任务到期会依据已验证档案恢复成功状态。

设备认证不能读取其他设备任务，用户认证不能读取他人的历史。删除/取消任务返回410；普通不存在返回404；配额不足在元数据预留阶段返回507。服务器容量不足不应远程停止设备录音。

当前没有公开注册、自动绑定、停止采集、OTA、任意文件转发或 STM32/HID 接口。
