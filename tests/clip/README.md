# ReSpeaker Clip Hardware Test Suite

## Overview

This test suite provides comprehensive testing for all hardware components on the ReSpeaker Clip board based on nRF5340.

## Building

```bash
# Set environment
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)

# Build
west build --build-dir build-test --pristine --board clip/nrf5340/cpuapp tests/clip

# Flash and reset
west flash --build-dir build-test && nrfutil device reset
```

## Serial Configuration

- **Baud Rate**: 921600
- **Port**: /dev/ttyACM0 (or appropriate USB-serial port)
- **Connect**: `minicom -D /dev/ttyACM0 -b 921600`

## Test Modules

### 1. BLE Test

**Purpose**: Test Bluetooth Low Energy functionality as peripheral device

**Description**: BLE automatically starts advertising on boot. Connect with a BLE central device to test GATT services and throughput.

**Expected Results**:
- Device advertises as "Clip_Test"
- Supports GATT connections and notifications for throughput testing

### 2. WiFi AP Test

**Purpose**: Test nRF7002 WiFi module in AP (hotspot) mode

**AP Configuration**:
- SSID: `ClipTest_XXXX` (auto-generated from chip ID)
- Password: `12345678`
- Band: 5GHz, Channel 36
- IP: 192.168.4.1
- DHCP pool: 192.168.4.2+

**Commands**:
```bash
wifi on              # Start AP
wifi off             # Stop AP
wifi status          # Show AP status
```

**Quick Test**:
1. Run `wifi on`, note the SSID from serial output
2. Connect phone/PC to `ClipTest_XXXX`, password `12345678`
3. Device should get 192.168.4.x address via DHCP
4. Run `wifi status` to confirm AP is running

---

#### WiFi Throughput Testing (zperf / iperf2)

**Overview**: Device uses zperf for UDP throughput testing, compatible with iperf2.

**Test Type**: UDP upload from device to PC (device sends, PC receives)

**Default Parameters**:
- Server IP: 192.168.4.10
- Port: 5001
- Duration: 10 seconds
- Rate: 100 Mbps (100000 kbps)

**Test Procedure**:

**Step 1: Start iperf2 server on PC (connected to ClipTest AP)**
```bash
iperf -s -u -p 5001 -i 1
```

**Step 2: Run iperf test on device**
```bash
iperf                       # Use defaults (192.168.4.10, 10s, 100Mbps)
iperf 192.168.4.10          # Specify PC IP
iperf 192.168.4.10 30       # 30 second test
iperf 192.168.4.10 10 50000 # 10 second test at 50 Mbps
```

**Command Parameters**:
| Parameter | Description | Range | Default |
|-----------|-------------|-------|---------|
| server_ip | PC IP address | Any valid IP | 192.168.4.10 |
| duration_sec | Test duration | 1-3600 seconds | 10 |
| rate_kbps | Send rate | 100-1000000 kbps | 100000 |


---

### 3. SD Card Test

**Purpose**: Test SD card file system operations

**Commands**:
```bash
sd mount             # Mount SD card
sd umount            # Unmount SD card
sd format            # Format SD card as FAT32
sd speed [size_kb]   # Speed test
sd status            # Show SD card status
fs ls /SD:           # List files
```

**Expected Results**:
- SD card mounts when present
- File listing works correctly
- Eject safely unmounts

### 4. Microphone Test

**Purpose**: Test PDM microphone audio capture and WAV recording

**Commands**:
```bash
mic capture [time_sec]  # Capture audio and print sample stats
mic record [time_sec]   # Record WAV file to SD card (default 3 sec)
```

**Expected Results**:
- Audio capture starts and stops
- Sample statistics (avg/min/max) printed for each block
- WAV file saved to SD card as RECXXXX.WAV

**Typical Workflow**:
1. Insert SD card and mount: `sd mount`
2. Record audio: `mic record 5`
3. Enable USB MSC to access files: `usb msc on`
4. Copy WAV files from USB drive on PC
5. Disable USB MSC: `usb msc off`

