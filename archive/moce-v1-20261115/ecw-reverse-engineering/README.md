# ECW6700F 逆向开发交接归档

- 归档日期：2026-11-14
- 目标：绕过 MOCE Workbench 平台，直接用厂商 SDK + 工具链编译、烧录、调试 ECW6700F 固件。
- 当前状态：**编译、烧录、启动链路已打通并实机验证通过**；应用层初始化 `mic=1 tts=1 i2c=1`，已进入 `e_stream`。
- 唯一未闭合项：I2C 对端 STM32（地址 `0x08`）未接入，写入返回 `reason=3`（无 ACK/超时）。

---

## 0. 30 秒快速开始

```bash
cd C:\Users\LiuLinjun\AppData\Local\Programs

# 1) 重新编译（约 3 分钟，自动解包 SDK、编译 board_support、导出产物）
bash ecw-build-output/build_firmware.sh

# 2) 烧录（先确认 CH340 为 COM7，设备管理器状态 OK）
cd MOCEWorkbench-data/moce-cache-ca0b65ff70a4/flash-jobs
C:/Users/LiuLinjun/AppData/Local/Programs/MOCEWorkbench-data/tools/ecw6700-windows/flash/eswin_tool.exe flash --device COM7 --allinoneFile C:/Users/LiuLinjun/AppData/Local/Programs/ecw-build-output/firmware_allinone.bin

# 3) 看启动日志（115200 8N1，COM7；在 Git Bash 里执行）
powershell -NoProfile -Command '$p=New-Object System.IO.Ports.SerialPort "COM7",115200,None,8,One; $p.Open(); Start-Sleep 3; $p.ReadExisting(); $p.Close()'
```

串口线序/供电、失败排查见第 5、7 节。

---

## 1. 关键路径

