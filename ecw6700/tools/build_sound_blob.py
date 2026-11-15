#!/usr/bin/env python3
"""Build the SD sound-asset blob from animal_audio/.

Output format (little-endian, all offsets from blob start):

  header (32 bytes)
    0  char[8]  magic "MOCESND1"
    8  u16      version = 1
   10  u16      header_size = 32
   12  u32      sample_rate = 16000
   16  u16      channels = 1
   18  u16      bits = 16
   20  u16      count
   22  u16      reserved
   24  u32      total_bytes
   28  u32      blob_crc32 (CRC32 of the whole blob with this field = 0)

  directory entry (40 bytes each)
    0  char[16] name (NUL padded)
   16  u8       class_id: 1 = negative, 2 = positive
   17  u8[3]    reserved
   20  u32      offset (512-byte aligned, from blob start)
   24  u32      bytes (PCM16LE mono)
   28  u32      crc32 of PCM bytes
   32  u32      duration_ms
   36  u32      reserved

  PCM data starts at the first 512-byte boundary after the directory.
  Each entry offset is 512-byte aligned so the firmware can read it with
  raw SD block reads.

The four clips are expected to be named 01_sad, 02_joy, 03_cry, 04_murmur.
1/3 are negative, 2/4 are positive.  Any extra files are ignored, and a
missing expected file is a hard error so a stale SD image cannot silently
lose a mood sound.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import wave
import zlib

try:
    import miniaudio
except ImportError:  # pragma: no cover - friendly message on the build host
    miniaudio = None

SAMPLE_RATE = 16000
CHANNELS = 1
BITS = 16
MAGIC = b"MOCESND1"
VERSION = 1
HEADER_SIZE = 32
ENTRY_SIZE = 40
DIR_ALIGN = 512
FADE_MS = 8
TRIM_PAD_MS = 25
TRIM_THRESHOLD = 0.012  # ~ -38 dBFS

# (filename prefix, class_id, display name)
EXPECTED = [
    ("01_sad", 1, "sad"),
    ("02_joy", 2, "joy"),
    ("03_cry", 1, "cry"),
    ("04_murmur", 2, "murmur"),
]


def decode_to_mono16(path: str) -> list[int]:
    """Decode any supported file to 16 kHz mono signed 16-bit samples."""
    if miniaudio is None:
        raise SystemExit("miniaudio is required: python -m pip install miniaudio")
    # miniaudio's file API passes the path to a C library that mishandles
    # non-ASCII (e.g. Chinese) directory names on Windows; decode from memory.
    with open(path, "rb") as f:
        data = f.read()
    dec = miniaudio.decode(data, output_format=miniaudio.SampleFormat.SIGNED16)
    raw = dec.samples.tobytes() if hasattr(dec.samples, "tobytes") else \
        struct.pack("<%dh" % len(dec.samples), *dec.samples)
    out = miniaudio.convert_frames(
        miniaudio.SampleFormat.SIGNED16, dec.nchannels, dec.sample_rate, raw,
        miniaudio.SampleFormat.SIGNED16, CHANNELS, SAMPLE_RATE,
    )
    return list(struct.unpack("<%dh" % (len(out) // 2), out))


def trim_and_shape(samples: list[int]) -> list[int]:
    if not samples:
        return samples
    threshold = int(TRIM_THRESHOLD * 32768)
    first = 0
    while first < len(samples) and abs(samples[first]) < threshold:
        first += 1
    last = len(samples) - 1
    while last > first and abs(samples[last]) < threshold:
        last -= 1
    if first >= last:
        return samples
    pad = TRIM_PAD_MS * SAMPLE_RATE // 1000
    first = max(0, first - pad)
    last = min(len(samples) - 1, last + pad)
    out = samples[first:last + 1]

    # Normalise each clip to a common -3 dBFS peak so the four moods feel
    # equally loud.  The firmware keeps its own gain divisor for fine tuning.
    peak = max(abs(s) for s in out)
    if peak > 0:
        target = int(0.707 * 32767)
        gain = target / peak
        out = [max(-32768, min(32767, int(round(s * gain)))) for s in out]

    # Short fades remove start/end clicks on the speaker.
    fade = max(1, FADE_MS * SAMPLE_RATE // 1000)
    fade = min(fade, len(out) // 2)
    for i in range(fade):
        g = i / fade
        out[i] = int(out[i] * g)
        out[-1 - i] = int(out[-1 - i] * g)
    return out


def pcm_bytes(samples: list[int]) -> bytes:
    return struct.pack("<%dh" % len(samples), *samples)


def build_blob(assets: list[tuple[str, int, bytes, int]]) -> bytes:
    """assets: list of (name, class_id, pcm, duration_ms)."""
    count = len(assets)
    data_start = HEADER_SIZE + count * ENTRY_SIZE
    data_start = (data_start + DIR_ALIGN - 1) // DIR_ALIGN * DIR_ALIGN

    entries = []
    cursor = data_start
    for name, class_id, pcm, duration_ms in assets:
        cursor = (cursor + DIR_ALIGN - 1) // DIR_ALIGN * DIR_ALIGN
        entries.append((name, class_id, cursor, pcm, duration_ms))
        cursor += len(pcm)
    total = cursor

    blob = bytearray(total)
    blob[0:8] = MAGIC
    struct.pack_into("<HHIHHHHII", blob, 8, VERSION, HEADER_SIZE, SAMPLE_RATE,
                     CHANNELS, BITS, count, 0, total, 0)

    for i, (name, class_id, offset, pcm, duration_ms) in enumerate(entries):
        base = HEADER_SIZE + i * ENTRY_SIZE
        raw_name = name.encode("ascii")[:15]
        blob[base:base + 16] = raw_name + b"\0" * (16 - len(raw_name))
        struct.pack_into("<B3xIIII", blob, base + 16, class_id, offset,
                         len(pcm), zlib.crc32(pcm) & 0xFFFFFFFF, duration_ms)
        blob[offset:offset + len(pcm)] = pcm

    crc = zlib.crc32(blob) & 0xFFFFFFFF
    struct.pack_into("<I", blob, 28, crc)
    return bytes(blob)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--audio-dir", default=os.path.join(os.path.dirname(__file__), "..", "..", "animal_audio"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "main", "assets", "animal_sounds.bin"))
    ap.add_argument("--wav-dir", default="", help="optional directory for preview WAVs")
    args = ap.parse_args()

    audio_dir = os.path.abspath(args.audio_dir)
    out_path = os.path.abspath(args.out)
    files = {}
    for filename in os.listdir(audio_dir):
        stem, ext = os.path.splitext(filename)
        if ext.lower() in (".wav", ".mp3"):
            files[stem] = os.path.join(audio_dir, filename)

    assets = []
    for prefix, class_id, name in EXPECTED:
        match = None
        for stem, path in files.items():
            if stem.lower().startswith(prefix):
                match = path
                break
        if match is None:
            raise SystemExit(f"missing expected audio clip: {prefix}.*")
        samples = trim_and_shape(decode_to_mono16(match))
        pcm = pcm_bytes(samples)
        duration_ms = len(samples) * 1000 // SAMPLE_RATE
        assets.append((name, class_id, pcm, duration_ms))
        print(f"  {os.path.basename(match):16s} -> {name:8s} class={class_id} "
              f"{len(pcm):6d} B {duration_ms:5d} ms")

    blob = build_blob(assets)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(blob)
    stored_crc = struct.unpack_from("<I", blob, 28)[0]
    print(f"wrote {out_path} ({len(blob)} bytes, crc32=0x{stored_crc:08x})")

    if args.wav_dir:
        os.makedirs(args.wav_dir, exist_ok=True)
        for name, _class_id, pcm, _ms in assets:
            with wave.open(os.path.join(args.wav_dir, name + ".wav"), "wb") as w:
                w.setnchannels(CHANNELS)
                w.setsampwidth(2)
                w.setframerate(SAMPLE_RATE)
                w.writeframes(pcm)
    return 0


if __name__ == "__main__":
    sys.exit(main())
