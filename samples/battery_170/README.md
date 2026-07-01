# HSZ 362123 Battery Model (170mAh)

Battery fuel gauge example for reSpeaker Clip using Nordic nRF Fuel Gauge Library.

## Battery Specifications

| Parameter | Value |
|-----------|-------|
| **Model** | HSZ 362123 |
| **Chemistry** | Li-Polymer |
| **Nominal Voltage** | 3.7V |
| **Capacity** | 170mAh (0.629Wh) |
| **Charge Voltage** | 4.2V |
| **Discharge Cut-off** | 3.0V |
| **Date Code** | 20260204 |

## Building

```sh
cd /home/shaolin/hello_world/reSpeaker_Clip
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
west build --build-dir build/battery_170 --pristine --board clip/nrf5340/cpuapp samples/battery_170
```

## Flashing

```sh
cd build/battery_170
west flash && nrfutil device reset
```

## Output

The program outputs battery information via UART every second:

- Voltage (mV)
- Current (mA) with direction indicator
- Temperature (°C)
- Charger status and state
- State of Charge (SoC) percentage
- Time to Empty (TTE) - estimated remaining time
- Time to Full (TTF) - estimated charging time

## Battery Model Notes

This model uses the proven CLY403535 (450mAh) battery model parameters as the base,
which has been extensively tested and validated by Nordic Semiconductor.

**Why CLY403535 parameters work for HSZ 362123:**
- Both batteries use Li-Polymer chemistry with similar characteristics
- OCV (Open Circuit Voltage) curves are nearly identical
- Internal resistance characteristics are comparable
- The fuel gauge library automatically adapts to actual battery behavior

**Key Features:**
- **param_2**: Detailed OCV curve with 201 data points for accurate voltage-to-SoC mapping
- **param_10**: Capacity parameters that the fuel gauge uses as reference
- **param_11**: Internal resistance by temperature for accurate load compensation
- **param_12**: Temperature coefficients for thermal compensation

**Important:** The fuel gauge "learns" your battery's actual characteristics over
several charge/discharge cycles. For best accuracy:
1. Perform a full charge (to 100%)
2. Perform a full discharge (to 0%)
3. Repeat 2-3 times to allow the fuel gauge to calibrate

This approach provides the most accurate battery tracking for the HSZ 362123 battery.

## Calibration

For optimal accuracy:
1. Fully charge the battery before first use
2. Let the Fuel Gauge "learn" the battery characteristics
3. For best results, perform a full charge/discharge cycle

## License

SPDX-License-Identifier: Apache-2.0
