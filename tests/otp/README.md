# nRF70 OTP Programming Tool

Factory tool for programming MAC addresses and other OTP data on the nRF7002 WiFi module.

## Build & Flash

```sh
west build --build-dir build-otp --pristine --board clip/nrf5340/cpuapp tests/otp
west flash --build-dir build-otp && nrfutil device reset
```

## Shell Commands

### Check OTP status
```
nrf70 otp status
```
Shows protection state, MAC0/MAC1 addresses, and whether they are programmed.

### Read all OTP fields (raw dump)
```
nrf70 otp read
```

### Write MAC address
```
nrf70 otp write_mac0 AA:BB:CC:DD:EE:FF
nrf70 otp write_mac1 AA:BB:CC:DD:EE:FF
```
- Automatically unlocks OTP if fresh from fab
- Validates MAC address (no multicast, no all-zeros)
- Reads back and verifies after writing

### Lock OTP (IRREVERSIBLE)
```
nrf70 otp lock
```
Permanently locks the OTP region. **This cannot be undone.**

## Typical Factory Flow

1. Flash OTP firmware
2. `nrf70 otp status` → confirm "FRESH (unprogrammed)"
3. `nrf70 otp write_mac0 AA:BB:CC:DD:EE:FF` → write unique MAC
4. `nrf70 otp status` → verify MAC0 is programmed
5. `nrf70 otp lock` → permanently lock
6. Flash production firmware

## OTP Memory Layout

| Field | Word Offset | Size | Description |
|-------|------------|------|-------------|
| REGION_PROTECT | 64-67 | 4 words | Protection state |
| MAC0_ADDR | 72-73 | 2 words | Primary MAC address |
| MAC1_ADDR | 74-75 | 2 words | Secondary MAC address |
| CALIB_XO | 76 | 1 word | XO calibration |
| REGION_DEFAULTS | 85 | 1 word | Field programmed flags |

MAC address format: word0 = byte0\|byte1<<8\|byte2<<16\|byte3<<24, word1 = byte4\|byte5<<8
