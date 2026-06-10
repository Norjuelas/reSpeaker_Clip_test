# MCUboot Application Development for ReSpeaker Clip

This guide explains how to build Zephyr RTOS applications that run under the ReSpeaker Clip's custom MCUboot bootloader, including OTA (Over-The-Air) firmware updates.

## Overview

The ReSpeaker Clip uses a customized MCUboot bootloader that provides:

- **Secure boot**: RSA-2048 signed image verification
- **Dual-core OTA**: Simultaneous updates for Application core and Network core
- **OLED display**: Boot animation and OTA progress visualization
- **Serial DFU**: USB CDC ACM firmware recovery mode
- **External flash OTA**: Secondary partition on 64MB SPI flash (PY25Q64H)

### Boot Flow

```
Power On
  │
  ▼
MCUboot (88KB @ 0x00000000)
  │
  ├─ Verify signature of slot0 image (RSA-2048)
  ├─ Display boot animation on OLED
  │
  ▼
Application (slot0 @ 0x00016000)
```

If the image in slot0 fails signature verification, MCUboot enters DFU recovery mode (USB CDC serial).

### Flash Partition Layout

```
Internal Flash (1MB)
┌─────────────────────────────────┐ 0x00000000
│  MCUboot (88KB)                 │
├─────────────────────────────────┤ 0x00016000
│  Image-0 Secure (264KB)         │  ← Primary app slot
├─────────────────────────────────┤ 0x00058000
│  Image-0 Non-Secure (192KB)     │
├─────────────────────────────────┤ 0x00088000
│  Image-1 Secure (256KB)         │  ← Secondary slot (OTA)
├─────────────────────────────────┤ 0x000C8000
│  Image-1 Non-Secure (192KB)     │
└─────────────────────────────────┘ 0x00100000

External SPI Flash (64MB, SPI3)
┌─────────────────────────────────┐ 0x00000000
│  Image-0 Secondary (~960KB)     │  ← OTA staging area
├─────────────────────────────────┤ 0x000F0000
│  Image-1 Secondary (~256KB)     │  ← Netcore OTA staging
├─────────────────────────────────┤ 0x00130000
│  LittleFS (~6.8MB)              │
└─────────────────────────────────┘
```

> **Note**: The external flash is used as the OTA staging area. When performing an OTA update, the new firmware is written to the external flash first, then MCUboot copies it to the internal flash slot during boot.

## Prerequisites

```bash
# Set up Zephyr environment
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh

# Set project modules (required for custom drivers and libraries)
export ZEPHYR_EXTRA_MODULES=$(pwd)
```

## Building Samples

### Quick Start

All samples in `samples/` come with MCUboot support pre-configured. The default build produces a signed, OTA-ready firmware.

```bash
# Build sample with MCUboot (signed image, includes bootloader + app)
west build --build-dir build-hello --pristine \
    --board clip/nrf5340/cpuapp samples/hello_world
```

### Sample Tiers

Samples are organized into three tiers based on their wireless requirements. The `sysbuild.conf` in each sample automatically selects the correct configuration.

| Tier | Features | Samples |
|------|----------|---------|
| **Tier 1** (Basic) | MCUboot only, no wireless | hello_world, button_demo, lua_repl, battery_170, t5838 |
| **Tier 2** (BLE) | MCUboot + BLE network core | opus_encode, lc3_encode, suspend_to_ram |
| **Tier 3** (WiFi) | MCUboot + BLE + WiFi (nRF7002) | http_server, wifi_ap_iperf, wifi_ble_coex |

### Build Output

When building with MCUboot, the following output files are generated:

| File | Description |
|------|-------------|
| `build-<name>/merged.hex` | MCUboot + Application core firmware (combined) |
| `build-<name>/merged_CPUNET.hex` | Network core firmware |
| `build-<name>/dfu_application.zip` | OTA update package (for BLE or USB DFU) |

When building without MCUboot (`-DSB_CONFIG_BOOTLOADER_MCUBOOT=n`):

| File | Description |
|------|-------------|
| `build-<name>/zephyr/zephyr.hex` | Application firmware only (standalone) |

## Flashing

### First-Time Flash (MCUboot + App)

For a device that has never been programmed, or when updating MCUboot itself:

```bash
# Flash both MCUboot and application (erases everything)
west flash --build-dir build-hello --recover && nrfutil device reset
```

> **Warning**: `--recover` erases ALL flash memory on both cores, including any existing data.

### Subsequent Flash (App Only via DFU)

Once MCUboot is on the device, use OTA to update just the application:

**BLE OTA:**
```bash
# Use the dfu_application.zip with nRF Connect or mcumgr
mcumgr image upload build-hello/dfu_application.zip \
    --conntype ble --connstring peer_name="ReSpeaker Clip"
```

**USB Serial DFU (recovery mode):**
1. Hold the user button while connecting USB
2. The device appears as a USB serial device
3. Use mcumgr to upload:
```bash
mcumgr image upload build-hello/dfu_application.zip \
    --conntype serial --connstring /dev/ttyACM0
```

## Creating Your Own Application

### From a Sample Template

The easiest way to start is to copy an existing sample:

```bash
# Copy hello_world as a starting point
cp -r samples/hello_world samples/my_app
```

The sample already includes:
- `sysbuild.conf` — MCUboot build settings
- `sysbuild/` — Symlinks to shared MCUboot configuration

### Choosing the Right Tier

When creating a new application, pick the correct `sysbuild.conf` for your needs:

**Tier 1 — No wireless (simplest)**:
```
samples/hello_world/sysbuild.conf
samples/button_demo/sysbuild.conf
```

**Tier 2 — BLE required**:
```
samples/opus_encode/sysbuild.conf
samples/lc3_encode/sysbuild.conf
```

