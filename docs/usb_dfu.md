# Firmware Upgrade Guide

Host-side procedures for upgrading reSpeaker Clip firmware — no debug probe needed for
the USB and BLE paths. All host tools talk the **mcumgr Simple Management Protocol
(SMP)** over the bootloader's USB CDC-ACM serial port or over BLE.

> The bootloader's USB serial recovery and the application's UART console are
> independent. **Production builds (console off) still support USB serial DFU.**

## At a glance

| Method | Probe? | Tool | Artifact |
|--------|--------|------|----------|
| **USB serial DFU** (recommended) | No | `nrfutil mcu-manager` / `mcumgr` / nRF Connect | `*-signed.bin` or `*-ota.zip` |
| BLE OTA | No | nRF Connect app / `mcumgr` | `*-ota.zip` |
| Programmer | Yes (J-Link) | `west flash` / `nrfutil device program` | `*-merged.hex` |

Native USB-DFU (`dfu-util`) is **not** used — see [Why not dfu-util / native USB-DFU?](#why-not-dfu-util--native-usb-dfu).

## Artifacts (per release, in `output/<version>/`)

| File | What it is | Use with |
|------|-----------|----------|
| `clip-<v>-*-merged.hex` | Full image (MCUboot + app + net core) | Programmer |
| `clip-<v>-*-merged_CPUNET.hex` | Network core only | Programmer |
| `clip-<v>-*-signed.bin` | Signed **app** image | USB serial DFU, BLE (mcumgr) |
| `clip-<v>-*-ota.zip` | Multi-image package (app + net core) | USB serial DFU, BLE (mcumgr/nRF Connect) |

For an app-only field upgrade, upload the `-signed.bin`. For a full app + net-core
upgrade, upload the `-ota.zip`.

## Entering USB serial recovery mode

MCUboot (`CONFIG_MCUBOOT_SERIAL=y`, `CONFIG_BOOT_SERIAL_CDC_ACM=y`) exposes an SMP
console over USB. Enter it one of two ways (USB/VBUS must be present):

- **Button** (field): hold the **user button** (GPIO1.15) while connecting USB
  (or while powering on). The device enumerates as a serial port, e.g. `/dev/ttyACM0`
  (Linux), `COMx` (Windows), `/dev/cu.usbmodem*` (macOS).
- **Software** (app-initiated): the app writes the boot-mode retention register
  (`boot_mode0`, the `zephyr,boot-mode` node) and reboots; MCUboot then enters
  serial recovery on the next boot.

VBUS gating (MCUboot patch `0001-require-vbus-for-gpio-serial-recovery`) requires a
USB connection for serial recovery, so the device is never left listening for DFU on
battery.

## USB serial DFU procedures

Pick any one tool — they all speak SMP over the same CDC-ACM port. Adjust the
serial port and firmware path to your system.

### Option A — `nrfutil mcu-manager` (recommended)

nrfutil (Nordic's tool; ≥ 8.x with the `mcu-manager` plugin) speaks mcumgr SMP
over serial or BLE.

```sh
# List serial ports
nrfutil device list

# Upload the app image (single-core field upgrade)
nrfutil mcu-manager serial image-upload \
    --firmware clip-0.0.5-production-signed.bin \
    --serial-port /dev/ttyACM0

# ...or the full package (app + net core)
nrfutil mcu-manager serial image-upload \
    --firmware clip-0.0.5-production-ota.zip \
    --serial-port /dev/ttyACM0

# Reset to apply (MCUboot verifies signature and swaps on next boot)
nrfutil mcu-manager serial reset --serial-port /dev/ttyACM0
```

`--firmware` accepts BIN, HEX, or ZIP (ZIP = multi-image package).

### Option B — `mcumgr`

The upstream mcumgr CLI (`go install github.com/apache/mynewt-mcumgr/cli/mcumgr@latest`).

```sh
mcumgr --conntype serial --connstring dev=/dev/ttyACM0,mtu=512 \
    image upload clip-0.0.5-production-signed.bin

# Inspect / confirm / reset
mcumgr --conntype serial --connstring dev=/dev/ttyACM0,mtu=512 image list
mcumgr --conntype serial --connstring dev=/dev/ttyACM0,mtu=512 reset
```

### Option C — nRF Connect

- **Desktop**: Programmer app → select the serial port → add the `-signed.bin` or
  `-ota.zip` → "Update".
- **Mobile**: Device Manager app → connect over BLE → "Add file" → the `-ota.zip` →
  "Update". (This is the BLE path; see below.)

## BLE OTA

Same SMP protocol over Bluetooth LE. Use the `-ota.zip`.

- **nRF Connect mobile** (Device Manager): connect to the device → "Add file" →
  `clip-<v>-*-ota.zip` → "Update".
- **nrfutil**:
  ```sh
  nrfutil mcu-manager ble image-upload \
      --firmware clip-0.0.5-production-ota.zip \
      --address <DEVICE-BLE-MAC>
  ```
- **mcumgr**:
  ```sh
  mcumgr --conntype ble --connstring peer_name=ClipAP_... \
      image upload clip-0.0.5-production-ota.zip
  ```

## Programmer (when a probe is available)

Full image, no signature-slot dance:

```sh
west flash --build-dir build-clip && nrfutil device reset
# or
nrfutil device program --firmware clip-0.0.5-production-merged.hex --serial-number <JLINK-SN>
```

> `west flash --reset` does **not** work on this board — reset separately with
> `nrfutil device reset`.

## Why not dfu-util / native USB-DFU?

`dfu-util` speaks the standard USB DFU device class (`USB_DFU_CLASS`), which is a
**different transport** from mcumgr-over-CDC-ACM. MCUboot supports it
(`BOOT_USB_DFU_WAIT`/`BOOT_USB_DFU_GPIO` choice), but it is **disabled** here
(`BOOT_USB_DFU_NO`), and intentionally so:

- The bootloader partition is ~99% full (USB CDC-ACM serial recovery + OLED already
  occupy the slot). Enabling the DFU class on top would overflow it; you would have
  to drop the OLED display patch or the CDC-ACM recovery, or grow the mcuboot
  partition (repartitioning the app slots).
- NCS/mcuboot's primary, well-supported upgrade path is mcumgr SMP — over serial
  CDC-ACM **and** BLE — which already covers USB and wireless field upgrades.

So the mcumgr path (above) is the supported route. `dfu-util` would only be worth
revisiting if a host without mcumgr/nrfutil must be supported and the bootloader
partition is enlarged.

## Post-upgrade verification

After the swap and reboot:

```sh
nrfutil mcu-manager serial image-list --serial-port /dev/ttyACM0   # running image + hash
# or on the device: AT+VERSION  →  confirms the new version string
```
