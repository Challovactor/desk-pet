cmake_minimum_required(VERSION 3.16)
project(moce_stm32_touch C ASM)

# 本地固定版本的 STM32F407 BSP / HAL / CMSIS / 工具链。
# 构建示例：
#   cmake -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE="$MOCE_BSP/integration/arm-gcc.cmake" \
#     -DARM_BIN="$MOCE_TOOLS/compiler/bin" \
#     -DMOCE_BSP_ROOT="$MOCE_BSP" \
#     -DMOCE_LINKER_SCRIPT="$MOCE_BOARD/memory.ld"
set(MOCE_BSP_ROOT "" CACHE PATH "STM32F407 BSP 根目录 (bsp/st/bsp/stm32f407/<ver>)")
set(MOCE_BOARD_DIR "" CACHE PATH "板级绑定目录 (boards/stm32f407vet6/schematic-*)")
set(MOCE_LINKER_SCRIPT "" CACHE FILEPATH "链接脚本 memory.ld")

if(NOT MOCE_BSP_ROOT)
  message(FATAL_ERROR "必须指定 -DMOCE_BSP_ROOT=<BSP 根目录>")
endif()
if(NOT MOCE_LINKER_SCRIPT)
  set(MOCE_LINKER_SCRIPT "${MOCE_BOARD_DIR}/memory.ld")
endif()

add_subdirectory(main)
