# reSpeaker Clip - Development Log

## Current State (2026-04-17)

### Implemented Features

| Feature | Notes |
|---------|-------|
| PDM Microphone Capture | 16kHz, stereo/mono/merge modes |
| Opus Encoding | Mode-specific bitrate/complexity via Kconfig |
| SpeexDSP Processing | Noise suppression, dereverberation (no AGC - FIXED_POINT limitation) |
| Audio Modes | Normal (stereo, 16kbps/ch, complexity 0), Enhanced (mono, 32kbps, complexity 1) |
| SD Card Storage | FAT32, session directories under /REC/ |
| Session Metadata | session.json, files.lst, marks.bin |
| Bookmark System | Binary marks.bin with notes |
| BLE GATT Service | Command, Response, File Data characteristics |
| AT Command Protocol | 26 commands |
| File Transfer (BLE) | Pause/resume/cancel, session-level resume |
| File Transfer (UDP) | Fire-and-forget with per-file CRC32, file-level retransmit |
| Transport Abstraction | BLE + UDP backends via transport.h |
| WiFi AP Mode | SSID: ClipAP_XXXX, Password: 12345678, IP: 192.168.4.1, Port: 8089 |
| NVS Configuration | 5 settings persist: mode, noise, autodel, dereverb, brightness |
| Battery Monitoring | NPM1300 PMIC + nRF Fuel Gauge (SoC smoothing) |
| Button Handler | Custom input driver: long-press record, short-press bookmark, single-click status |
| OLED Display | CH1115 driver (88x48, I2C), 24x24 icons, 8x16 font, status bar, recording time, battery/charging, low battery fullscreen |
| Haptic Motor | PMIC GPIO2 control (optional, Kconfig) |
| CPU Boost | 128MHz/64MHz reference-counted system |
| Event System | k_msgq + k_sem driven main loop |
| Factory Reset | Config reset + SD card format + reboot |
| Power Off | PMIC ship mode via AT+POWEROFF |
| Time Sync | Unix timestamp via AT+TIME, persisted to NVS |
| Firmware Update (DFU) | MCUmgr SMP OTA DFU via BLE, dual-image, OTA progress display |
| WiFi Client Detection | WiFi AP client connected icon in status bar |
| Session Sorting | AT+LIST sessions sorted newest-first via shared cache |
| Transfer Cancel | Thread-safe cancel via volatile flag (no race with transfer thread) |

### AT Commands (26)

| Command | Type | Purpose |
|---------|------|---------|
| AT+GSTAT | EXEC | Get device status |
| AT+DEVICE | EXEC/GET | Device name |
| AT+VERSION | EXEC | Version info |
| AT+TIME | GET/SET | System time (Unix timestamp) |
| AT+START | EXEC/SET | Start recording |
| AT+STOP | EXEC | Stop recording |
| AT+MARK | EXEC/SET | Add bookmark |
| AT+PAUSE | EXEC | Pause recording |
| AT+RESUME | EXEC | Resume recording |
| AT+CANCEL | EXEC | Cancel transfer |
| AT+LIST | GET/SET | List sessions/files |
| AT+DELETE | SET | Delete session |
| AT+MARKS | GET/SET | Get bookmarks |
| AT+DOWNLOAD | SET | Download file/session |
| AT+PURGEABLE | EXEC | Query cleanable sessions |
| AT+PURGE | EXEC | Delete transferred sessions |
| AT+AUTODEL | GET/SET | Auto-delete policy |
| AT+FORMAT | EXEC | Format SD card |
| AT+POWEROFF | EXEC | Power off (ship mode) |
| AT+WIFI | EXEC/GET/SET | WiFi AP control |
| AT+MODE | GET/SET | Recording mode |
| AT+NOISE | GET/SET | Noise suppression |
| AT+DEREVERB | GET/SET | Dereverberation |
| AT+BRIGHTNESS | GET/SET | OLED brightness |
| AT+PAIR | GET/SET | BLE pairing |
| AT+FACTORY | SET | Factory reset |
| AT+REBOOT | EXEC | Reboot |

### Thread Architecture

| Thread | Priority | Stack | Purpose |
|--------|----------|-------|---------|
| Main | 0 | Default | Event loop, status updates |
| Audio | 0 | 32768 | PDM capture, DSP, Opus encode |
| Transfer | 5 | 16384 | File transfer over BLE/UDP |
| UDP Server | 5 | 4096 | WiFi UDP packet handling |
| AT Server | 7 | 4096 | AT command parsing and dispatch |

### Memory Usage

- FLASH: ~79 KB (secure app image)
- RAM: ~302 KB (total system)

### Recording Modes

