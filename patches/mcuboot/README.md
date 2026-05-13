# MCUboot Patches

This directory contains patches to be applied to the MCUboot source tree in NCS.
The patches are against **NCS v3.2.1** (`~/ncs/v3.2.1/bootloader/mcuboot`).

## Applying Patches (Required After Fresh NCS Install)

```sh
cd ~/ncs/v3.2.1/bootloader/mcuboot
git apply /path/to/ReSpeaker_Clip/patches/mcuboot/0001-require-vbus-for-gpio-serial-recovery.patch
git apply /path/to/ReSpeaker_Clip/patches/mcuboot/0002-add-oled-display-support.patch
git apply /path/to/ReSpeaker_Clip/patches/mcuboot/0003-add-serial-upload-progress-hook.patch
```

To verify a patch is already applied:
```sh
cd ~/ncs/v3.2.1/bootloader/mcuboot
git status
```

## Patch Development Workflow

1. **Modify source directly** in `~/ncs/v3.2.1/bootloader/mcuboot/`
2. **Build**: `west build --build-dir build-clip --pristine --board clip/nrf5340/cpuapp applications/clip`
3. **Test**: Flash or export `dfu_application.zip`
4. **Export patches**: `git diff` from mcuboot tree → save to `patches/mcuboot/`
5. **Verify patches**: `git checkout -- .` then `git apply` each patch in order, rebuild
6. **Update this README**

---

## 0001-require-vbus-for-gpio-serial-recovery.patch

**File**: `boot/zephyr/io.c`
**Function**: `io_detect_pin()`

### Problem

The ReSpeaker Clip board uses a single button (`gpio1.15`) for two purposes:
- **Short press**: user interaction (next track, toggle, etc.)
- **Long press (8s)**: hardware reset via `BOOT_SERIAL_ENTRANCE_GPIO` detect delay

MCUboot's `BOOT_SERIAL_ENTRANCE_GPIO` mode enters serial recovery when `io_detect_pin()`
returns `true` at boot. Without additional protection, **every long-press hardware reset
would enter DFU mode**, even with no USB cable connected and no intent to flash.

### Fix

The patch adds a VBUS detection check immediately after the GPIO debounce logic.
If the button is held at boot but USB is **not** connected, serial recovery is skipped
and the device boots normally.

**Logic**: `enter DFU ⟺ button held AND USB VBUS present`

### DFU Entry Methods (all still work)

| Method | Trigger | VBUS required? |
|--------|---------|----------------|
| **Button + USB** | Hold button at power-on/reset with USB connected | Yes |
| **AT+DFU** | App BLE command → `bootmode_set` + reboot | No |
| **`BOOT_SERIAL_BOOT_MODE`** | App sets retention register + reboot | No |

---

## 0002-add-oled-display-support.patch

**Files**: `boot/zephyr/CMakeLists.txt`, `boot/zephyr/Kconfig`, `boot/zephyr/main.c`, `boot/zephyr/io_display.c` (new), `boot/bootutil/src/loader.c`

### Summary

Adds OLED display support to MCUboot for showing OTA progress, status messages, and error conditions on the CH1115 display (88x48).

### What it adds

- **`io_display.c`**: Self-contained display helper using Zephyr Display API.
  - 6x12 pixel font (95 printable ASCII chars) for status text
  - Native 8x16 pixel font (95 printable ASCII chars) for boot animation
  - 24x24 OTA icon (column-major bitmap)
  - `draw_bitmap()` for column-major rendering
  - `io_display_show()` for two-line centered text
  - `io_display_show_progress()` for OTA icon + progress bar + percentage
  - `io_display_boot_animation()` for "seeed studio" boot animation with haptic feedback
- **`CONFIG_MCUBOOT_DISPLAY`**: New Kconfig option (selects I2C, depends on GPIO)
- **Progress hooks** in `main.c`:
  - `mcuboot_status_change()`: Shows OTA icon + "Updating..." + 0% on swap start
  - `boot_serial_upload_progress_hook()`: Serial recovery upload progress
  - `boot_copy_progress_hook()`: Real-time progress during image copy (`boot_copy_region()`)
- **Weak hook** in `loader.c`: `boot_copy_progress_hook(total, copied)` called after each chunk

### Display states

| Condition | Display |
|-----------|---------|
| Boot | "seeed studio" animation + double vibration |
| Serial recovery (button+USB or boot mode) | "Recovery Mode" |
| OTA image swap | OTA icon + "Updating..." + progress bar + real-time % |
| Serial recovery upload | OTA icon + "Updating..." + progress bar + upload % |
| No bootable image | "Error No Image" |
| Before jumping to app | Display off |

### Requirements

- `CONFIG_DISPLAY=y`, `CONFIG_I2C=y`, `CONFIG_REGULATOR=y` in MCUboot config
- I2C2, CH1115, and `oled_reg` must be enabled in MCUboot device tree overlay

---

## 0003-add-serial-upload-progress-hook.patch

**File**: `boot/boot_serial/src/boot_serial.c`

### Summary

Adds a weak callback `boot_serial_upload_progress_hook(img_index, curr_off, img_size)` in `bs_upload()` that fires after each flash chunk is written during serial recovery uploads, and once when upload completes (`curr_off == img_size`).

### What it adds

- `__weak boot_serial_upload_progress_hook()` default no-op
- Called after `curr_off += img_chunk_len + rem_bytes` (per-chunk progress)
- Called after upload completes (100%)
- Override in `main.c` provides display updates via `io_display_show_progress()`
