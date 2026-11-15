# ecw6700/tools

## build_sound_blob.py

把仓库根目录 `animal_audio/` 的 4 个音效转换成固件内嵌的
`main/assets/animal_sounds.bin`。

```bash
python -m pip install miniaudio
python ecw6700/tools/build_sound_blob.py
# 可选：同时输出可试听的 16 kHz WAV
python ecw6700/tools/build_sound_blob.py --wav-dir ecw6700/tools/preview_wav
```

- 期望文件名前缀：`01_sad`、`02_joy`、`03_cry`、`04_murmur`；
  1/3 为负面情绪，2/4 为正面情绪。
- 输出统一为 16 kHz / 16-bit / 单声道 PCM；自动裁剪首尾静音、
  统一峰值到 -3 dBFS、加 8 ms 淡入淡出。
- blob 格式与 SD 入库逻辑见
  [docs/07-v2-gesture-sound.md](../../docs/07-v2-gesture-sound.md)。
- 修改音效后必须重新生成并重新烧录；固件启动时会检测 CRC 并覆盖
  SD 上的旧资产。

构建脚本 `ecw-build-output/build_firmware.sh` 会把
`main/assets/*.bin` 复制到 SDK 板级目录，供 `app_sounds_blob.c` 的
`.incbin` 使用。不要在构建时漏掉这一步。
