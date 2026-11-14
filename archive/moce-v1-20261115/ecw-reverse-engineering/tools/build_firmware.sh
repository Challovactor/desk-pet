#!/usr/bin/env bash
# Rebuild firmware-code-ff5c7de034b6-ecw6700 for ECW6700F with the pinned
# B02P12 SDK, including the project's board_support unit (board.c + .inc).
# Usage: ./build_firmware.sh [--fresh]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# SDK 的 make 在 realpath 阶段处理不了非 ASCII 路径。若当前是通过中文路径
# 调用，优先切到 ASCII 目录联接 C:\moce 重新执行；没有联接则给出明确提示。
if printf '%s' "$ROOT" | LC_ALL=C grep -q '[^ -~]'; then
  if [ -d /c/moce ] && [ "$(cd /c/moce && pwd -P)" = "$(cd "$ROOT" && pwd -P)" ]; then
    exec bash /c/moce/ecw-build-output/build_firmware.sh "$@"
  fi
  echo "ERROR: 工程路径包含非 ASCII 字符，SDK make 无法处理。" >&2
  echo "请先建立目录联接，再从 ASCII 路径运行：" >&2
  echo "  powershell -NoProfile -Command \"New-Item -ItemType Junction -Path 'C:\\moce' -Target '$(cygpath -w "$ROOT")'\"" >&2
  echo "  cd /c/moce && bash ecw-build-output/build_firmware.sh" >&2
  exit 1
fi

PROOT="$ROOT"                       # POSIX style, for PATH
WROOT="$(cygpath -m "$ROOT")"       # Windows style, for make variables

TOOLS_REL="MOCEWorkbench-data/tools/ecw6700-windows"
TOOLS="$ROOT/$TOOLS_REL"
PY="$ROOT/MOCEWorkbench/runtime/python.exe"
BSP_SDK="$TOOLS/bsp/eswin/sdk/b02p12"
PROJECT="$ROOT/firmware-code-ff5c7de034b6-ecw6700"
WORK="$ROOT/ecw-build-work"
SDK="$WORK/sdk"
CODE="$SDK/ECW6700FSDK_V1.0.0B02P12/code"
BOARD="$CODE/Boards/ecw6700f/standalone"
OUT="$SCRIPT_DIR"

if [ "${1:-}" = "--fresh" ]; then
  rm -rf "$SDK"
fi
mkdir -p "$WORK" "$WORK/tmp"

# 1. Extract the pinned SDK and apply the reviewed overlays.
if [ ! -d "$CODE" ]; then
  echo "[1/6] preparing SDK (this extracts ~300 MB)"
  "$PY" -I -B -X pycache_prefix="$(cygpath -m "$WORK")/tmp/pycache" \
        "$(cygpath -m "$BSP_SDK")/prepare_sdk.py" --output "$(cygpath -m "$SDK")"
else
  echo "[1/6] reusing prepared SDK: $CODE"
fi

# 2. Windows-host adaptations for the vendor Makefiles (same as MOCE Workbench).
echo "[2/6] applying Windows makefile adaptations"
export ECW_CODE="$(cygpath -m "$CODE")"
"$PY" -I - <<'PYEOF'
import os, pathlib
code = pathlib.Path(os.environ['ECW_CODE'])
for rel, old, new in [
    ('components/wpa/Makefile',
     '$(subst :, ,$(VPATH))',
     '$(subst :, ,$(subst $(CURRENT_MAKEFILE),.,$(VPATH)))'),
    ('components/sntp/Makefile',
     '$(subst :, ,$(VPATH))',
     '$(subst :, ,$(subst $(TOPDIR),../..,$(VPATH)))'),
]:
    p = code / rel
    text = p.read_text(encoding='utf-8')
    if new not in text:
        p.write_text(text.replace(old, new), encoding='utf-8', newline='\n')
p = code / 'Boards/ecw6700f/makefile'
text = p.read_text(encoding='utf-8')
line = '\t$(Q) $(CC) -E -nostdinc -P $(LD_SCRIPT_CPPFLAGS) $< -o $@'
if '$(LDSCRIPT).rsp' not in text:
    if text.count(line) != 1:
        raise SystemExit('linker recipe mismatch')
    text = text.replace(
        line,
        '\t$(file >$(LDSCRIPT).rsp,$(call win_rsp,$(LD_SCRIPT_CPPFLAGS)))\n'
        '\t$(Q) $(CC) -E -nostdinc -P @$(LDSCRIPT).rsp $< -o $@', 1)
    p.write_text(text, encoding='utf-8', newline='\n')
