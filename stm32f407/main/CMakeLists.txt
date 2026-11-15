file(GLOB HAL_SRCS "${MOCE_BSP_ROOT}/vendor/stm32f4xx-hal-driver-*/Src/*.c")
# 模板文件依赖未启用的模块，不能参与编译
list(FILTER HAL_SRCS EXCLUDE REGEX "_template\\.c$")
file(GLOB CMSIS_DEVICE_DIRS "${MOCE_BSP_ROOT}/vendor/cmsis-device-f4-*")
list(GET CMSIS_DEVICE_DIRS 0 CMSIS_DEVICE)
file(GLOB HAL_INC_DIRS "${MOCE_BSP_ROOT}/vendor/stm32f4xx-hal-driver-*/Inc")

add_executable(firmware
  main.c
  app_epd.c
  app_logo.c
  app_sound_stm32.c
  app_sounds_data.c
  app_lcd.c
  app_lcd_logo.c
  epd_logo.c
  syscalls.c
  ${HAL_SRCS}
  "${CMSIS_DEVICE}/Source/Templates/system_stm32f4xx.c"
  "${CMSIS_DEVICE}/Source/Templates/gcc/startup_stm32f407xx.s"
)

target_include_directories(firmware PRIVATE
  "${MOCE_BSP_ROOT}/vendor/cmsis"
  "${CMSIS_DEVICE}/Include"
  "${MOCE_BSP_ROOT}/integration"
  ${HAL_INC_DIRS}
)

target_compile_definitions(firmware PRIVATE
  STM32F407xx
  USE_HAL_DRIVER
  HSE_VALUE=8000000U
)

target_compile_options(firmware PRIVATE
  -Os -Wall -Wextra -Wno-unused-parameter
  -ffunction-sections -fdata-sections
)

target_link_options(firmware PRIVATE
  "-T${MOCE_LINKER_SCRIPT}"
  "-Wl,-Map=${CMAKE_BINARY_DIR}/firmware.map"
  "-Wl,--gc-sections"
  "-Wl,--print-memory-usage"
)

target_link_libraries(firmware PRIVATE m)

set_target_properties(firmware PROPERTIES
  SUFFIX ".elf"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_custom_command(TARGET firmware POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary "$<TARGET_FILE:firmware>" "${CMAKE_BINARY_DIR}/firmware.bin"
  COMMAND ${CMAKE_OBJCOPY} -O ihex   "$<TARGET_FILE:firmware>" "${CMAKE_BINARY_DIR}/firmware.hex"
  COMMAND ${CMAKE_SIZE} "$<TARGET_FILE:firmware>"
)