### 10. USB Mass Storage Test

**Purpose**: Expose SD card as USB drive for direct file access from PC

**Commands**:
```bash
usb msc on       # Unmount SD, enable USB MSC (SD appears as USB drive)
usb msc off      # Disable USB MSC, remount SD card
usb status       # Show USB and SD card status
```

**Usage**:
1. Record audio to SD: `mic record 5`
2. Enable USB MSC: `usb msc on`
3. Connect USB cable to PC - SD card appears as USB mass storage
4. Copy files from the drive
5. Safely eject drive on PC, then: `usb msc off`

**Notes**:
- USB MSC and filesystem cannot access SD card simultaneously
- Always disable MSC before recording again
- UART shell (921600 baud) uses a separate UART, not USB

### 11. SPI Flash Speed Test

**Purpose**: Test SPI flash (PY25Q64H 64MB) raw read/write/erase performance

**Commands**:
```bash
flash speed            # Test 1MB (default)
flash speed 2048       # Test 2MB
flash speed 512        # Test 512KB
```

**Test Procedure**:
1. Erases test area (4KB sectors)
2. Writes test pattern
3. Reads back and verifies data integrity
4. Reports erase/write/read speeds in KB/s

**Notes**:
- Tests on LittleFS partition area (offset 0x130000), OTA slots are not affected
- 4KB chunk size aligned to flash erase sector
- Max test size: 10MB

### 5. Button Test

**Purpose**: Test user button functionality

**Expected Results**:
- Button presses are detected and logged

### 6. OLED Display Test

**Purpose**: Test CH1115 OLED display (88x48)

**Commands**:
```bash
oled test            # Run automated test
oled clear           # Clear display
oled fill            # Fill display
oled pattern         # Show test pattern
oled circle          # Draw circle
oled pixels          # Draw test pixels
oled brightness <0-255>  # Set brightness
```

**Expected Results**:
- Display shows test patterns correctly
- Brightness adjustment works
- No visible artifacts

### 7. PMIC Test

**Purpose**: Test NPM1300 PMIC battery and power management

**Commands**:
```bash
pmic status          # Show battery/charger status
pmic monitor         # Continuously monitor status
pmic ship            # Enter ship mode (power off)
```

**Expected Results**:
- Battery voltage and percentage display correctly
- Charging status accurate
- Ship mode powers off device

### 8. Motor Test

**Purpose**: Test vibration motor

**Commands**:
```bash
motor on             # Turn motor on
motor off            # Turn motor off
motor pulse <ms>     # Pulse for duration
motor pattern <short|double|long|sos|alert>  # Play pattern
motor test           # Run motor test
```

**Expected Results**:
- Motor turns on/off correctly
- Pulse duration is accurate
- Patterns play as expected

### 9. IMU Test

**Purpose**: Test LSM6DS3TR 6-axis IMU sensor

**Commands**:
```bash
imu on               # Power on IMU (GPIO0.2=HIGH)
imu off              # Power off IMU (GPIO0.2=LOW)
imu init             # Full init (power on + I2C + configure)
imu read             # Read sensor data
imu monitor [n]      # Monitor n iterations (default 10)
imu scan             # Scan I2C bus
imu selftest         # Run self-test
```

**Expected Results**:
- IMU initializes and detects at address 0x6A
- WHO_AM_I returns 0x6C or 0x6A
- Accelerometer and gyroscope data update
- Values change when device is moved

**Interpreting Sensor Data**:
- **Accelerometer**: +/- 4g range, ~1000 LSB/g at rest
- **Gyroscope**: +/- 500dps range, ~0 LSB/s at rest

### 10. Crystal Capacitance Tuning

**Purpose**: Tune internal load capacitance for LFXO (32.768kHz) and HFXO (32MHz) crystals. The board has no external load capacitors — internal capacitance must be configured via registers.

**LFXO Commands** (32.768kHz crystal):

