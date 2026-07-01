---
name: clip-dev
description: |
  reSpeaker Clip firmware development guide for Zephyr RTOS on nRF5340.
  Use when building firmware, configuring MCUboot, working with audio pipeline
  (PDM/Opus/SpeexDSP), BLE AT commands, WiFi AP/UDP transfer, OLED display,
  SD card storage, PMIC power management, or nRF5340 dual-core architecture.
  Trigger on board name clip/nrf5340/cpuapp, wireless coexistence, OTA DFU,
  and Chinese terms such as 录音, 烧录, 固件, 麦克风.
compatibility: Requires NCS v3.3.0, Zephyr RTOS, west build tool, nRF Connect SDK.
version: 1.0.0
---

# reSpeaker Clip Development Skill

Zephyr RTOS firmware for the Seeed reSpeaker Clip voice recording device,
based on Nordic nRF5340 dual-core MCU with BLE, WiFi AP, OLED display,
PDM microphone array, SD card storage, and OTA firmware updates.

## Instructions

### Step 1: Confirm the build environment

Before building or modifying firmware, confirm:
- NCS v3.3.0 installed at `~/ncs/v3.3.0/`
- Zephyr environment sourced
- `ZEPHYR_EXTRA_MODULES` set to project root

```bash
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
```

### Step 2: Identify the target subsystem

| Task | Key Files | Reference |
|------|-----------|-----------|
| Build/flash firmware | `CLAUDE.md` | [references/build-flash.md](references/build-flash.md) |
| MCUboot app development | `docs/mcuboot_app_development.md` | [references/mcuboot.md](references/mcuboot.md) |
| Audio recording/encoding | `applications/clip/src/audio.c` | [references/audio.md](references/audio.md) |
| BLE AT commands | `applications/clip/src/at_commands.c` | [references/ble-at.md](references/ble-at.md) |
| WiFi AP / UDP transfer | `applications/clip/src/wifi.c` | [references/wifi-udp.md](references/wifi-udp.md) |
| OLED display | `applications/clip/src/display.c` | [references/display.md](references/display.md) |
| SD card storage | `applications/clip/src/storage.c` | [references/storage.md](references/storage.md) |
| Power management | `applications/clip/prj.conf` | [references/power.md](references/power.md) |
| Board hardware/DT | `boards/seeed/clip/` | [references/hardware.md](references/hardware.md) |

---

## Quick Reference

### Board Identifier

```
clip/nrf5340/cpuapp        # Application core
clip/nrf5340/cpunet        # Network core (auto-built)
```

### Build & Flash

```bash
# Incremental build
west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip

# Clean build
west build --build-dir build-clip --pristine --board clip/nrf5340/cpuapp applications/clip

# Flash + reset
west flash --build-dir build-clip --recover && nrfutil device reset

# Build a sample with MCUboot
west build --build-dir build-hello --pristine --board clip/nrf5340/cpuapp samples/hello_world
```

### Flash Partition Layout

```
Internal Flash (1MB)
┌──────────────────────┐ 0x00000000
│  MCUboot (88KB)      │  Custom: OLED, USB CDC, SD erase
├──────────────────────┤ 0x00016000
│  App Secure (264KB)  │  ← Primary slot
├──────────────────────┤ 0x00058000
│  App NS (192KB)      │
├──────────────────────┤ 0x00088000
│  OTA Secure (256KB)  │  ← Secondary slot
├──────────────────────┤ 0x000C8000
│  OTA NS (192KB)      │
└──────────────────────┘ 0x00100000

External SPI Flash (64MB, SPI3)
├── OTA app staging (960KB)
├── OTA netcore staging (256KB)
└── LittleFS (~6.8MB)
```

### Key Peripherals

| Peripheral | Bus | Device | Pin/Address |
|------------|-----|--------|-------------|
| PDM mics | PDM0 | T5838 array | alias: dmic0 |
| OLED | I2C2 | CH1115 @ 0x3c | reset: gpio1.9 |
| PMIC | I2C1 | NPM1300 @ 0x6b | 5 GPIOs |
| SPI flash | SPI3 | PY25Q64H | CS: gpio0.20 |
| SD card | SPI4 | SDHC-SPI | CS: gpio0.9 |
| WiFi | QSPI | nRF7002 | — |
| Button | GPIO1.15 | Active-low | Pull-up |

### Event System

Central dispatcher (`clip_event.c`) with states:

```
UNINITIALIZED → IDLE → RECORDING → TRANSPORT_IDLE
                                ↗ TRANSMITTING
                                ↗ WIFI_SYNC
                                → PAUSED
                                → ERROR
```

Events: `START`, `STOP`, `PAUSE`, `RESUME`, `MARK`, `WIFI_ON`, `WIFI_OFF`, etc.

### Audio Pipeline

```
PDM microphones → SpeexDSP (NS/AGC/Dereverb) → Opus encode → Storage/Transfer
```

Modes: `mono` (L only), `merge` (L+R average), `stereo` (L+R)

