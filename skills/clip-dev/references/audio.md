# Audio Pipeline Reference

## Pipeline Overview

```
PDM DMIC (16kHz, 2ch, 20ms frames)
    │
    ▼  (DMA, memory slab buffers: 16 x 1280 B)
Audio Thread: dmic_read() → 1280-byte block
    │
    ▼
process_pcm_frame():
    STEREO mode → pass through unchanged
    MERGE mode  → (L + R) / 2 → SpeexDSP (noise suppress + dereverb, NO AGC)
    │
    ▼
Opus Encoder (20ms frames, 320 samples)
    │
    ▼
storage_write_frame(): [2-byte len][Opus packet] → SD card
```

Source: `applications/clip/src/audio.c`, `docs/architecture.md` §3.5.

## Recording Modes

| Mode | Audio Mode | Channels | Bitrate | Complexity | DSP | Segment Duration |
|------|-----------|----------|---------|------------|-----|------------------|
| Normal | STEREO | 2 (stereo Opus) | 32kbps (16k/ch) | 0 | None | 5 min (300s) |
| Enhanced | MERGE | 1 (mono Opus) | 32kbps | 1 | Noise suppress + dereverb | 2 min |

Bitrate/complexity are **build-time per-mode Kconfig constants** — NOT runtime configurable. AGC is unsupported (SpeexDSP FIXED_POINT limitation).

## Audio Constants

```c
#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_SAMPLE_BITS     16
#define AUDIO_CHANNELS        2
#define AUDIO_FRAME_MS        20
#define AUDIO_OPUS_FRAME_SIZE 320   // 16000 * 0.020
#define AUDIO_BLOCK_SIZE      1280  // (16/8) * (16000*20/1000) * 2
#define AUDIO_MAX_PACKET_SIZE 4000
```

Memory: `K_MEM_SLAB_DEFINE_STATIC(audio_mem_slab, AUDIO_BLOCK_SIZE, 16, 4)` — 16 blocks of 1280 bytes.

## Audio Thread

- **Priority**: 0, **Stack**: 32768 (`CONFIG_CLIP_AUDIO_STACK_SIZE`)
- Blocks on `audio_start_sem` when idle. While recording: read DMIC → process PCM → encode Opus → write storage → manage segment files.
- **DMIC recovery**: after 5 consecutive timeouts, DMIC is auto-retriggered.

## Segment File Management

Recording splits into segment files. Duration adapts to transfer state:
- During sync: `CLIP_AUDIO_SEGMENT_DURATION_SYNC` (60s)
- Not syncing: `CLIP_AUDIO_SEGMENT_DURATION_NO_SYNC` (300s)
- Transfer starts mid-file → immediate slice if file exceeds sync duration

A `file_closed_sem` signals the transfer thread when a file is ready for transfer (enables live recording sync).

## Pause/Resume

- **Pause**: stop DMIC → power off microphone → close current file → keep session open
- **Resume**: create new segment file (incremented index) → power on mic → restart DMIC

## Audio Visualization

Energy level (0-10) from RMS of each frame → BLE notifications ~every 200ms.
13-sample history packed into 7 bytes (4 bits per sample). See `ble-at.md` for format.

## Key Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `CLIP_NORMAL_BITRATE` | 16000 | Normal mode Opus bitrate per channel (bps) |
| `CLIP_NORMAL_COMPLEXITY` | 0 | Normal mode Opus complexity |
| `CLIP_ENHANCED_BITRATE` | 32000 | Enhanced mode Opus bitrate (bps) |
| `CLIP_ENHANCED_COMPLEXITY` | 1 | Enhanced mode Opus complexity |
| `CLIP_DEFAULT_NOISE` | 15 | Default noise suppression (dB) |
| `CLIP_DEFAULT_DEREVERB` | n | Default dereverberation |
| `CLIP_AUDIO_STACK_SIZE` | 32768 | Audio thread stack |
| `CLIP_AUDIO_SEGMENT_DURATION_SYNC` | 60 | Segment duration during transfer (s) |
| `CLIP_AUDIO_SEGMENT_DURATION_NO_SYNC` | 300 | Segment duration without transfer (s) |
| `CONFIG_OPUS_EMBEDDED` | y | Enable Opus library |
| `CONFIG_SPEEXDSP` | y | Enable SpeexDSP library |

## Key Source Files

- `applications/clip/src/audio.c` — pipeline, audio thread, DMIC capture
- `applications/clip/include/audio.h` — public API
- `lib/opus/` — Opus codec library (enable `CONFIG_OPUS_EMBEDDED=y`)
- `lib/speexdsp/` — SpeexDSP preprocessing (enable `CONFIG_SPEEXDSP=y`)

## Key Functions

| Function | Purpose |
|----------|---------|
| `audio_start()` / `audio_stop()` | Begin/end a recording session |
| `audio_pause()` / `audio_resume()` | Pause/resume (new segment on resume) |
| `process_pcm_frame()` | Mode-dependent DSP: STEREO passthrough vs MERGE+SpeexDSP |
| `audio_add_bookmark()` | Record bookmark at current position |
| `clip_cpu_boost_acquire()` / `clip_cpu_boost_release()` | Ref-counted 128MHz (recording) / 64MHz (idle) |

## Opus File Format

Each `.opus` file is a sequence of frames:
```
[2 bytes length LE][Opus frame data][2 bytes length][Opus frame data]...
```
Frame size: typically 20ms @ 16kHz = 320 samples.

## Continuous Sync (Real-time)

Client can start `AT+DOWNLOAD` during active recording. Device streams files as written to SD. On `AT+STOP`, device sends `TRANSFER_DONE`. Used by `record.py` and `clip-web.py` via `SessionSync(continuous=True)`.