| 内容 | 路径 |
|---|---|
| 工程源码 | `C:\Users\LiuLinjun\AppData\Local\Programs\firmware-code-ff5c7de034b6-ecw6700` |
| 可复现构建脚本 | `C:\Users\LiuLinjun\AppData\Local\Programs\ecw-build-output\build_firmware.sh`（归档副本 `tools/build_firmware.sh`） |
| 编译产物 | `C:\Users\LiuLinjun\AppData\Local\Programs\ecw-build-output\firmware_allinone.bin` 等 |
| SDK 解包工作树 | `C:\Users\LiuLinjun\AppData\Local\Programs\ecw-build-work\sdk\ECW6700FSDK_V1.0.0B02P12\code` |
| SDK 原始包 | `MOCEWorkbench-data\tools\ecw6700-windows\bsp\eswin\sdk\b02p12\ECW6700FSDK_V1.0.0B02P12.tar.gz`（sha256 `084618cdb6ab6f9c014e683fafff4ae82c3b77d3f424018a8d0e5604aa1c229c`） |
| SDK 解包+overlay 脚本 | `MOCEWorkbench-data\tools\ecw6700-windows\bsp\eswin\sdk\b02p12\prepare_sdk.py` |
| 编译器 | `MOCEWorkbench-data\tools\ecw6700-windows\compiler\bin\riscv64-unknown-elf-gcc.exe`（ESWIN GCC 14.1.1） |
| make | `MOCEWorkbench-data\tools\ecw6700-windows\make\bin\make.exe` |
| shell | `MOCEWorkbench-data\tools\ecw6700-windows\git\usr\bin\sh.exe` |
| 烧录器（Workbench 版） | `MOCEWorkbench-data\tools\ecw6700-windows\flash\eswin_tool.exe`（v2.1.13） |
| 烧录器（SDK 版） | `ecw-build-work\sdk\ECW6700FSDK_V1.0.0B02P12\code\scripts\bin\eswin_tool\eswin_tool.exe`（v2.2.0，烧录逻辑与 2.1.13 相同） |
| Workbench 工程副本 | `MOCEWorkbench-data\projects\a37b8e3e450f4c14a0c6d44a05938ec5`（name=`firmware-code-ff5c7de034b6-ecw6700`） |
| Workbench 烧录后端 | `MOCEWorkbench\backend\flashing.py`（ECW 分支在 91-95、148-149 行） |
| Workbench 构建后端 | `MOCEWorkbench\backend\recipes.py` 的 `build_ecw()` |
| 本次成功烧录日志 | `ecw-reverse-engineering\logs\flash_success_20261114.log` |
| 本次启动日志 | `ecw-reverse-engineering\logs\boot_after_flash_20261114.log` |
| 逆向脚本 | `ecw-reverse-engineering\tools\` |

产物校验（2026-11-14）：

```
firmware_allinone.bin  1,558,391 bytes
sha256 = cb636d097002ea3d7a74acbcb3bfec981835cb8d19da39111b13f43002916636
firmware_cpu.bin       1,486,656 bytes
firmware.elf / firmware.map / firmware.flash 同目录
```

> 注意：`MOCEWorkbench-data\moce-cache-ca0b65ff70a4\builds\abbe9ad589f8` 是另一个旧工程（`firmware-code-4afd3142b799`）的构建缓存，不是当前工程。当前工程的 Workbench 副本 id 是 `a37b8e3e450f4c14a0c6d44a05938ec5`。

---

## 2. 工程结构

```
firmware-code-ff5c7de034b6-ecw6700/
├─ CMakeLists.txt                 # 只做 ecw6700f_application(SOURCE main.c)
├─ main/
│  ├─ main.c                      # 应用主体：PDM 麦克风 -> I2C/STM32；SYN6288E TTS；状态机
│  └─ board_support/
│     ├─ board.h / board.c        # MOCE board-support/v1：引脚表 + __wrap_chip_i2c_pinmux_init
│     ├─ pins.inc / ports.inc / signals.inc / i2c.inc
│     ├─ attachment.json          # 板级绑定元数据，不参与编译
│     └─ build.py                 # Linux 构建适配器（Windows Workbench 不执行）
└─ README.md / SUBSYSTEM.md
```

应用入口：`main.c` 里的 `moce_app_main()`。SDK 启动文件 `Boards/ecw6700f/standalone/main.c` 在 marker 后调用它。

主要行为：

- `init_mic_i2s()`：`drv_i2s_master_init` + `drv_i2s_master_read_enable(SLOT_MODE_MONO, 1024)`
- `init_tts_uart()`：UART0，9600 8N1，`drv_uart_open`
- `init_i2c()`：I2C0，地址 `0x08`，100 kHz；实际 SCL/SDA 由 `board.c` 的 wrap 重映射到 IO24/IO23
- `app_task`：`ST_INIT -> ST_STREAM`，每 10 ms 采集一次 PDM，解码 PCM16，经 I2C 发 `PROTO_ID_AUDIO_CHUNK(0x01)`
- TTS：`0xFD | len_hi | len_lo | 0x01 | 0x01 | GBK | XOR`，帧间 >=9 ms
- 无线部分目前只是 `wireless_mark_connected()` / `wireless_service()` 桩函数，**没有调用任何 BLE API**

---

## 3. 构建

### 3.1 为什么 Workbench 直接编译会失败

`MOCEWorkbench\backend\recipes.py` 的 `build_ecw()` 只做：

1. 通过 `prepare_sdk.py` 解包 SDK；
2. 跑一次 CMake，生成 `moce-application.txt`；
3. 把 `project/main/main.c` 复制成 `Boards/ecw6700f/standalone/moce_application.c`；
4. 在 `Boards/ecw6700f/standalone/Make.defs` 追加 `CSRCS += moce_application.c`；
5. 直接 `make`。

它**不会**：

- 把 `board_support/` 加入 `INCPATHS`；
- 编译 `board.c`；
- 给链接器加 `--wrap=chip_i2c_pinmux_init`。

因此会出现：

```
ecw6700f/standalone/moce_application.c:23:10: fatal error: board_support/board.h: No such file or directory
```

即使修掉头文件，后面也会因 `moce_board_pin` / `__wrap_chip_i2c_pinmux_init` 未定义而链接失败。

### 3.2 可复现构建（已验证）

脚本：`ecw-build-output\build_firmware.sh`（归档副本 `tools/build_firmware.sh`）。它做的事情：

1. `prepare_sdk.py --output ecw-build-work/sdk`（解包 + overlay）；
2. 打 Windows 主机补丁：`components/wpa/Makefile`、`components/sntp/Makefile` 的 VPATH 替换；`Boards/ecw6700f/makefile` 的 ldscript 改用 `win_rsp`；
3. 在 `Boards/ecw6700f/standalone/main.c` 的 marker 后插入 `extern void moce_app_main(void); moce_app_main();`；
4. 复制工程 `main.c` -> `moce_application.c`；复制 `board_support/`；复制 `board.c` -> `moce_board_support.c`；
5. `Make.defs` 追加：

```make
CSRCS += moce_application.c
CSRCS += moce_board_support.c
INCPATHS += $(BOARD_DIR)/board_support
```

6. 关键环境：

```bash
PATH=<tools>/compiler/bin:<tools>/make/bin:<tools>/git/usr/bin:<workbench>/runtime/tools/bin:$PATH
RISCV_GCC_PATH=<tools>/compiler/bin/
MSYS_NO_PATHCONV=1
SHELL=<tools>/git/usr/bin/sh.exe
OS_TYPE_CFG=Windows
LD=<tools>/compiler/bin/riscv64-unknown-elf-ld.exe --wrap=chip_i2c_pinmux_init
```

7. `make -j2 all PARALLEL=-j2 V=1 SDK_V=V1.0.0B02P12 REL_V=1.0.0`
8. 导出 `standalone.elf`、`standalone.map`、`standalone.flash`、`ECW6700F_standalone_cpu_0x14000.bin`、`ECW6700F_standalone_*_allinone.bin`。

已验证关键符号都进了 ELF：

```
T __wrap_chip_i2c_pinmux_init
T moce_board_instance
T moce_board_pin
T moce_app_main
t app_task
```

### 3.3 SDK overlay（改底层时必看）

`prepare_sdk.py` 会覆盖以下文件（见 `SDK_SOURCE.json`）：

- `drivers/i2s/i2s_ll.c`、`drivers/i2s/i2s_ll.h`（I2S 底层）
- `drivers/dma/dma.c`、`include/drivers/dma/dma.h`
- `drivers/adc/aux_adc.c`、`aux_adc_common.c`、新增 `include/drivers/adc/moce_adc_checked.h`
- `components/wpa/*`、`components/sntp/Makefile`、`components/wifi_ctrl/system_wifi.c`
- `components/ble/nimble/.../gap|gatt`
- `Boards/ecw6700f/makefile`、`Boards/makefile`

如果你改的是 I2S/DMA/ADC，记得改 overlay 或直接改解包后的构建副本，否则下次重新准备 SDK 会被覆盖。

---

## 4. 非标准头文件与 API 地图

### 4.1 直接包含

| include | 实际文件 | 用途 |
|---|---|---|
| `drivers/uart/uart.h` | `SDK/include/drivers/uart/uart.h` | `drv_uart_open/write/read/close`、`T_DRV_UART_CONFIG`、`BAUD_RATE_*`、`E_UART_NUM_0` |
| `drivers/i2c/i2c_master.h` | `SDK/include/drivers/i2c/i2c_master.h` | `drv_i2c_master_open/read/write/write_read/close`、`T_DRV_I2C_MASTER_CONFIG`、`I2C_ADDR_M_LEN_7`、`I2C_Speed_100K` |
| `drivers/i2s/i2s.h` | `SDK/include/drivers/i2s/i2s.h` | `drv_i2s_master_init/read_enable/read_timeout/read_disable`、`T_DRV_I2S_CONFIG`、`SLOT_*` |
| `os/oshal.h` | `SDK/include/os/oshal.h` | `os_task_create`、`os_msleep`、`os_time_get`、`os_printf`、`LL_INFO`、`OS_TASK_CREATE_FAILED` |
| `chip_pinmux.h` | `SDK/Boards/ecw6700f/common/include/chip_pinmux.h` | `set_pin_function`、`FUNC_I2C0_SCL/SDA`、`chip_i2c_pinmux_init` |
| `chip_smuaon.h` | `SDK/Boards/ecw6700f/common/include/chip_smuaon.h` | AON 寄存器；配合 `MEM_BASE_SMU_AON` 设置 pad 上拉 |
| `board_support/board.h` | 工程本地 | `moce_board_pin()`、`moce_board_instance()`、`MOCE_SIGNAL_*` |

`uart.h` / `i2c_master.h` / `i2s.h` 都是自包含的，不 include 其他头文件。

### 4.2 传递包含

- `oshal.h` -> `os_task_config.h`、`components/platform_api/{reg_macro_def,common_macro_def,format_conversion}.h`、`arch/arch_cpu.h`、`chip_memmap.h`、`arch/arch_irq.h`、`components/cmd/auto_generate_log_common.h`
- `arch_cpu.h` -> `chip_irqvector.h`、`emsis_version.h`、`riscv_encoding.h`、`emsis_compiler.h`、`features/core_feature_*.h`
- `riscv_encoding.h` -> `csr/riscv_encoding_{m,s,u}.h` -> `csr/riscv_bits.h`
- `chip_pinmux.h` -> `oshal.h`、`chip_smupd.h`、`chip_smuaon.h`
- `chip_smupd.h` / `chip_smuaon.h` -> `chip_memmap.h`
- 编译时强制 `-include Boards/ecw6700f/standalone/generated/config.h`（由 Kconfig/defconfig 生成）

### 4.3 实现源码

| 模块 | 文件 |
|---|---|
| UART | `SDK/drivers/uart/uart.c`、`uart_ll.c`、`uart_psm.c` |
| I2C | `SDK/drivers/i2c/i2c_master.c`、`i2c_common.c` |
| I2S/PDM | `SDK/drivers/i2s/i2s_master.c`、`i2s_core.c`、`i2s_ll.c`（被 overlay 覆盖） |
| OS 抽象 | `SDK/os/os_hal/os_hal_freertos.c` |
| 引脚复用 | `SDK/Boards/ecw6700f/common/chip_pin_mux.c` |

### 4.4 包含路径

```
-include <SDK>/Boards/ecw6700f/standalone/generated/config.h
-I <SDK>/Boards/ecw6700f/common/include/arch
-I <SDK>/Boards/ecw6700f/common/include
-I <SDK>/Boards/ecw6700f/standalone/generated
-I <SDK>/include                       # 以及 include 下所有子目录
-I <SDK>/Boards/ecw6700f/standalone
-I <SDK>/os/freertos/FreeRTOS-Kernel/portable/GCC/RISCV_E315/non_secure
-I <项目>/main
-I <项目>/main/board_support
```

---

## 5. 烧录链路（已逆向）

### 5.1 Workbench 没有额外逻辑

`backend/flashing.py` 的 ECW 分支只做：

```python
tool = find_one(root, ['flash/eswin_tool.exe', 'flash/eswin_tool'])
output = run([tool, 'flash', '--device', device,
              '--allinoneFile', image], cwd=operation, env=env, timeout=300)
```

真正的下载通道在 `eswin_tool` 内部。Workbench 版 2.1.13 和 SDK 版 2.2.0 的 `__init__` / `reset` / 同步逻辑相同。

### 5.2 eswin_tool 内部协议（反汇编确认）

| 阶段 | 行为 |
|---|---|
| 初始化 | `serial.Serial()`；`baudrate = boot_baudrate`；`port = COMx`；`setDTR(0)`；`setRTS(0)`；`timeout = 0.5`；`open()` |
| 复位/进下载 | `setRTS(1)` -> 读空（timeout 1 ms）-> `setRTS(0)` |
| 同步 | 循环写 ASCII `cnys`（`63 6e 79 73`），读 1 字节；非空则再读 32 字节并扫描 |
| 有效响应 | 字节 `2`/`3` -> ECW6630；字节 `4` -> ECW6700 |
| 同步上限 | 3000 次（`BOOT_SYNC_MAX_SENDS`） |
| Boot 波特率 | 默认 **115200** |
| 下载波特率 | 默认 **2000000** |
| 切波特率命令 | `struct.pack('<BBHI', 0xA5, 7, 4, baud)` + checksum，写后读 6 字节 |
| stub | 大小 `0x315c`，下载到 RAM `0x80048000` |
| 后续 | `A5 sync ok` -> 切 2M -> 逐分区写 + hash 校验 -> `chip start_up` |

`A5 07 04` 中的 `0x07` 是命令码，`0x04` 是长度，后面 4 字节是小端波特率；checksum 由 `get_check_value()` 生成。

### 5.3 allinone 布局（本次镜像实测）

| 地址 | 大小 | 内容 |
|---|---|---|
| `0x00000000` | `0x2000` | 分区表 |
| `0x00002000` | `0x76a0` | sboot |
| `0x00014000` | `0x16af40` | 应用（CPU 镜像） |
| `0x003EC000` | `0x2000` | OTA status |
| `0x003FB000` | `0x3000` | NV |
| RAM `0x80048000` | `0x315c` | dboot（串口下载到 RAM 运行） |

`allinone file num:6` 指的是 dboot + 5 个 Flash 分区。

### 5.4 成功日志（2026-11-14）

```
COM7 sync ECW6700 OK (resp=0x04)
COM7 dboot download_addr 0x80048000
COM7 RAM download ok end
COM7 A5 sync ok
COM7 set baudrate:2000000
COM7 A5 sync ok
COM7 download file @0x00000000  size(B):0x2000      hash check ok
COM7 download file @0x00002000  size(B):0x76a0      hash check ok
COM7 download file @0x00014000  size(B):0x16af40    hash check ok
COM7 download file @0x003ec000  size(B):0x2000      hash check ok
COM7 download file @0x003fb000  size(B):0x3000      hash check ok
COM7 chip start_up
```

完整日志：`logs/flash_success_20261114.log`。

### 5.5 烧录硬件与接线（实测）

- 烧录器：CH340 USB 转串口 + 双掷拨动开关。
  - 开关左侧丝印：`ECW6700 / ESP32`
  - 开关右侧丝印：`STM32`
  - 烧 ECW6700 时开关拨到 **ECW6700/ESP32 一侧**。
- 6 针 ISP 排线颜色（两端顺序一致，从上到下 / 从右到左）：
  `蓝、黄、红、黑、白、绿`
- 主板：`YL6700B` 模组；底部 6 针 `ISP` 座；板上有 `RST` 按键、`5V IN` 黄色接口、LED。
- 实测由烧录器 6 针 ISP 给主板供电，主板电源处 3.3V 正常，LED 亮。
- 串口：`USB-SERIAL CH340 (COM7)`，115200 8N1。
- 工具只脉冲 **RTS**，**DTR 全程为 0**。烧录器必须把 RTS 正确接到 ECW6700 的复位/下载控制，把 DTR 接到 BOOTMODE（或等效电平）。
- 失败特征：`COM7 sync timeout after 3000 fast cnys sends` -> 芯片没有进入 ROM 下载模式。
- 本次第一块主板 RST 网络实测 0V（一直处于复位），导致完全无响应；换新主板后一次通过。遇到 `sync timeout` 且 UART 无启动日志时，先量 RST 网络是否 0V、确认板子没有损坏。

### 5.6 手动烧录命令

```bash
cd C:/Users/LiuLinjun/AppData/Local/Programs/MOCEWorkbench-data/moce-cache-ca0b65ff70a4/flash-jobs
C:/Users/LiuLinjun/AppData/Local/Programs/MOCEWorkbench-data/tools/ecw6700-windows/flash/eswin_tool.exe flash --device COM7 --allinoneFile C:/Users/LiuLinjun/AppData/Local/Programs/ecw-build-output/firmware_allinone.bin
```

可选参数：

```
--boot_baudrate 115200
--download_baudrate 2000000
```

烧录期间不要拔 USB；CH340 与板子共用 USB 供电时，拔插会让 COM7 消失并导致 `PermissionError(13)`。

---

## 6. 运行验证与当前状态

启动日志（`logs/boot_after_flash_20261114.log`）：

```
SDK version ECW6700F_V1.0.0B02P12, Release version 1.0.0
Start by: RST_TYPE_HARDWARE_REBOOT
flash init OK !  FlashID :0x1640c8
[ecw] i2c: board I2C_0 SCL pin=24 SDA pin=23 (controller)
[ecw] init: mic=1 tts=1 i2c=1 interval=30s
Task:ecw_app create success.
[ecw] state: e_stream (listening)
I2C write failed: addr=0x08 reason=3
```

结论：

- 烧录后固件正常启动；
- 麦克风 I2S、TTS UART、I2C 控制器初始化全部成功；
- 板级引脚重映射生效（SCL=IO24 / SDA=IO23）；
- 应用进入 `ST_STREAM` 并持续尝试向 STM32（`0x08`）发送音频块。

I2C `reason=3`：

- `drv_i2c_master_write()` 在底层写失败后统一返回 `-3`；
- 含义是目标地址没有 ACK 或总线超时；
- 当前测试没有接入 STM32 对端，所以这是预期现象；
- 需要检查：STM32 是否上电、I2C 是否接 IO24/IO23、地址是否 `0x08`、SCL/SDA 是否有上拉（空闲应为 3.3V）。

日志刷屏：`stream_audio_tick()` 每 10 ms 重试一次，失败时每次打印两行。后续可加失败退避或限频。

---

## 7. 已知坑与注意事项

1. **Workbench 无法直接编译本工程**：它不处理 `board_support`。用 `build_firmware.sh`，或把 `board.h/board.c` 合并成单文件 `main.c` 才能让 Workbench 直接编译。
2. **不要删除 `board_support/board.c`**：它提供 `moce_board_pin()`、`moce_board_instance()` 和 `__wrap_chip_i2c_pinmux_init`；没有它 I2C 会落到 SDK 默认引脚（IO11/IO12），不是板子的 IO24/IO23。
3. **链接必须带 `--wrap=chip_i2c_pinmux_init`**：否则 `board.c` 的引脚重映射不会被调用。
4. **Workbench 的工程副本**：`MOCEWorkbench-data\projects\a37b8e3e450f4c14a0c6d44a05938ec5` 与工程目录逐字节一致；改代码后要同步或重新导入。
5. **旧构建缓存不属于当前工程**：`builds\abbe9ad589f8` 是 `firmware-code-4afd3142b799`，其 `moce_application.c` 与当前 `main.c` 不同，不要拿它的 `.d` 当依赖清单。
6. **SDK overlay**：重新准备 SDK 会覆盖 `i2s_ll.*`、`dma.*`、`adc` 等；改底层要同步 overlay。
7. **all-in-one 是整片烧录**：包含 dboot/sboot/分区表/NV，不只是应用；会覆盖 bootloader 和 NV 配置。
8. **烧录期间不要断电/拔 USB**：会导致串口句柄失效和 `PermissionError(13)`；用 RTS/EN 复位，不要拔 USB。
9. **首次失败的主板是硬件损坏**：RST 网络 0V，芯片一直复位；换板后 `sync ECW6700 OK (resp=0x04)` 一次通过。
10. **无线功能尚未实现**：`main.c` 只有 `wireless_mark_connected()` / `wireless_service()` 桩，未包含任何 BLE 头文件。要做蓝牙需引入 SDK 的 `include/components/ble/eswin_ble.h` + NimBLE，或 BSP 的 `bsp_ble.h`。

---

## 8. 逆向方法（可复现）

### 8.1 提取 eswin_tool（PyInstaller）

```bash
python -I tools/extract_pyinstaller.py ^
  C:\...\MOCEWorkbench-data\tools\ecw6700-windows\flash\eswin_tool.exe ^
  C:\...\Temp\pyi_tool\out
```

要点：

- PyInstaller cookie 在文件末尾，magic `MEI\014\013\012\013\016`；
- `CArchive` 的 TOC offset 是相对 `pkg_start = file_size - pkg_len` 的；
- 主脚本条目名是 `eswin_tool`，内容是 Python 3.8 marshal code object（首字节 `e3`）；
- 加 16 字节 pyc 头 `55 0d 0d 0a` + 12 个 0，再用 `xdis` 反汇编。

### 8.2 反汇编 Python 3.8 字节码

```bash
python -m pip install --target ./pylibs xdis
python -I -c "
import sys; sys.path.insert(0,'pylibs')
from xdis import load_module
from xdis.disasm import disco
v=load_module('eswin_tool38.pyc')
version,timestamp,magic_int,top=v[0],v[1],v[2],v[3]
def walk(co):
    for c in co.co_consts:
        if hasattr(c,'co_name'):
            if c.co_name in {'__init__','reset','_bootrom_sync','_run_boot_sync_burst','up_chip_baudrate'}:
                print('###', c.co_name, c.co_firstlineno)
                disco(version,c,timestamp,magic_int=magic_int)
            walk(c)
walk(top)
"
```

关键函数位置（2.1.13）：`eswin_download_support` 类在 `eswin_tool.py` 第 1912 行；`__init__` 1917、`reset` 1934、`_run_boot_sync_burst` 1973、`_bootrom_sync` 1998、`up_chip_baudrate` 2007。

### 8.3 Workbench 后端

- `backend/worker.py`：JSON 行协议，动作 `catalog/import/install/register/build/plan/flash/devices`；
- `backend/recipes.py`：`build_ecw()`；
- `backend/flashing.py`：ECW 烧录只调 `eswin_tool`；
- `catalog/toolchains.json`：固定版本 `eswin_tool 2.1.13`。

---

## 9. 下一步建议

1. **接通 STM32 对端**：确认 `0x08` 从机、IO24/IO23、上拉；验证 `PROTO_ID_AUDIO_CHUNK`、`PROTO_ID_CLIP_READY`、`PROTO_ID_CLIP_CHUNK`、`PROTO_ID_INTERVAL`、`PROTO_ID_CARD_FORWARD`。
2. **给 I2C 失败加退避/限频**，避免每 10 ms 刷屏。
3. **实现 BLE**：当前只是桩；从 `eswin_ble.h`/NimBLE 或 BSP `bsp_ble.h` 开始，先做广播 + GATT 透传。
4. **音频链路**：`pdm_decode()` 目前是三级 64 抽头滑动和 + DC blocker；可评估音质/延迟，或改用 SDK 的 I2S/DMA 配置。
5. **TTS**：`SYN6288E` 帧协议已实现；可增加文本队列、音量/语速命令。
6. **工程形态**：保留 `board_support` + `build_firmware.sh`；如果必须用 Workbench 编译，再把 board 支持合并进 `main.c`（单文件）。

---

## 10. 证据文件

- `logs/flash_success_20261114.log`：6 分区烧录 + hash 校验成功。
- `logs/boot_after_flash_20261114.log`：启动初始化与 `e_stream` 日志。
- `ecw-build-output/firmware_allinone.bin`：本次验证镜像（sha256 见第 1 节）。
- `ecw-build-output/build_firmware.sh`：可复现构建脚本。
