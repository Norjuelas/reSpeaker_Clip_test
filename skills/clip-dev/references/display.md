# Display Reference

## CH1115 OLED Specifications

| Parameter | Value |
|-----------|-------|
| Controller | CH1115 |
| Resolution | 88 x 48 pixels |
| Interface | I2C (I2C2 bus) |
| Address | 0x3c |
| Reset Pin | GPIO1.9 |
| Power Enable | GPIO1.8 |
| Icon size | 24x24 pixels (XBM format) |
| Font size | 8x16 pixels |
| Brightness range | 0-255 (default 128) |

Source: `applications/clip/src/display.c`, `applications/clip/src/icons.c`, `docs/architecture.md` §3.14.

## Device Tree

```dts
&i2c2 {
    ch1115: ch1115@3c {
        compatible = "custom,ch1115";
        reg = <0x3c>;
        width = <88>;
        height = <48>;
        reset-gpios = <&gpio1 9 GPIO_ACTIVE_LOW>;
    };
};
```

Board DTS: `boards/seeed/clip/clip_nrf5340_cpuapp.dts`

## UI State Machine

```c
enum ui_state {
    UI_STATE_OFF,              // Display off
    UI_STATE_PAIRING_GUIDE,    // BLE not bonded
    UI_STATE_STATUS_BAR,       // Battery, connection, mode icons
    UI_STATE_REC_WAVE,         // Recording (enhanced mode wave animation)
    UI_STATE_REC_DOT,          // Recording (normal mode dot animation)
    UI_STATE_MARKING,          // Bookmark flash
    UI_STATE_PAUSED,           // Paused recording
    UI_STATE_POWER_OFF,        // Power-off confirmation
    UI_STATE_USB_CONNECTED,    // USB plugged in
    UI_STATE_OTA,              // OTA in progress
    UI_STATE_LOW_BATTERY,      // Low battery (<10%) fullscreen
};
```

## Animations

| State | Animation | Details |
|-------|-----------|---------|
| REC_WAVE | Wave histogram | 13-bar energy histogram from real-time audio data (enhanced mode) |
| REC_DOT | Dot pulse | Slower, simpler pulse animation (normal mode) |
| MARKING | Flash | Brief flash when bookmark is added |
| STATUS_BAR | Timed | Auto-timeout after 3 seconds |
| LOW_BATTERY | Fullscreen | Warning when battery < 10% |

## Status Bar Icons (24x24 XBM)

| Icon | Variants |
|------|----------|
| Battery | 0%, 25%, 50%, 75%, 100% + charging indicator |
| BLE | Connected indicator |
| WiFi | AP active + client connected |
| Mode | Normal / Enhanced indicator |
| OTA | Update in progress |

Icons source: `applications/clip/src/icons.c` (XBM format bitmaps).

## MCUboot Boot Animation

MCUboot also uses the CH1115 OLED. The boot animation is implemented in:
- `~/ncs/v3.2.1/bootloader/mcuboot/boot/zephyr/io_display.c` (patched)

Patches: `patches/mcuboot/` — see CLAUDE.md "MCUboot Patch Development" section.

MCUboot overlay enables OLED and PMIC, disables WiFi, PDM, ADC to minimize boot resources.

## Key Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `CLIP_DEFAULT_BRIGHTNESS` | 128 | Default OLED brightness (0-255) |

Brightness is stored persistently via `AT+BRIGHTNESS=<0-255>` → saved to NVS → applied on every boot.

## Key Functions

| Function | Purpose |
|----------|---------|
| `display_init()` | Initialize CH1115, set initial UI state |
| `display_post_event()` | Handle UI state transitions from event dispatcher |
| `display_set_brightness()` | Set OLED contrast (0-255) |

## Key Source Files

- `applications/clip/src/display.c` — display controller, UI state machine
- `applications/clip/src/icons.c` — XBM icon bitmaps
- `applications/clip/include/display.h` — public API
