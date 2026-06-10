# Power Management Reference

## PM Device Runtime

`CONFIG_PM_DEVICE_RUNTIME=y` enables automatic peripheral power management. UART, I2C, SPI drivers automatically suspend when idle and resume on access.

- No separate production snippet needed for power savings
- Debug console is retained without power penalty (UART auto-suspends between log outputs)

Additional power config:
```
CONFIG_PM=y
CONFIG_PM_DEVICE=y
CONFIG_PM_DEVICE_RUNTIME=y
CONFIG_TICKLESS_KERNEL=y
```

## CPU Frequency Scaling (Reference Counted)

```c
void clip_cpu_boost_acquire(void);  // 128MHz (recording)
void clip_cpu_boost_release(void);  // 64MHz (idle)
```

Uses `atomic_t` reference counter. Multiple subsystems can request boost; frequency drops to 64MHz only when count reaches 0.

## NPM1300 PMIC Regulators

PMIC on I2C1 at address 0x6b. Provides battery management + 5 GPIOs + 4 regulators.

| Regulator | Output | Purpose | Control |
|-----------|--------|---------|---------|
| BUCK1 | MOTOR_3V3 | Vibration motor | PMIC GPIO2 |
| BUCK2 | VDD_3V3 | Main system power | Always-on |
| LDO1 | VDDMIC_1V8 | Microphone power | Always-on |
| LDO2 | VDD_SD | SD card power | Always-on |

## GPIO-Controlled Power

| GPIO | Function | Active |
|------|----------|--------|
| GPIO1.14 | Microphone power enable | High = on |
| GPIO1.8 | OLED display power enable | High = on |
| GPIO0.29 | WiFi RF switch power | High = on |

Microphone power is gated: powered off during pause, powered on during resume.

## WiFi Power Management

- `CONFIG_NRF70_QSPI_LOW_POWER=y` — puts QSPI in low power when WiFi is not in use
- WiFi radio NOT started at boot (`CONFIG_NRF_WIFI_IF_AUTO_START=n`)
- WiFi auto-off after 3 minutes if no client (`CONFIG_CLIP_WIFI_TIMEOUT_MS=180000`)
- Manual start/stop via `AT+WIFI=on|off`
- ~30mA saved when WiFi is off

## BLE Power Impact

- BLE slow advertising (1s interval) adds ~0.1mA averaged to idle current
- BLE bonding: single paired device, LE Secure Connections

## PMIC Ship Mode

`AT+POWEROFF` or long-press power-off sequence enters ship mode:
```c
regulator_parent_ship_mode(...);
```
Ultra-low power state. Requires physical button press to wake. All unsaved data is preserved (session files already flushed to SD).

## Battery Monitoring

- NPM1300 PMIC with nRF Fuel Gauge (`CONFIG_NRF_FUEL_GAUGE=y`)
- `CONFIG_NRF_FUEL_GAUGE_VARIANT_SECONDARY_CELL=y` for accurate SoC
- SoC smoothed over time to avoid sudden jumps
- Battery level (0-100%) reported via `AT+GSTAT` and OLED status bar
- Low battery warning event at <10%
- Charging status tracked

## Production Snippet

The `production` snippet (`applications/clip/snippets/production/`) was historically used to disable debug UART. With `PM_DEVICE_RUNTIME`, it is no longer necessary for power savings since UART auto-suspends between log outputs.

```sh
# Build with production snippet (legacy)
west build ... -- -DSNIPPET_ROOT=applications/clip -DSNIPPET=production
```

## Key Kconfig Options

| Option | Description |
|--------|-------------|
| `CONFIG_PM` | Enable power management |
| `CONFIG_PM_DEVICE` | Enable device power management |
| `CONFIG_PM_DEVICE_RUNTIME` | Automatic peripheral PM (auto suspend/resume) |
| `CONFIG_TICKLESS_KERNEL` | Tickless idle for lower power |
| `CONFIG_NRF70_QSPI_LOW_POWER` | WiFi QSPI low power when idle |
| `CONFIG_NRF_FUEL_GAUGE` | Battery fuel gauge |
| `CONFIG_CLIP_WIFI_TIMEOUT_MS` | WiFi AP auto-off timeout (0=disabled) |

## Key Source Files

- `applications/clip/src/battery.c` — battery monitoring + fuel gauge
- `applications/clip/src/audio.c` — microphone power gating, CPU boost
- `applications/clip/src/wifi.c` — WiFi on/off, auto-off timer
- `applications/clip/src/button.c` — power-off sequence → ship mode
- `applications/clip/src/haptic.c` — motor control via PMIC GPIO