PYEOF

# 3. Wire the application entry point into the SDK startup file.
echo "[3/6] patching startup entry point"
export ECW_BOARD="$(cygpath -m "$BOARD")"
"$PY" -I - <<'PYEOF'
import os, pathlib
board = pathlib.Path(os.environ['ECW_BOARD'])
startup = board / 'main.c'
text = startup.read_text(encoding='utf-8')
marker = '/*---------------------------------- user code start ----------------------------------------*/'
if text.count(marker) != 1:
    raise SystemExit('startup marker mismatch')
if 'moce_app_main();' not in text:
    text = text.replace(marker, marker + '\n    extern void moce_app_main(void);\n    moce_app_main();', 1)
    startup.write_text(text, encoding='utf-8', newline='\n')
PYEOF

# 4. Copy the project into the SDK and add board_support + app modules.
echo "[4/6] staging project sources"
rm -f "$BOARD"/moce_application.c "$BOARD"/app_*.c "$BOARD"/app_*.h
cp "$PROJECT/main/main.c" "$BOARD/moce_application.c"
for f in "$PROJECT"/main/app_*.c; do
  if [ -e "$f" ]; then cp "$f" "$BOARD/"; fi
done
for f in "$PROJECT"/main/app_*.h; do
  if [ -e "$f" ]; then cp "$f" "$BOARD/"; fi
done
rm -rf "$BOARD/board_support"
cp -r "$PROJECT/main/board_support" "$BOARD/board_support"
cp "$BOARD/board_support/board.c" "$BOARD/moce_board_support.c"

# Rewrite Make.defs from the known B02P12 baseline + the MOCE block.
# (Deterministic and idempotent: repeated builds must not duplicate CSRCS.)
cat > "$BOARD/Make.defs" <<'EOF'
CSRCS += main.c rf_cal_config.c

VPATH += :ecw6700f/standalone
INCPATHS += ecw6700f/standalone/include

# ---- MOCE application sources (re-generated) ----
CSRCS += moce_application.c
CSRCS += moce_board_support.c
CSRCS += app_audio.c
CSRCS += app_store.c
CSRCS += app_sdspi.c
CSRCS += app_link.c
CSRCS += app_net.c
INCPATHS += $(BOARD_DIR) $(BOARD_DIR)/board_support
EOF

# 5. Build with the vendor GCC/ld; --wrap preserves board.c's I2C pinmux override.
echo "[5/6] building"
export PATH="$PROOT/$TOOLS_REL/compiler/bin:$PROOT/$TOOLS_REL/make/bin:$PROOT/$TOOLS_REL/git/usr/bin:$PROOT/MOCEWorkbench/runtime/tools/bin:$PROOT/MOCEWorkbench/runtime:$PATH"
export RISCV_GCC_PATH="$WROOT/$TOOLS_REL/compiler/bin/"
export MSYS_NO_PATHCONV=1
export TEMP="$WROOT/ecw-build-work/tmp" TMP="$WROOT/ecw-build-work/tmp" TMPDIR="$WROOT/ecw-build-work/tmp"
export PYTHONIOENCODING=utf-8

# Recompute the toolchain-relative path in Windows form for make variables.
( cd "$BOARD" && make -j2 all PARALLEL=-j2 V=1 SDK_V=V1.0.0B02P12 REL_V=1.0.0 \
    "SHELL=$WROOT/MOCEWorkbench-data/tools/ecw6700-windows/git/usr/bin/sh.exe" \
    OS_TYPE_CFG=Windows \
    "LD=$WROOT/MOCEWorkbench-data/tools/ecw6700-windows/compiler/bin/riscv64-unknown-elf-ld.exe --wrap=chip_i2c_pinmux_init" ) \
  > "$WORK/build.log" 2>&1

# 6. Export the verified artifacts.
echo "[6/6] exporting artifacts"
BUILT="$CODE/build/ecw6700f/standalone"
cp "$BUILT/standalone.elf" "$OUT/firmware.elf"
cp "$BUILT/standalone.map" "$OUT/firmware.map"
cp "$BUILT/standalone.flash" "$OUT/firmware.flash"
cp "$BUILT/ECW6700F_standalone_cpu_0x14000.bin" "$OUT/firmware_cpu.bin"
cp "$BUILT"/ECW6700F_standalone_*_allinone.bin "$OUT/firmware_allinone.bin"
sha256sum "$OUT/firmware_allinone.bin"
echo "OK: $OUT/firmware_allinone.bin"