| Mode | Audio | Bitrate | Complexity | DSP | Segment Duration |
|------|-------|---------|------------|-----|-----------------|
| Normal | Stereo (L+R) | 16kbps/ch (32kbps total) | 0 | Disabled | 60s (sync) / 300s (no sync) |
| Enhanced | Mono (L+R merged) | 32kbps | 1 | Enabled | 60s (sync) / 300s (no sync) |

### Build & Flash

```sh
# Environment
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)

# Build
west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip

# Flash and reset
west flash --build-dir build-clip && nrfutil device reset

# Serial output
minicom -D /dev/ttyACM0 -b 115200
```

### Not Yet Implemented

| Feature | Priority | Notes |
|---------|----------|-------|
| Low Power Mode | Medium | Sleep when idle |
| Auto-Purge Execution | Medium | Background task to delete old transferred sessions |

---

## Change History

### 2026-04-17 - UI Overhaul and Bug Fixes (v2.0.5)

- UI overhaul: 24x24 icons, 8x16 font, battery fix, charging display
- Low battery (<10%) full-screen display
- Disable idle BT/WiFi icons, add OTA icon, instant transfer display refresh
- Fuel gauge: nRF Fuel Gauge integration, SoC smoothing, charging status
- OTA progress display
- WiFi client connected icon
- Fix FILE_ACK NACK: skip CRC update on UDP send failure
- Fix AT+CANCEL race: cancel handled in transfer thread via volatile flag
- Fix AT+LIST sorting: sessions sorted newest-first with shared cache
- Fix DOWNLOAD response: add bytes field (use %u not %llu)
- Fix Ctrl+C in Python tools: gracefully merge downloaded files on interrupt
- Fix unexpected disconnect: merge files on disconnect/timeout
- Transfer file retry limit increased from 5 to 10

### 2026-03-28 - Documentation and Code Cleanup

### 2026-03-28 - Documentation and Code Cleanup

- Removed AT+BITRATE, AT+COMPLEXITY, AT+AGC, AT+CHUNKSIZE, AT+PROGRESS commands
- Bitrate and complexity are now mode-specific via Kconfig (not runtime configurable)
- AGC removed entirely (SpeexDSP FIXED_POINT build limitation)
- Transfer chunk size is compile-time only (CONFIG_CLIP_TRANSFER_CHUNK_SIZE)
- Unified all config defaults to Kconfig (removed hardcoded macros from config.h)
- Added DSP timing to encode log (avg/min/max for both Opus and DSP)
- Updated all documentation to match current codebase

### 2026-03-27 - WiFi/BLE Coexistence and Transport Refactoring

- Added WiFi/BLE coexistence configuration (nrf_wifi_coex_config_pta/non_pta)
- Added WiFi/BLE coex hardware reset on WiFi stop
- Fixed compiler warnings: net_if_ipv4_addr_add return type, deprecated net_if_ipv4_set_netmask
- Refactored transport layer: transport.h abstraction with BLE + UDP backends
- Rewrote UDP protocol: fire-and-forget with per-file CRC32 (replaced sliding window)
- Fixed button status bar in WIFI_SYNC state
- Added CPU boost system (128MHz/64MHz reference-counted)
- Event-driven main loop (k_msgq + k_sem)

### 2026-03-23 - Audio Recording Optimization

- On-demand encoder/DSP initialization
- Microphone power control (on/off with stabilization delay)
- Removed VBR settings (21ms → ~12ms encode time)
- Increased audio thread stack to 32KB and priority to 0
- Fixed AT command JSON format and buffer check
- Increased AT server thread stack from 2KB to 4KB (stack overflow fix)
- Set HFCLK divider to 1 for maximum CPU frequency (128MHz)

### 2026-02-26 - Mode Mapping Fix and DSP Restriction

- Fixed mode mapping: Normal=stereo (no DSP), Enhanced=mono (with DSP)
- DSP only enabled in enhanced mode
- Bitrate scaling: mono=1x, stereo=2x

### 2026-02-25 - Simultaneous Recording and BLE Transfer

- Added transfer_resume_from() for file-level resume
- Simultaneous recording and BLE transfer
- Disconnect callback cleanup
- Extended AT+DOWNLOAD with session/start_file syntax
- Python sync tool with auto-detection and resume support

### 2026-02-24 - Session Storage Structure

- Session directories: /SD:/REC/YYYYMMDDHHMMSS/
- session.json, files.lst, marks.bin per session
- AT+TIME command for time synchronization
- Fallback session IDs when time not synced

### 2026-02-20 - Initial Implementation

- BLE GATT service with 3 characteristics
- AT command parser (EXEC/SET/GET)
- State machine (IDLE/RECORDING/TRANSMITTING/PAUSED/ERROR)
- NVS configuration storage
- Opus encoding integration
- SpeexDSP noise suppression and dereverberation
- SD card FAT32 storage
- Audio recording thread
- Custom button input driver