**Tier 3 — WiFi + BLE required**:
```
samples/http_server/sysbuild.conf
```

To switch tiers, copy the `sysbuild.conf` from a sample in the desired tier.

### What Each Tier Provides

**Tier 1 (Basic)**:
- MCUboot bootloader with signed images
- External flash OTA staging
- Dual-core image support (netcore firmware is included but minimal)

**Tier 2 (BLE)** adds:
- Network core IPC radio (BLE controller)
- HCI IPC transport for BLE stack
- Secure boot for network core

**Tier 3 (WiFi)** adds:
- nRF7002 WiFi driver
- WiFi/BLE coexistence via MPSL

### Required Files

Every MCUboot-compatible application must have:

```
my_app/
├── CMakeLists.txt
├── prj.conf
├── src/
│   └── main.c
├── pm_static_clip_nrf5340_cpuapp.yml  ← Symlink to shared partition layout
├── sysbuild.conf                        ← MCUboot settings (pick the right tier)
└── sysbuild/                            ← Symlinks to shared MCUboot child config
    ├── mcuboot.conf    → ../../_mcuboot/sysbuild/mcuboot.conf
    ├── mcuboot.overlay → ../../_mcuboot/sysbuild/mcuboot.overlay
    └── ipc_radio/
        └── prj.conf    → ../../../_mcuboot/sysbuild/ipc_radio/prj.conf
```

> **Important**: The `pm_static_clip_nrf5340_cpuapp.yml` file is **required**. It tells the Partition Manager to use the fixed flash layout (88KB MCUboot, 936KB app, external flash OTA slots, LittleFS partition). Without it, the Partition Manager dynamically calculates smaller partitions and the build will fail with a FLASH overflow error.

To set up the files for a new app:

```bash
cd samples/my_app

# Static partition layout (required!)
ln -s ../_mcuboot/pm_static_clip_nrf5340_cpuapp.yml pm_static_clip_nrf5340_cpuapp.yml

# MCUboot child image config
mkdir -p sysbuild/ipc_radio
ln -s ../../_mcuboot/sysbuild/mcuboot.conf sysbuild/mcuboot.conf
ln -s ../../_mcuboot/sysbuild/mcuboot.overlay sysbuild/mcuboot.overlay
ln -s ../../../_mcuboot/sysbuild/ipc_radio/prj.conf sysbuild/ipc_radio/prj.conf
```

Then copy the `sysbuild.conf` from a sample matching your tier.

### Signing Key

The signing key is located at `samples/_mcuboot/sysbuild/root-rsa-2048.pem`. This key must match the one baked into the MCUboot bootloader on the device. If you change the key, you must reflash MCUboot.

The `sysbuild.conf` references the key via:
```
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="${APPLICATION_CONFIG_DIR}/../_mcuboot/sysbuild/root-rsa-2048.pem"
```

> **Important**: Do NOT use the key in `bootloader/root-rsa-2048.pem` — it is a different key. Always use the one in `samples/_mcuboot/sysbuild/` or `applications/clip/sysbuild/`.

## Shared MCUboot Configuration

The `samples/_mcuboot/` directory contains the shared configuration that all samples reference via symlinks:

| File | Purpose |
|------|---------|
| `pm_static_clip_nrf5340_cpuapp.yml` | Fixed flash partition layout (88KB MCUboot, 936KB app, external flash OTA, LittleFS) |
| `sysbuild/mcuboot.conf` | MCUboot Kconfig settings (OLED, USB CDC, SD card erase, PMIC) |
| `sysbuild/mcuboot.overlay` | Device tree overlay (enables OLED, PMIC, SD card; disables WiFi, PDM, ADC) |
| `sysbuild/ipc_radio/prj.conf` | Network core BLE controller configuration |
| `sysbuild/root-rsa-2048.pem` | RSA-2048 signing key (production) |

These files are copied from `applications/clip/` and must be kept in sync if the main app's MCUboot configuration changes.

## Troubleshooting

### "Signature verification failed" on boot

The signing key in your build does not match the key in the MCUboot on the device. Solutions:
1. Make sure `sysbuild.conf` references `samples/_mcuboot/sysbuild/root-rsa-2048.pem`
2. Reflash MCUboot + app together: `west flash --recover`

### MCUboot FLASH overflow ("region FLASH overflowed by N bytes")

The `pm_static_clip_nrf5340_cpuapp.yml` file is missing from the sample directory. Without it, the Partition Manager dynamically allocates a smaller MCUboot partition (48KB instead of 88KB), causing the overflow.

Fix: ensure the symlink exists:
```bash
ls -la samples/my_app/pm_static_clip_nrf5340_cpuapp.yml
# Should point to ../_mcuboot/pm_static_clip_nrf5340_cpuapp.yml
```

### Boot loop (MCUboot keeps restarting)

The application image is too large for the slot. Check the build output:
- Secure slot (image-0): 264KB max
- Non-secure slot (image-0-ns): 192KB max

Reduce features or enable size optimizations:
```
CONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE=y
CONFIG_LTO=y
```

### "Network core access port is protected" when flashing

The device has readback protection enabled. Use `--recover`:
```bash
west flash --build-dir build-hello --recover && nrfutil device reset
```

### Build fails with "unknown SB_CONFIG_*" variable

Make sure the Zephyr environment is sourced:
```bash
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
```

### MCUboot does not find the signing key

The relative path `${APPLICATION_CONFIG_DIR}/../_mcuboot/sysbuild/root-rsa-2048.pem` must resolve correctly. Verify:
```bash
# From your sample directory, check the key exists
ls ../_mcuboot/sysbuild/root-rsa-2048.pem
```
