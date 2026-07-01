# reSpeaker Clip Firmware

Zephyr RTOS firmware for the **Seeed reSpeaker Clip** — a wearable voice
recording device based on the Nordic nRF5340 dual-core MCU, with BLE, WiFi AP,
USB, AT-command control, and UDP file transfer.

> **Note**: Product name is spelled **reSpeaker** (lowercase `r`).

## Hardware

| Component | Part |
|-----------|------|
| MCU | nRF5340 (Application core + Network core, dual-core) |
| WiFi | nRF7002 (QSPI, AP mode) |
| PMIC / Charger | NPM1300 + nRF Fuel Gauge |
| Display | CH1115 OLED (88×48) |
| Audio | PDM microphone array (DMIC) |
| Storage | microSD (FAT) + 64 Mbit external SPI flash (LittleFS) |
| Connectivity | BLE 5.x + WiFi 2.4/5G AP + USB CDC ACM + USB MSC |

## Key Features

- **Audio**: PDM mic → SpeexDSP preprocessing (noise suppression / AGC / dereverb) → Opus encoding
- **BLE**: AT-command protocol, OTA DFU (MCUmgr), GATT notifications
- **WiFi**: AP mode (`ClipAP_XXXX`) with UDP file transfer (CRC32-verified)
- **USB**: CDC ACM serial (3rd AT channel) + MSC mass storage (SD card)
- **Power**: Production idle ~170µA (DCDC, SD power-gating, console off)
- **Battery**: NPM1300 charging + nRF Fuel Gauge SoC, custom "240" cell model
- **OTA**: MCUboot (custom) with signed images, BLE/USB serial DFU

## Getting Started

### Prerequisites

- [nRF Connect SDK (NCS) v3.3.0](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/index.html)
- Zephyr SDK (toolchain)
- `west` (Zephyr's meta-tool)
- nRF Connect for Desktop (flashing) or `nrfutil`
- Python 3.10+ (for test tools)

### Build

```sh
# 1. Source the NCS v3.3.0 environment
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh

# 2. Set the module path (REQUIRED — enables Kconfig to discover this repo's
#    board/drivers/lib. Must be an env var, not -D, because Kconfig module
#    discovery runs before CMake.)
export ZEPHYR_EXTRA_MODULES=$(pwd)

# 3. Build the clip app (sysbuild: mcuboot + app + network-core radio)
west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip
```

**Production (low-power, console off):**
```sh
west build --build-dir build-clip-prod --board clip/nrf5340/cpuapp applications/clip \
  -- -DSNIPPET_ROOT=$(pwd)/applications/clip -DSNIPPET=production
```

> **Board identifier**: `clip/nrf5340/cpuapp` (NOT `respeaker/...`)

### Flash

```sh
# west flash handles the dual-core routing (app + net core)
west flash --build-dir build-clip && nrfutil device reset
```

> `west flash --reset` does NOT work on this board — use `nrfutil device reset`
> after flashing.

### Serial Console

```sh
minicom -D /dev/ttyACM0 -b 921600
```

## Project Structure

| Path | Description |
|------|-------------|
| `applications/clip/` | Main application (AT commands, audio, BLE, WiFi, storage) |
| `boards/seeed/clip/` | Board support package (device trees, Kconfig) |
| `drivers/` | Custom drivers (GPIO button) |
| `lib/` | Libraries (Opus, SpeexDSP, Lua) |
| `samples/` | Example apps (hello_world, opus_encode, wifi_ap_iperf, etc.) |
| `tests/` | Factory/RF test firmware (`clip`, `otp`, `dtm`, `wifi_radio`, `re`, ...) |
| `patches/mcuboot/` | MCUboot customization patches (applied to the NCS tree) |
| `docs/` | Project documentation |

## Documentation

### Official References

- **[nRF Connect SDK](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/index.html)** — NCS documentation (this firmware targets NCS v3.3.0)
- **[Zephyr Project](https://docs.zephyrproject.org/)** — Zephyr RTOS documentation
- **[nRF5340 Product Page](https://www.nordicsemi.com/Products/nRF5340)** — MCU datasheet & specs
- **[nRF7002](https://www.nordicsemi.com/Products/nRF7002)** — WiFi chipset
- **[NPM1300](https://www.nordicsemi.com/Products/npm1300)** — PMIC / battery charger

### Project Docs (`docs/`)

| Doc | Description |
|-----|-------------|
| [architecture.md](docs/architecture.md) | System architecture & design |
| [protocol.md](docs/protocol.md) | BLE AT command protocol specification |
| [udp_protocol.md](docs/udp_protocol.md) | WiFi UDP file transfer protocol |
| [requirements.md](docs/requirements.md) | Product requirements |
| [mcuboot_app_development.md](docs/mcuboot_app_development.md) | Building apps under the custom MCUboot (OTA guide) |
| [usb_dfu.md](docs/usb_dfu.md) | Firmware upgrade guide (USB / BLE / programmer) |
| [audio_quality_standard.md](docs/audio_quality_standard.md) | Audio recording quality standard |
| [development.md](docs/development.md) | Development log |
| [whitepaper.md](docs/whitepaper.md) | Firmware whitepaper |

See [CLAUDE.md](CLAUDE.md) for detailed build/flash/power-management guidance
and known pitfalls.

## Testing

```sh
# BLE protocol tests
python tests/ble_test.py --interactive

# WiFi UDP file sync (connect to ClipAP_XXXX first; password 12345678 by default,
# becomes a random one after the first BLE pairing)
python applications/clip/tests/tools/udp_sync.py --session <session_id>

# Hardware test firmware
west build --build-dir build-test --board clip/nrf5340/cpuapp --pristine tests/clip
```

WiFi AP: SSID `ClipAP_XXXX` (last 4 hex of chip ID) · Password `12345678` (default; random after first pairing) · IP `192.168.4.1` · UDP Port `8089`

## License

Apache License 2.0. See individual files for details. Third-party libraries
(Opus, SpeexDSP, Lua) retain their respective licenses.

## Acknowledgements

- [Nordic Semiconductor](https://www.nordicsemi.com/) — nRF Connect SDK, nRF5340, nRF7002, NPM1300
- [Zephyr Project](https://zephyrproject.org/) — RTOS
- [Seeed Studio](https://www.seeedstudio.com/) — reSpeaker Clip hardware
