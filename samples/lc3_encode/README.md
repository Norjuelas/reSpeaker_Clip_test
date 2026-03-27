# LC3 Audio Encoder Sample

This sample demonstrates LC3 audio encoding on the ReSpeaker Clip using the nRF5340's LC3 codec support.

## Features

- **LC3 Codec**: Low Complexity Communication Codec (Bluetooth LE Audio standard)
- **Audio Capture**: PDM microphone capture at 16 kHz, 16-bit
- **Audio Modes**: Mono (left channel), Stereo, or Merged (L+R mixed)
- **Output Streams**: UART, BLE, and SD card
- **Optional**: SpeexDSP preprocessing (noise suppression, dereverb)

## LC3 vs Opus

| Feature | LC3 | Opus |
|---------|-----|------|
| Frame Duration | 7.5ms, 10ms | 2.5ms, 5ms, 10ms, 20ms, 60ms |
| Sample Rates | 16kHz, 48kHz | 8kHz - 48kHz |
| Bitrate Range | 16-160 kbps | 6-510 kbps |
| Use Case | Bluetooth LE Audio | General VoIP/streaming |
| Complexity | Lower | Higher |
| Latency | Very low | Low to medium |
| Codec Size | ~20 KB | ~85 KB |

## Configuration

```
Sample Rate:     16 kHz
Frame Duration:  10 ms (160 samples)
Frame Size:      320 bytes (stereo), 160 bytes (mono)
Bitrate:         32 kbps (mono), 64 kbps (stereo)
```

## Building

```bash
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
west build --build-dir build-lc3 --board clip/nrf5340/cpuapp samples/lc3_encode
```

## Flash Usage

Expected flash usage: ~250 KB (vs ~420 KB for Opus)

Savings from Opus:
- LC3 codec: ~20 KB (vs Opus ~85 KB)
- Smaller frame buffers: ~2 KB

## Usage

Connect via UART (115200 baud) and use the following commands:

- `s` - Start streaming
- `e` - Stop streaming
- `1` - Set mono mode
- `2` - Set stereo mode
- `3` - Set merge mode
- `u` - Toggle UART output
- `d` - Toggle SD card save
- `b` - Toggle BLE streaming
- `l` - List SD card files
- `p` - Toggle SpeexDSP (if enabled)
- `q` - Quit

## Output Format

The encoder outputs LC3 frames in the following format:

```
>>> LC3_STREAM_START
SAMPLE_RATE=16000
CHANNELS=2
FRAME_SIZE=160
FRAME_DURATION_US=10000
BITRATE=64000
>>> DATA_START
0080
<hex data>
0081
<hex data>
...
>>> DATA_END
```

Each frame consists of:
- 4-character hex length (e.g., "0080" = 128 bytes)
- Hex-encoded LC3 data

## Python Receiver

Use the `receive_lc3.py` script to receive and decode LC3 audio:

```bash
python3 receive_lc3.py --output output.lc3
```

## Notes

- LC3 is optimized for Bluetooth LE Audio but can be used independently
- The 10ms frame duration provides a good balance between latency and quality
- For lower latency, use 7.5ms frame duration (modify `LC3_FRAME_DURATION_US`)