### BLE AT Commands (JSON responses)

| Command | Function |
|---------|----------|
| `AT+RECORD` / `AT+STOP` | Recording control |
| `AT+LIST` / `AT+LIST?page&per_page` | Session listing |
| `AT+DOWNLOAD=<id>` | Start file transfer |
| `AT+CANCEL` | Cancel transfer |
| `AT+DELETE=<id>` / `AT+PURGE` | Session management |
| `AT+WIFI=on\|off` | WiFi AP control |
| `AT+MODE` / `AT+NOISE` / `AT+DEREVERB` | Audio config |
| `AT+BRIGHTNESS` | OLED brightness |
| `AT+TIME=<ts>` | Time sync |
| `AT+MARKS=<id>` | Bookmark management |

### WiFi AP Configuration

- SSID: `ClipAP_XXXX` (last 4 hex of chip ID)
- Password: `12345678` (default; becomes random after the first BLE pairing — `config_generate_wifi_password()`)
- IP: `192.168.4.1`
- UDP Port: `8089`

### Python Tools

```bash
pip install -r applications/clip/tests/requirements.txt

# UDP file sync
python applications/clip/tests/tools/udp_sync.py --session <id>
python applications/clip/tests/tools/udp_sync.py --all-sessions

# Recording tool
python applications/clip/tests/tools/record.py

# UDP terminal
python applications/clip/tests/tools/udp_terminal.py
```

---

## Known Pitfalls

| Pitfall | Solution |
|---------|----------|
| `%llu` not supported | Use `%u` with `(unsigned int)` cast for 64-bit values |
| UDP `sendto()` silently drops | Returns success even when TX queue full. Use CRC + file retry |
| `except Exception` misses Ctrl+C | Use `except:` or handle `KeyboardInterrupt` explicitly |
| FAT directory not chronological | Session listing uses cached sorted buffer |
| Transfer thread safety | Use `volatile` flags for coordination between AT and transfer threads |
| MCUboot FLASH overflow | `pm_static_clip_nrf5340_cpuapp.yml` is required in every app |
| Signing key mismatch | Use key from `applications/clip/sysbuild/`, NOT `bootloader/` |

---

## MCUboot Sample Development

Samples are in `samples/` with MCUboot support pre-configured. Key files per sample:

```
samples/<name>/
├── sysbuild.conf           # MCUboot sysbuild settings
├── pm_static_clip_nrf5340_cpuapp.yml  → ../_mcuboot/pm_static_*.yml
└── sysbuild/               → ../_mcuboot/sysbuild/*
```

Two sysbuild.conf tiers:
- **Base** (all samples): MCUboot + netcore IPC + dual-image OTA
- **WiFi** (http_server, wifi_ap_iperf, wifi_ble_coex): adds `SB_CONFIG_WIFI_NRF70=y`

See `docs/mcuboot_app_development.md` for full guide.

---

## MCUboot Patch Development

Patches in `patches/mcuboot/`. Workflow:
1. Edit source in `~/ncs/v3.3.0/bootloader/mcuboot/`
2. Build with `--pristine`
3. Export: `git diff > patches/mcuboot/NNNN-name.patch`
4. New files: use `sed 's/^/+'/` prefix
5. Verify: reset → apply patches → build

---

## Output Firmware

```bash
VERSION=$(grep APP_VERSION_STRING build-clip/clip/zephyr/include/generated/zephyr/app_version.h | cut -d'"' -f2)
mkdir -p output/$VERSION

cp build-clip/merged.hex output/$VERSION/
cp build-clip/merged_CPUNET.hex output/$VERSION/
cp build-clip/dfu_application.zip output/$VERSION/clip-$VERSION-ota.zip
```

---

## References

Detailed subsystem documentation in `references/` directory:

| File | Topic |
|------|-------|
| [build-flash.md](references/build-flash.md) | Build commands, flash methods, output files |
| [mcuboot.md](references/mcuboot.md) | MCUboot development, OTA, signing keys |
| [audio.md](references/audio.md) | Audio pipeline, PDM, Opus, SpeexDSP |
| [ble-at.md](references/ble-at.md) | BLE AT command protocol |
| [wifi-udp.md](references/wifi-udp.md) | WiFi AP, UDP file transfer protocol |
| [display.md](references/display.md) | CH1115 OLED, icons, UI |
| [storage.md](references/storage.md) | SD card, FAT32, session management |
| [power.md](references/power.md) | PM device runtime, PMIC, low power |
| [hardware.md](references/hardware.md) | Board DTS, pinout, peripherals |

---

## Related Skills

- [mcuboot_app_development](../../docs/mcuboot_app_development.md) - MCUboot developer guide
- [protocol](../../docs/protocol.md) - BLE AT command protocol spec
- [udp_protocol](../../docs/udp_protocol.md) - UDP file transfer protocol spec
- [architecture](../../docs/architecture.md) - System architecture design

---

**Version**: 1.0.0 | **Board**: clip/nrf5340/cpuapp | **RTOS**: Zephyr v3.3.0