```bash
lfxo get                # Read current capacitance setting
lfxo set <0-3>          # Set capacitance (0=external, 1=6pF, 2=7pF, 3=9pF)
```

**HFXO Commands** (32MHz crystal):

```bash
hfxo get                # Read current capacitance setting
hfxo set <pF>           # Set capacitance in pF (7.0-20.0, step 0.5, 0=external)
```

**Example**:
```bash
uart:~$ lfxo get
LFXO capacitance: 0 (external)
uart:~$ lfxo set 2
LFXO capacitance set to: 2 (7pF)
uart:~$ hfxo get
HFXO capacitance: external
uart:~$ hfxo set 9.0
HFXO capacitance set to: 9.0 pF (CAPVALUE=90)
```

**After tuning**, configure the optimal values in device tree:
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

## Troubleshooting

### IMU Not Detected

**Symptoms**: WHO_AM_I returns 0x00 or no device found

**Solutions**:
1. Check IMU is powered: GPIO0.2 should be high
2. Verify I2C connections: GPIO1.0 (SDA), GPIO1.1 (SCL)
3. Check SDO/SA0 pin is grounded (I2C address 0x6A)
4. Ensure I2C pull-ups are connected to GPIO0.2
5. Run `imu scan` to check for any I2C devices

### PMIC Ship Mode

**Important**: After entering ship mode (`pmic ship`), the device will power off. To wake:
- Connect USB cable
- Press button
- Apply voltage to VBUS

### SD Card Issues

**Symptoms**: Card not mounting or errors

**Solutions**:
1. Check card is properly inserted
2. Try reformatting card as FAT32
3. Use `sd eject` before removing card
4. Check for transient sync errors (these are normal)

### WiFi Connection Failures

**Symptoms**: Cannot connect to WiFi

**Solutions**:
1. Check SSID and password are correct
2. Ensure device supports 5GHz WiFi (nRF7002 AP is 5GHz only)
3. Check antenna is connected
4. Try `wifi on` then check status

## Hardware Specifications

### Pin Assignments

| Function | GPIO | Description |
|----------|------|-------------|
| Button | GPIO1.15 | User button (active low) |
| IMU SDA | GPIO1.0 | I2C data (software) |
| IMU SCL | GPIO1.1 | I2C clock (software) |
| IMU INT1 | GPIO0.3 | IMU interrupt |
| IMU VDD_EN | GPIO0.2 | IMU power enable (NFC1) |
| Motor Ctrl | GPIO1.6 | Vibration motor control (via PMIC GPIO) |
| Mic VDD_EN | GPIO1.14 | Microphone power enable (GPIO-controlled) |
| OLED VDD_EN | GPIO1.8 | OLED power enable (GPIO-controlled) |
| RFSW VDD_EN | GPIO0.29 | WiFi RF switch enable (GPIO-controlled) |

### I2C Devices

| Device | Address | Bus | Description |
|--------|---------|-----|-------------|
| NPM1300 PMIC | 0x6B | I2C1 | Power management IC |
| CH1115 OLED | 0x3C | I2C2 | Display controller |
| LSM6DS3TR IMU | 0x6A | Software I2C | 6-axis IMU sensor |

### Power Supply

- **USB**: 5V VBUS for charging and main power
- **Battery**: Li-Po battery managed by NPM1300
- **PMIC Regulators** (NPM1300):
  - BUCK1: MOTOR_3V3 (vibration motor)
  - BUCK2: VDD_3V3 (main system)
  - LDO1: VDDMIC_1V8 (microphone)
  - LDO2: VDD_SD (SD card)
- **GPIO-controlled Regulators**:
  - Mic VDD_EN: GPIO1.14 (microphone power enable)
  - OLED VDD_EN: GPIO1.8 (OLED display power enable)
  - RFSW VDD_EN: GPIO0.29 (WiFi RF switch enable)

## Memory Usage

```
FLASH:      979 KB (93.4% of 1 MB)
RAM:        374 KB (81.5% of 448 KB)
```

