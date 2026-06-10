# Hardware Reference

## Board Support Package

- **Board identifier**: `clip/nrf5340/cpuapp`
- **MCU**: Nordic nRF5340 dual-core (Application core + Network core)
- **Device tree**: `boards/seeed/clip/clip_nrf5340_cpuapp.dts`
- **Board dir**: `boards/seeed/clip/`

Source: `docs/architecture.md` §10, `CLAUDE.md`.

## Key Peripherals & Pin Mapping

### GPIO

| Pin | Function | Direction | Notes |
|-----|----------|-----------|-------|
| GPIO1.15 | User button | Input, pull-up | Active-low, custom input driver |
| GPIO1.14 | Mic power enable | Output | High = on, gated during pause |
| GPIO1.8 | OLED power enable | Output | High = on |
| GPIO1.9 | OLED reset | Output | Active-low reset |
| GPIO0.29 | WiFi RF switch power | Output | High = on |
| GPIO1.6 | Haptic motor (PMIC GPIO1) | Output | Controls BUCK1 (MOTOR_3V3) |

### I2C Devices

| Bus | Address | Device | Purpose |
|-----|---------|--------|---------|
| I2C1 | 0x6b | NPM1300 PMIC | Battery, 5 GPIOs, regulators |
| I2C2 | 0x3c | CH1115 OLED | 88x48 display |

### SPI Devices

| Bus | Device | CS Pin | Purpose |
|-----|--------|--------|---------|
| SPI3 | PY25Q64H flash | GPIO0.20 | 64MB external flash |
| SPI4 | SD card (SDHC-SPI) | GPIO0.9 | FAT32 storage |

### Other Buses

| Bus | Device | Purpose |
|-----|--------|---------|
| PDM0 | Microphone array | 2-channel PDM input (alias: dmic0) |
| QSPI | nRF7002 WiFi | WiFi module |

## PMIC Regulator Assignments (NPM1300)

| Regulator | Output | Purpose | Control |
|-----------|--------|---------|---------|
| BUCK1 | MOTOR_3V3 | Vibration motor | PMIC GPIO2 |
| BUCK2 | VDD_3V3 | Main system power | Always-on |
| LDO1 | VDDMIC_1V8 | Microphone power | Always-on |
| LDO2 | VDD_SD | SD card power | Always-on |

## External Flash Partitions (PY25Q64H, 64MB)

| Partition | Size | Purpose |
|-----------|------|---------|
| Image-0 Secondary | ~960KB | OTA staging area |
| Image-1 Secondary | ~256KB | Netcore OTA staging |
| LittleFS | ~6.8MB | Settings, OTA patches |

## Internal Flash (1MB)

| Region | Size | Offset |
|--------|------|--------|
| MCUboot | 88KB | 0x00000000 |
| Image-0 Secure | 264KB | 0x00016000 |
| Image-0 Non-Secure | 192KB | 0x00058000 |
| Image-1 Secure (OTA) | 256KB | 0x00088000 |
| Image-1 Non-Secure (OTA) | 192KB | 0x000C8000 |

## Crystal Capacitance Tuning

Board has no external load capacitors for LFXO/HFXO. Internal capacitors must be configured.

### Test Firmware Shell Commands (tests/clip)

```
lfxo get                  — Read 32.768kHz crystal capacitance
lfxo set <0-3>            — 0=external, 1=6pF, 2=7pF, 3=9pF
hfxo get                  — Read 32MHz crystal capacitance
hfxo set <pF>             — Set in pF (7.0-20.0, step 0.5, 0=external)
```

### Device Tree Configuration (after tuning)

```dts
&lfxo {
    load-capacitors = "internal";
    load-capacitance-picofarad = <7>;
};
&hfxo {
    load-capacitors = "internal";
    load-capacitance-picofarad = <9>;
};
```

## nRF70 OTP Programming (Factory Tool)

Burns factory MAC address into nRF7002 WiFi module.

```sh
# Build OTP tool
west build --build-dir build-otp --board clip/nrf5340/cpuapp --pristine tests/otp
west flash --build-dir build-otp && nrfutil device reset

# Shell commands
nrf70 otp status          — Check OTP status
nrf70 otp read            — Read OTP data
nrf70 otp write_mac0 <addr>  — Write MAC address 0
nrf70 otp write_mac1 <addr>  — Write MAC address 1
nrf70 otp lock            — Lock OTP (irreversible!)
```

See `tests/otp/README.md` for full usage.

## Hardware Test Suite

```sh
# Multi-image hardware test (sysbuild)
west build --build-dir build-test --board clip/nrf5340/cpuapp --pristine tests/clip
west flash --build-dir build-test && nrfutil device reset
```

## Button Behavior

| Action | Trigger | Behavior |
|--------|---------|----------|
| Single click | Press & release (< 1s) | Bookmark (recording) or status bar (idle) |
| Long press | Hold > 1s | Start/stop recording + vibrate |
| Long press Level 1+ | Hold > 2s | Power-off screen (execute on release) |
| Double click | Two quick presses | Reserved (no action) |

Charging: power-off is blocked. Long press levels are ignored.

## WiFi Module (nRF7002)

| Parameter | Value |
|-----------|-------|
| Mode | AP only |
| Band | 5GHz channel 36 |
| Regulatory domain | US |
| SSID prefix | ClipAP_ |
| Max clients | 1 |
| Coexistence | `CONFIG_NRF70_SR_COEX=y` with PTA |

## Key Source Files

- `boards/seeed/clip/clip_nrf5340_cpuapp.dts` — device tree (all pin assignments)
- `boards/seeed/clip/Kconfig*` — board Kconfig
- `drivers/input/` — custom GPIO button driver
- `tests/clip/` — hardware test suite + crystal tuning
- `tests/otp/` — nRF70 OTP programming tool
