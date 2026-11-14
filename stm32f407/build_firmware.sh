#!/usr/bin/env bash
# STM32F407 触摸桥固件可复现构建。
#
# 关键点：工程路径可能含中文/空格，而 GNU Arm 工具链通过自身相对路径查找
# cc1/libgcc，所以这里在 C:\temp\armgcc 建一个指向 compiler 根目录的目录联接
# （不是 bin 目录），再用 ASCII 路径调用编译器。构建目录也放在 C:\temp 下。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLS="$ROOT/MOCEWorkbench-data/tools/st-windows"
BSP="$TOOLS/bsp/st/bsp/stm32f407/1.0.0-alpha.1"
BOARD="$TOOLS/bsp/st/boards/stm32f407vet6/schematic-20261005-binding-1.0.0-alpha.1"
CM="$TOOLS/cmake/bin/cmake.exe"
NINJA="$(cygpath -m "$TOOLS/ninja/ninja.exe")"
BUILD_WIN="C:/temp/moce_stm32_build"

# 1) 确保编译器的 ASCII 目录联接存在（GCC 需要用 ASCII 路径查找 cc1/libgcc）
mkdir -p /c/temp
if [ ! -e /c/temp/armgcc/bin/arm-none-eabi-gcc.exe ]; then
  echo "[1/4] creating C:\\temp\\armgcc junction"
  powershell -NoProfile -Command "\$ErrorActionPreference='Stop'; \
    \$t='$(cygpath -w "$TOOLS/compiler")'; \
    if (Test-Path 'C:\temp\armgcc') { Remove-Item 'C:\temp\armgcc' -Force -Recurse }; \
    New-Item -ItemType Junction -Path 'C:\temp\armgcc' -Target \$t | Out-Null"
  if [ ! -e /c/temp/armgcc/bin/arm-none-eabi-gcc.exe ]; then
    echo "ERROR: 无法创建 C:\\temp\\armgcc 目录联接" >&2
    exit 1
  fi
else
  echo "[1/4] compiler junction ok"
fi

# 2) 配置
echo "[2/4] cmake configure"
rm -rf "$BUILD_WIN"
"$CM" -S "$(cygpath -m "$SCRIPT_DIR")" -B "$BUILD_WIN" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$BSP/integration/arm-gcc.cmake")" \
  -DARM_BIN="C:/temp/armgcc/bin" \
  -DMOCE_BSP_ROOT="$(cygpath -m "$BSP")" \
  -DMOCE_LINKER_SCRIPT="$(cygpath -m "$BOARD/memory.ld")"

# 3) 编译
echo "[3/4] build"
"$CM" --build "$BUILD_WIN"

# 4) 产物拷回工程目录
echo "[4/4] export artifacts"
mkdir -p "$SCRIPT_DIR/build"
cp "$BUILD_WIN/firmware.elf" "$SCRIPT_DIR/build/"
cp "$BUILD_WIN/firmware.bin" "$SCRIPT_DIR/build/"
cp "$BUILD_WIN/firmware.hex" "$SCRIPT_DIR/build/"
cp "$BUILD_WIN/firmware.map" "$SCRIPT_DIR/build/"
ls -la "$SCRIPT_DIR/build"
