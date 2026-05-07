# BLE Direct Test Mode (DTM) for Clip

Bluetooth Direct Test Mode (DTM) firmware for RF testing and certification on the ReSpeaker Clip board.

## Overview

DTM is a Bluetooth SIG standard test mode for RF conformance testing. It uses a 2-wire UART interface at 19200 baud to control radio TX/RX operations.

The Clip board uses nRF5340 dual-core architecture:
- **Network core (cpunet)**: Runs DTM firmware controlling the radio
- **Application core (cpuapp)**: Runs remote_shell to bridge IPC to physical UART (P1.04/P1.05)

## Build

```sh
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
west build --build-dir build-dtm --pristine --board clip/nrf5340/cpunet tests/dtm
```

## Flash

```sh
west flash --build-dir build-dtm && nrfutil device reset
```

This flashes both the cpunet (DTM) and cpuapp (remote_shell) images.

## Hardware Connection

Connect a DTM test tool to the Clip board's UART:
- **TX**: P1.04 (Clip → Test Tool)
- **RX**: P1.05 (Test Tool → Clip)
- **Baud rate**: 19200
- **Format**: 8N1

## DTM Commands

DTM uses a 2-wire UART protocol (Bluetooth SIG standard):

| Command | Bytes | Description |
|---------|-------|-------------|
| Reset | `0x00 0x00` | Reset DTM state |
| TX Test | `0x01 0xXX 0xYY 0xZZ` | Start TX (channel, length, pattern) |
| RX Test | `0x02 0xXX 0x00 0x00` | Start RX (channel) |
| Test End | `0x03 0x00` | Stop test, get packet count |

Response format: 2 bytes (event code + status/count).

## Troubleshooting

- **No response on UART**: Verify baud rate is 19200, check TX/RX wiring
- **Build errors**: Ensure `ZEPHYR_EXTRA_MODULES` points to the Clip project root
- **Flash fails**: Try `nrfutil device recover` to unlock the device

## References

- [Nordic DTM Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/bluetooth/direct_test_mode/README.html)
- [Bluetooth DTM Specification](https://www.bluetooth.com/specifications/specs/direct-test-mode/)