## Test Coverage Matrix

| Module | Power | Comm | Config | Read | Write |
|--------|-------|------|--------|------|-------|
| BLE | ✓ | ✓ | ✓ | - | - |
| WiFi | ✓ | ✓ | ✓ | - | - |
| SD Card | ✓ | - | - | ✓ | ✓ |
| Mic | ✓ | - | ✓ | ✓ | ✓ |
| Button | ✓ | - | - | ✓ | - |
| OLED | ✓ | ✓ | ✓ | - | - |
| PMIC | - | ✓ | ✓ | ✓ | ✓ |
| Motor | ✓ | - | - | - | - |
| IMU | ✓ | ✓ | ✓ | ✓ | ✓ |
| USB MSC | - | ✓ | ✓ | - | ✓ |

## Built-in Shell Commands

The following Zephyr built-in shell commands are available for low-level hardware testing.

### Regulator Shell

Controls PMIC regulators (BUCK1/2, LDO1/2) and GPIO-fixed regulators (mic, oled, rfsw):

```bash
regulator status         # List all regulators and their state
regulator enable <name>  # Enable a regulator
regulator disable <name> # Disable a regulator
regulator vget <name>    # Get regulator voltage
```

Regulator names (from device tree):
| Name | Type | Controls |
|------|------|----------|
| `BUCK1` | NPM1300 | Motor 3.3V |
| `BUCK2` | NPM1300 | Main 3.3V (always-on) |
| `LDO1` | NPM1300 | Mic 1.8V |
| `LDO2` | NPM1300 | SD card 3.3V |
| `mic_vdd` | GPIO-fixed | Mic power enable (GPIO1.14) |
| `oled_vdd` | GPIO-fixed | OLED power enable (GPIO1.8) |
| `rfsw_vdd` | GPIO-fixed | WiFi RF switch (GPIO0.29) |

### GPIO Shell

```bash
gpio get <port> <pin>      # Read GPIO pin state
gpio set <port> <pin> <0|1> # Set GPIO output
gpio conf <port> <pin> <cfg> # Configure GPIO pin
```

Examples:
```bash
gpio set gpio1 14 1   # Enable mic power
gpio set gpio1 8 0    # Disable OLED power
gpio get gpio1 15     # Read button state
```

### I2C Shell

```bash
i2c scan i2c1       # Scan I2C1 bus (PMIC @ 0x6B)
i2c scan i2c2       # Scan I2C2 bus (OLED @ 0x3C)
i2c read i2c1 0x6b <reg> <len>   # Read NPM1300 register
i2c write i2c1 0x6b <reg> <data> # Write NPM1300 register
```

## Development Notes

### Adding New Tests

1. Create source file in `tests/clip/src/`
2. Create header file in `tests/clip/src/`
3. Add to CMakeLists.txt
4. Initialize in main.c
5. Add shell commands

### Code Style

- Follow Zephyr coding style
- Use LOG_MODULE_REGISTER for logging
- Return negative errno on errors
- Check device_is_ready() before using devices

### Shell Commands

Use SHELL_CMD_* macros for shell command registration:
- SHELL_CMD: Simple command
- SHELL_CMD_ARG: Command with arguments
- SHELL_STATIC_SUBCMD_SET_CREATE: Subcommand hierarchy

## Version History

- 2026-05-12: Added SPI flash speed test command
- 2026-05-11: Added LFXO/HFXO crystal capacitance tuning commands
- 2026-05-08: Added USB MSC module (expose SD card as USB drive), added WAV recording
- 2026-04-22: Updated documentation to accurately reflect implemented features, removed non-existent BLE and WiFi scan commands, corrected SD card commands
- 2025-03-09: Added IMU test module with software I2C
- 2025-03-09: Added vibration motor test commands
- 2025-03-09: Added PMIC (NPM1300) test commands
- 2025-03-09: Added OLED display test commands
- 2023: Initial test suite framework

## License

Copyright (c) 2023 Nordic Semiconductor ASA

SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
