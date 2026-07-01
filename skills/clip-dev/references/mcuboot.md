# MCUboot Reference

## Overview

Customized MCUboot bootloader providing secure boot, dual-core OTA, OLED display, and serial DFU recovery.

- **Secure boot**: RSA-2048 signed image verification
- **Dual-core OTA**: Simultaneous app-core + net-core updates
- **OLED display**: Boot animation + OTA progress visualization
- **Serial DFU**: USB CDC ACM recovery mode
- **External flash OTA**: Secondary partition on 64MB SPI flash (PY25Q64H)

Source: `docs/mcuboot_app_development.md`, `docs/architecture.md`.

## Boot Flow

```
Power On → MCUboot (88KB @ 0x0)
  ├─ Verify slot0 signature (RSA-2048)
  ├─ Display boot animation on OLED
  └→ Application (slot0 @ 0x00016000)
```

If slot0 signature fails → DFU recovery mode (USB CDC serial).

## Flash Partition Layout

**Internal Flash (1MB):**

| Region | Size | Offset |
|--------|------|--------|
| MCUboot | 88KB | 0x00000000 |
| Image-0 Secure (primary app) | 264KB | 0x00016000 |
| Image-0 Non-Secure (net core) | 192KB | 0x00058000 |
| Image-1 Secure (OTA staging) | 256KB | 0x00088000 |
| Image-1 Non-Secure (OTA staging) | 192KB | 0x000C8000 |

**External SPI Flash (64MB, SPI3):**

| Region | Size | Offset |
|--------|------|--------|
| Image-0 Secondary (OTA staging) | ~960KB | 0x00000000 |
| Image-1 Secondary (netcore OTA) | ~256KB | 0x000F0000 |
| LittleFS | ~6.8MB | 0x00130000 |

OTA flow: new firmware written to external flash → MCUboot copies to internal slot during next boot.

## pm_static.yml (REQUIRED)

Every MCUboot app MUST symlink the static partition layout:

```sh
cd samples/my_app   # or applications/clip
ln -s ../_mcuboot/pm_static_clip_nrf5340_cpuapp.yml pm_static_clip_nrf5340_cpuapp.yml
```

**Without it**, the Partition Manager dynamically calculates smaller partitions and the build fails with `region FLASH overflowed`.

The `pm_static_clip_nrf5340_cpuapp.yml` defines the 88KB MCUboot / 936KB app / external flash OTA slots / LittleFS layout.

## Signing Key

**Location:** `samples/_mcuboot/sysbuild/root-rsa-2048.pem`
(app copy: `applications/clip/sysbuild/root-rsa-2048.pem`)

Referenced in `sysbuild.conf`:
```
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="${APPLICATION_CONFIG_DIR}/../_mcuboot/sysbuild/root-rsa-2048.pem"
```

**Warning:** Do NOT use `bootloader/root-rsa-2048.pem` — it is a different key. If you change the key, you must reflash MCUboot (`west flash --recover`).

## Sample Tier Configuration

Samples are organized into 3 wireless tiers. The `sysbuild.conf` selects the tier.

| Tier | Features | Sample Examples |
|------|----------|-----------------|
| **Tier 1** (Basic) | MCUboot only, no wireless | hello_world, button_demo, lua_repl, battery_170, t5838 |
| **Tier 2** (BLE) | + BLE network core | opus_encode, lc3_encode, suspend_to_ram |
| **Tier 3** (WiFi) | + BLE + WiFi (nRF7002) | http_server, wifi_ap_iperf, wifi_ble_coex |

To create a new app: copy a sample, set up the required symlinks, and copy `sysbuild.conf` from the desired tier.

### Required Files for a New App

```
my_app/
├── CMakeLists.txt
├── prj.conf
├── src/main.c
├── pm_static_clip_nrf5340_cpuapp.yml   ← symlink (REQUIRED)
├── sysbuild.conf                        ← tier config
└── sysbuild/                            ← symlinks to shared MCUboot config
    ├── mcuboot.conf
    ├── mcuboot.overlay
    └── ipc_radio/prj.conf
```

## Shared MCUboot Config (`samples/_mcuboot/`)

| File | Purpose |
|------|---------|
| `pm_static_clip_nrf5340_cpuapp.yml` | Fixed flash partition layout |
| `sysbuild/mcuboot.conf` | MCUboot Kconfig (OLED, USB CDC, SD erase, PMIC) |
| `sysbuild/mcuboot.overlay` | Device tree overlay (OLED, PMIC, SD on; WiFi, PDM, ADC off) |
| `sysbuild/ipc_radio/prj.conf` | Network core BLE controller config |
| `sysbuild/root-rsa-2048.pem` | RSA-2048 signing key |

## OTA Update Methods

### BLE OTA (mcumgr)

```sh
mcumgr image upload build-clip/dfu_application.zip \
    --conntype ble --connstring peer_name="reSpeaker Clip"
```

### USB Serial DFU (recovery mode)

1. Hold user button while connecting USB → device appears as serial
2. `mcumgr image upload build-clip/dfu_application.zip --conntype serial --connstring /dev/ttyACM0`

### First-Time Flash (MCUboot + App)

```sh
west flash --build-dir build-clip --recover && nrfutil device reset
```

## MCUboot Patch Development

MCUboot source: `~/ncs/v3.2.1/bootloader/mcuboot`. Patches stored in `patches/mcuboot/`.

Workflow: **modify source → pristine build → verify → export patch** (see CLAUDE.md "MCUboot Patch Development" for full `git diff`/`git apply` workflow). Must build pristine for any mcuboot change. OLED display changes touch `boot/zephyr/io_display.c` and `boot/zephyr/main.c`.
