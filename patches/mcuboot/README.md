# MCUboot Patches

This directory contains patches to be applied to the MCUboot source tree in NCS.
The patches are against **NCS v3.2.1** (`~/ncs/v3.2.1/bootloader/mcuboot`).

## Applying Patches (Required After Fresh NCS Install)

```sh
cd ~/ncs/v3.2.1/bootloader/mcuboot
git apply /path/to/ReSpeaker_Clip/patches/mcuboot/0001-require-vbus-for-gpio-serial-recovery.patch
```

To verify a patch is already applied:
```sh
cd ~/ncs/v3.2.1/bootloader/mcuboot
git diff boot/zephyr/io.c
```

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

```c
#if defined(CONFIG_SOC_NRF5340_CPUAPP)
    if (pin_active) {
        if (!(NRF_USBREGULATOR_S->USBREGSTATUS & USBREG_USBREGSTATUS_VBUSDETECT_Msk)) {
            printk("[MCUboot] Button pressed but USB not connected, skipping serial recovery\n");
            pin_active = 0;
        }
    }
#endif
```

**Peripheral**: `NRF_USBREGULATOR_S` (base `0x50037000`), `USBREGSTATUS.VBUSDETECT` bit 0.  
This register is readable without initializing the USB stack — it reflects hardware state.

### DFU Entry Methods (all still work)

| Method | Trigger | VBUS required? |
|--------|---------|----------------|
| **Button + USB** | Hold button at power-on/reset with USB connected | Yes |
| **AT+DFU** | App BLE command → `bootmode_set` + reboot | No (app already running) |
| **`BOOT_SERIAL_BOOT_MODE`** | App sets retention register + reboot | No |

### Tested On

- NCS v3.2.1, MCUboot commit `3cdbf4df`
- Board: `clip/nrf5340/cpuapp` (Seeed ReSpeaker Clip)
