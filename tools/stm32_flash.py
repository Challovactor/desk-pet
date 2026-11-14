#!/usr/bin/env python3
"""STM32F407 UART Bootloader 烧录器（AN3155 协议，macOS/Linux/Windows 通用）。

用法：
    # 探测（BOOT0=1、BOOT1=0、开关拨到 STM32、复位后运行）
    python3 tools/stm32_flash.py --port /dev/cu.usbserial-2130 --check

    # 烧录 + 校验 + 运行
    python3 tools/stm32_flash.py --port /dev/cu.usbserial-2130 \
        --bin stm32f407/build/firmware.bin --verify --go

需要 pyserial：pip3 install pyserial
烧录完成后：BOOT0=0，复位即可运行新固件。
"""
import argparse
import sys
import time

import serial

ACK, NACK = 0x79, 0x1F
FLASH_BASE = 0x08000000
PID_F407 = 0x0413


class Bootloader:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, bytesize=8, parity="E",
                               stopbits=1, timeout=1.0)
        self.s.rts = False
        self.s.dtr = False
        time.sleep(0.1)

    def _cmd(self, c):
        self.s.write(bytes([c, c ^ 0xFF]))
        r = self.s.read(1)
        if r != bytes([ACK]):
            raise RuntimeError("命令 0x%02X 无 ACK（收到 %r）" % (c, r))

    def _data(self, payload):
        """payload：不含命令字节的数据块（首字节为 N-1 或地址），自带异或校验"""
        chk = 0
        for b in payload:
            chk ^= b
        self.s.write(bytes(payload) + bytes([chk]))
        r = self.s.read(1)
        if r != bytes([ACK]):
            raise RuntimeError("数据帧无 ACK（收到 %r）" % (r))

    def sync(self):
        self.s.reset_input_buffer()
        self.s.write(b"\x7f")
        r = self.s.read(1)
        if r != bytes([ACK]):
            raise RuntimeError(
                "同步失败（收到 %r）：确认 BOOT0=1、BOOT1=0、开关在 STM32 侧并已复位" % (r,))

    def get_id(self):
        self._cmd(0x00)                      # GET
        n = self.s.read(1)[0]
        self.s.read(n + 1)                   # 版本 + 命令表
        pid = int.from_bytes(self.s.read(2), "big")
        if self.s.read(1) != bytes([ACK]):
            raise RuntimeError("GET 帧未正常结束")
        return pid

    def global_erase(self):
        self._cmd(0x44)                      # 扩展擦除
        self.s.timeout = 30.0                # F4 全片擦除较慢
        try:
            self._data([0xFF, 0xFF, 0x00])   # 页数 0xFFFF=全片 + 异或校验 0x00
        finally:
            self.s.timeout = 1.0

    def write(self, addr, data):
        if len(data) % 4:
            data = data + b"\xff" * (4 - len(data) % 4)
        self._cmd(0x31)                      # Write Memory
        a = addr.to_bytes(4, "big")
        self._data(list(a))                  # 地址 + 异或校验
        for off in range(0, len(data), 256):
            chunk = data[off:off + 256]
            self._data([len(chunk) - 1] + list(chunk))

    def read_back(self, addr, size, chunk=256):
        out = bytearray()
        self.s.timeout = 2.0
        while size > 0:
            n = min(chunk, size)
            self._cmd(0x11)                  # Read Memory
            self._data(list((addr + len(out)).to_bytes(4, "big")))
            self._data([n - 1])
            part = self.s.read(n)
            if len(part) != n:
                raise RuntimeError("读回不完整（0x%08X 处只读到 %d/%d）"
                                   % (addr + len(out), len(part), n))
            out += part
            size -= n
        return bytes(out)

    def go(self, addr):
        self._cmd(0x21)                      # Go
        self._data(list(addr.to_bytes(4, "big")))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--bin", help="要烧录的 firmware.bin")
    ap.add_argument("--check", action="store_true", help="只探测 Bootloader 并读芯片 ID")
    ap.add_argument("--verify", action="store_true", help="烧录后读回校验")
    ap.add_argument("--no-go", action="store_true", help="烧完不发 Go（手动复位运行）")
    args = ap.parse_args()

    bl = Bootloader(args.port)
    bl.sync()
    pid = bl.get_id()
    print("bootloader 同步成功，芯片 ID=0x%04X%s" %
          (pid, "（F407 ✓）" if pid == PID_F407 else "（非 F407，继续？）"))
    if args.check:
        return

    if not args.bin:
        sys.exit("--bin 未指定")
    data = open(args.bin, "rb").read()
    print("擦除 %d 字节…" % len(data))
    bl.global_erase()
    print("写入…")
    t0 = time.time()
    bl.write(FLASH_BASE, data)
    print("烧录完成（%.1fs）" % (time.time() - t0))

    if args.verify:
        print("校验…")
        back = bl.read_back(FLASH_BASE, len(data))
        if back != data:
            n = next(i for i, (x, y) in enumerate(zip(back, data)) if x != y)
            sys.exit("校验失败：首个差异在偏移 0x%X" % n)
        print("校验通过")

    if not args.no_go:
        bl.go(FLASH_BASE)
        print("已跳转运行（BOOT0=0 复位后同样从 Flash 启动）")


if __name__ == "__main__":
    main()
