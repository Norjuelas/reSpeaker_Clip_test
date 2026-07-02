# Custom App Development Guide for reSpeaker Clip

This guide explains how to build your own Zephyr RTOS applications that run
under the reSpeaker Clip's custom MCUboot bootloader — including how to flash,
upgrade over BLE, and recover from a broken app.

> **Audience**: developers building custom firmware for the reSpeaker Clip
> platform. The official clip app (`applications/clip/`) is one such app;
> yours can be any Zephyr application targeting `clip/nrf5340/cpuapp`.

## Overview

The reSpeaker Clip is a **platform**: it ships with a full-featured voice
recorder app, but you can write your own. Every app runs under the same
custom MCUboot bootloader, which provides:

- **Secure boot**: RSA-2048 signed image verification
- **Dual-core OTA**: simultaneous app-core + network-core updates
- **OLED display**: boot animation, "Recovery Mode", OTA progress bar
- **USB serial DFU**: firmware recovery via USB CDC (always available, even
  if your app is broken — this is your safety net)
- **VBUS gating**: serial recovery only enters when USB is plugged in
  (prevents accidental DFU from a long-press reset on battery)
- **Factory reset commands**: erase SD card / erase settings from recovery

### Firmware Upgrade Paths

| Path | When to use | Requires |
|------|------------|----------|
| **BLE OTA** | Production wireless upgrades | Your app includes mcumgr (see [§5](#5-ble-ota-optional-recommended)) |
| **USB Serial DFU** | App won't boot, BLE is dead, recovery | USB cable + `nrfutil` (see [§6](#6-usb-serial-dfu-the-safety-net)) |
| **J-Link** | First-time flash, MCUboot update | J-Link probe + `west` (advanced, see [§4](#4-first-time-flash-j-link)) |

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

If the image fails signature verification (or no valid image exists), MCUboot
enters **serial recovery mode** — the OLED shows "Recovery Mode" and the
device accepts firmware upload over USB.

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

External SPI Flash (8MB, SPI3)
┌─────────────────────────────────┐ 0x00000000
│  Image-0 Secondary (~960KB)     │  ← OTA staging area
├─────────────────────────────────┤ 0x000F0000
│  Image-1 Secondary (~256KB)     │  ← Netcore OTA staging
├─────────────────────────────────┤ 0x00130000
│  LittleFS (~6.8MB)              │
└─────────────────────────────────┘
```

> During OTA, the new firmware is written to the external flash first, then
> MCUboot copies it to the internal flash slot during the next boot.

## 1. Prerequisites

- [nRF Connect SDK (NCS) v3.3.0](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/index.html)
- Zephyr SDK (toolchain)
- `west` (Zephyr's meta-tool)
- `nrfutil` (for serial DFU — see [§6](#6-usb-serial-dfu-the-safety-net))
- J-Link probe + nRF Connect for Desktop (for first-time flash only)

```bash
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
```

> `ZEPHYR_EXTRA_MODULES` must be an **environment variable** (not `-D`),
> because Kconfig module discovery runs before CMake configuration.

## 2. Create a Custom App

The easiest way to start is to copy an existing sample:

```bash
cp -r samples/hello_world samples/my_app
```

### Required File Structure

```
my_app/
├── CMakeLists.txt
├── prj.conf                        ← Your app's Kconfig
├── src/
│   └── main.c
├── pm_static_clip_nrf5340_cpuapp.yml  ← REQUIRED (symlink)
├── sysbuild.conf                   ← MCUboot settings (pick a tier)
└── sysbuild/                       ← Symlinks to shared MCUboot config
    ├── mcuboot.conf    → ../../_mcuboot/sysbuild/mcuboot.conf
    ├── mcuboot.overlay → ../../_mcuboot/sysbuild/mcuboot.overlay
    └── ipc_radio/
        └── prj.conf    → ../../../_mcuboot/sysbuild/ipc_radio/prj.conf
```

> **`pm_static_clip_nrf5340_cpuapp.yml` is required.** It tells the
> Partition Manager to use the fixed flash layout. Without it, the build
> fails with a FLASH overflow error.

### Sample Tiers

Samples are organized by wireless requirements. Pick the `sysbuild.conf`
from a sample matching your needs:

| Tier | Features | Samples |
|------|----------|---------|
| **1** (Basic) | MCUboot only, no wireless | `hello_world`, `button_demo`, `lua_repl`, `battery_170`, `t5838` |
| **2** (BLE) | MCUboot + BLE network core | `opus_encode`, `lc3_encode`, `suspend_to_ram` |
| **3** (WiFi) | MCUboot + BLE + WiFi (nRF7002) | `http_server`, `wifi_ap_iperf`, `wifi_ble_coex` |

To switch tiers, copy the `sysbuild.conf` from a sample in the desired tier.

### Build

```bash
west build --build-dir build-myapp --pristine \
    --board clip/nrf5340/cpuapp samples/my_app
```

The build produces three images (sysbuild):

| Output file | Description |
|-------------|-------------|
| `build-myapp/merged.hex` | MCUboot + app core + net core (full flash) |
| `build-myapp/merged_CPUNET.hex` | Network core only |
| `build-myapp/dfu_application.zip` | BLE OTA package (app + net core) |
| `build-myapp/<name>/zephyr/zephyr.signed.bin` | Signed app image (for USB serial DFU) |

## 3. Signing Key

All apps **must** be signed with the project's RSA-2048 key:

```
samples/_mcuboot/sysbuild/root-rsa-2048.pem
```

MCUboot verifies the signature at boot — images signed with a different key
are rejected. The `sysbuild.conf` references the key via:

```
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="${APPLICATION_CONFIG_DIR}/../_mcuboot/sysbuild/root-rsa-2048.pem"
```

> **Do NOT lose or change this key.** Devices already in the field have the
> public key baked into MCUboot — they cannot accept images signed with a
> different key without reflashing the bootloader (J-Link).
>
> Do NOT use `bootloader/root-rsa-2048.pem` — it is a different key.

## 4. First-Time Flash (J-Link)

For a brand-new device or when updating MCUboot itself:

```bash
west flash --build-dir build-myapp && nrfutil device reset
```

> `west flash --reset` does NOT work on this board — always use
> `nrfutil device reset` after flashing.

This installs MCUboot + your app + the network core in one shot. Only needed
once — subsequent upgrades are wireless.

## 5. BLE OTA (optional, recommended)

If your app needs wireless upgrade capability, add these Kconfig flags to
your `prj.conf`:

```kconfig
# BLE
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y

# MCUmgr OTA over BLE (NCS sample)
CONFIG_NCS_SAMPLE_MCUMGR_BT_OTA_DFU=y
CONFIG_MCUMGR_GRP_IMG_UPDATABLE_IMAGE_NUMBER=2
CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS=y
CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS=y
CONFIG_MCUMGR_GRP_IMG_UPLOAD_CHECK_HOOK=y
```

Build and upload the `signed.bin` over BLE:

```sh
nrfutil install mcu-manager
nrfutil mcu-manager ble image-upload \
    --firmware build-myapp/my_app/zephyr/zephyr.signed.bin \
    --device <BLE_ADDRESS>
```

> The `dfu_application.zip` can also be used for app + net core combined OTA.

After upload, reset the device (or the app can call `sys_reboot`). MCUboot
verifies the new image's signature and swaps it into the primary slot on
the next boot. The OLED shows an OTA progress bar during the swap.

## 6. USB Serial DFU (the safety net)

**This always works** — even if your custom app won't boot, BLE is dead, or
the device is in a boot loop. MCUboot's serial recovery mode runs from the
bootloader partition, independent of the application.

### When to use

- Your app crashed / boot loops / won't start
- BLE OTA is not available (app doesn't include mcumgr, or BLE is broken)
- You need to flash a different app over USB without opening the device

### How to enter recovery mode

**Hold the user button** while connecting USB. The OLED shows "Recovery Mode"
and the device enumerates as a USB CDC serial port.

> The VBUS gating patch ensures serial recovery only enters when USB power
> is present — a long-press reset on battery does **not** trigger DFU.

### Reflash

```sh
# Install nrfutil mcu-manager (one-time)
nrfutil install mcu-manager

# Upload the signed image
nrfutil mcu-manager serial image-upload \
    --firmware build-myapp/my_app/zephyr/zephyr.signed.bin \
    --serial-port /dev/ttyACM0

# Reset to apply (MCUboot verifies the signature and swaps on next boot)
nrfutil mcu-manager serial reset --serial-port /dev/ttyACM0
```

> On Windows, replace `/dev/ttyACM0` with `COM12` (or the actual COM port).
>
> **nrfutil docs**: <https://docs.nordicsemi.com/bundle/nrfutil/latest/page/guides/device_tools/mcu_manager_serial.html>

### MCUboot recovery features

While in recovery mode, MCUboot also exposes custom mcumgr commands:

| Command | Purpose |
|---------|---------|
| Erase SD card | Destroys the FAT filesystem (factory reset / privacy) |
| Erase settings | Destroys the LittleFS superblock (clears BLE bonds + app settings) |

These are accessible via mcumgr SMP over the same USB CDC serial port.

## 7. Shared MCUboot Configuration

The `samples/_mcuboot/` directory contains shared config that all samples
reference via symlinks:

| File | Purpose |
|------|---------|
| `pm_static_clip_nrf5340_cpuapp.yml` | Fixed flash partition layout |
| `sysbuild/mcuboot.conf` | MCUboot Kconfig (OLED, USB CDC, SD erase, PMIC) |
| `sysbuild/mcuboot.overlay` | Device tree overlay (OLED, PMIC, SD; no WiFi/PDM/ADC) |
| `sysbuild/ipc_radio/prj.conf` | Network core BLE controller config |
| `sysbuild/root-rsa-2048.pem` | RSA-2048 signing key |

## 8. Troubleshooting

### "Signature verification failed" on boot

The signing key in your build doesn't match the key in MCUboot on the device.

1. Ensure `sysbuild.conf` references `samples/_mcuboot/sysbuild/root-rsa-2048.pem`
2. Reflash everything: `west flash --build-dir build-myapp && nrfutil device reset`

### "region FLASH overflowed by N bytes"

The `pm_static_clip_nrf5340_cpuapp.yml` symlink is missing. Without it, the
Partition Manager allocates a smaller MCUboot partition (48KB instead of 88KB).

```bash
ls -la samples/my_app/pm_static_clip_nrf5340_cpuapp.yml
# Should point to ../_mcuboot/pm_static_clip_nrf5340_cpuapp.yml
```

### Boot loop (MCUboot keeps restarting)

The app image is too large for the slot:
- Secure slot (image-0): 264KB max
- Non-secure slot (image-0-ns): 192KB max

Reduce features or enable size optimizations:
```
CONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE=y
CONFIG_LTO=y
```

### "Network core access port is protected"

Readback protection is enabled. Use `--recover`:
```bash
west flash --build-dir build-myapp --recover && nrfutil device reset
```

### Build fails with "unknown SB_CONFIG_*" variable

The Zephyr environment isn't sourced:
```bash
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
```

### Can't enter recovery mode

- **Hold the button BEFORE plugging USB** — not after
- The button must be held through the power-on reset
- USB must be plugged in (VBUS gating — battery-only won't enter DFU)
- If the device is in a deep sleep, unplug USB first, then hold button + replug
