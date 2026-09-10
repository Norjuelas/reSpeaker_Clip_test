# reSpeaker Clip / B·Pin Firmware

Zephyr RTOS firmware for a **wearable voice recorder** built on the Seeed reSpeaker Clip
(Nordic nRF5340 + nRF7002). It records Opus audio to an **encrypted file** on a microSD card and
pushes it to an **HTTPS endpoint over WiFi station mode**. Control is by **AT commands over USB
CDC serial**.

> **There is no Bluetooth, no WiFi access point and no phone app in this build.** If you are
> reading about BLE, GATT, `ClipAP_XXXX` or `192.168.4.1`, you are reading about the old device
> (v0.0.8, on `main`, July 2026). BLE was removed as a **security** decision, and the WiFi AP and
> UDP control channel were removed because the UDP server answered `AT+FACTORY` and `AT+FORMAT`
> to anyone on the network.

The enclosure is sealed — **no SWD access** — so firmware is installed over USB serial recovery.
Every flash therefore exercises the real field update path.

## Hardware

| Component | Part |
|---|---|
| MCU | nRF5340 (application + network core) |
| WiFi | nRF7002 over QSPI, **station mode** |
| PMIC / charger | NPM1300 + nRF Fuel Gauge (custom 170 mAh cell model) |
| Display | CH1115 OLED 88×48 (I2C2) |
| Audio | PDM microphone pair (PDM0) |
| Storage | microSD over SPI4 (FAT) + PY25Q64H 8 MB SPI flash (LittleFS) |
| Control | USB CDC ACM + MSC, 1200-baud DFU trigger |

## How one recording travels

```
botón → clip_event.c → audio.c → audio_crypto.c → storage.c → http_upload.c → HTTPS
```

- **Opus** encoding, then **AES-128-GCM** at rest in a BPE2 container. The reference decryptor,
  `applications/clip/tests/tools/bpin_decrypt.py`, *is* the format spec.
- Sessions are `YYYYMMDDHHMMSS`, chunked under `/SD:/REC/YYYYMMDD/HH/MM/SS/`.
- Uploads go over **TLS 1.2, ECDHE-ECDSA P-256**. The server certificate must be EC P-256; an RSA
  certificate or a TLS 1.3-only server fails the handshake without saying why.
- `/SD:/UPLOADED.TXT` records what has been sent. It **fails open**: unreadable means re-upload,
  because a duplicate costs storage and a loss costs audio.
- Every 15 minutes the radio wakes for one window that does **both** jobs — send the heartbeat and
  drain the upload backlog — so one association pays for both.

## Constraints worth knowing before you touch anything

**Flash is the binding constraint.** The linker offers **933,376 bytes** and the image sits at
about **99%**. Measure before adding anything: `west build -d build-clip/clip -t rom_report`.
This single number explains most of the architecture — why Bluetooth is compiled out, why the JSON
parser in `health.c` is hand-rolled, and why the 87 KB nRF7002 firmware patch lives in its own
flash partition instead of the image.

**Static RAM is at about 95%.** Any buffer over ~1 KB goes on the heap, never on a thread stack.
This firmware has crashed three times from stack sizing.

Full guidance, hard invariants and the accumulated list of pitfalls: **[CLAUDE.md](CLAUDE.md)**.

## Build

`zephyr-env.sh` alone does **not** give you `west` — it lives inside Nordic's toolchain bundle with
its own Python. The bundle's `environment.json` is the authoritative list of what to export:

```sh
T=~/ncs/toolchains/<hash>                    # el bundle instalado por nRF Connect
export PATH="$T/bin:$T/usr/bin:$T/usr/local/bin:$T/opt/bin:$T/nrfutil/bin:\
$T/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
export LD_LIBRARY_PATH="$T/lib:$T/lib/x86_64-linux-gnu:$T/usr/local/lib:$LD_LIBRARY_PATH"
export PYTHONHOME="$T/usr/local"
export PYTHONPATH="$T/usr/local/lib/python3.12:$T/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$T/opt/zephyr-sdk"
export ZEPHYR_BASE=~/ncs/v3.3.0/zephyr
export ZEPHYR_EXTRA_MODULES=$(pwd)           # env var, no -D: Kconfig descubre
                                             # los módulos antes de que exista CMake

west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip
```

Board identifier is `clip/nrf5340/cpuapp`, not `respeaker/...`. Every build is a **sysbuild**
(MCUboot + app core + network-core radio); the board's `Kconfig.sysbuild` supplies the defaults.

Use `--pristine` after any change to Kconfig, devicetree, sysbuild, partitions or the board.

**Production image** (console off — required for any current measurement):

```sh
west build --build-dir build-clip-prod --board clip/nrf5340/cpuapp applications/clip -- \
  -DSNIPPET_ROOT=$(pwd)/applications/clip -DSNIPPET=production
```

## Install — USB only, no probe

```sh
# 1. Entrar en recuperación de MCUboot (o AT+DFU, o botón pulsado al enchufar)
python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',1200); time.sleep(0.5); s.close()"

# 2. Subir la imagen firmada y reiniciar
nrfutil mcu-manager serial image-upload \
  --firmware build-clip/clip/zephyr/zephyr.signed.bin --serial-port /dev/ttyACM0
nrfutil mcu-manager serial reset --serial-port /dev/ttyACM0
```

- In recovery **two ports appear**; SMP answers on **vcom 0** (the lower number).
- Upload `zephyr.signed.bin`, **not** `merged.hex` — the latter contains the bootloader and only
  goes in over a probe.
- **Do not export the toolchain's `NRFUTIL_HOME` when flashing.** It hides the user's own
  `~/.nrfutil`, and `mcu-manager` only exists there. The build environment and the flash
  environment are not the same environment.

## Provisioning, by cable, once per device

```
AT+STACFG="<ssid>","<psk>"     credenciales WiFi
AT+UPCFG="<host>",<port>       endpoint de subida
AT+KEYCFG=<32 hex>             clave de cifrado del audio en reposo
AT+TIME=<epoch>                reloj — no hay fuente de hora autónoma
```

The fleet CA goes on the card as `/SD:/ca.pem`, and **`nrf70.bin` must be on the card before first
boot** or WiFi never comes up.

## Repository map

| Path | What |
|---|---|
| `applications/clip/` | the product firmware |
| `applications/clip/tests/hil/` | bench harnesses — association gates, battery logging. **Look here before writing a new one**: numbers only compare when the same script produced them |
| `applications/clip/tests/tools/` | host side: bench receiver, cable provisioning, BPE2 decryptor, Opus/Ogg wrapper |
| `applications/clip/tests/audio_test/` | ASR-scored audio quality harness — run before and after any codec or DSP change |
| `boards/seeed/clip/` | board support package; `pm_static_*.yml` is the authoritative partition map |
| `drivers/`, `lib/`, `dts/`, `sysbuild/` | out-of-tree drivers, vendored Opus/SpeexDSP, module wiring |
| `patches/mcuboot/` | five bootloader patches (VBUS-gated recovery, OLED UI, progress hooks) |
| `tests/` | standalone firmware images flashed *instead of* the product, over SWD |
| `samples/` | one-idea reference apps — **prototype here**, you cannot try things in a 99%-full image |

Nothing in `tests/` is a unit test. Host-side tests live in `sdk/tests/`.

## Testing on hardware

Bench harnesses live in `applications/clip/tests/hil/` and refuse to run when the bench is dirty —
a radio lease left over from `AT+STA=on` (which never expires) or the SD card mounted on the host
(which stops the radio *associating*, not just uploading). Both have destroyed real measurements.

```sh
cd applications/clip/tests/hil
python3 assoc_gate.py /tmp/assoc.log 20      # arranque en frío de la radio, N ciclos
python3 traffic_gate.py /tmp/traffic.log 20  # igual, con tráfico TLS de por medio
```

**One class of bug these cannot see.** With a USB cable attached the SD card never idle-powers-off,
so anything that depends on the card sleeping only reproduces **on battery**. Recording is also
refused while USB is up and VBUS present, since the host could be writing the card over MSC.

## Security posture

TLS is on by default and BLE stays out even if flash appears. Two things are **not** resolved and
are tracked as such: MCUboot is signed with Nordic's *published example key*, so signature
verification currently protects nothing; and the network-core key `b0-ecdsa-p256.pem` is a real
private key in this repository's history whose public hash is in immutable boot on shipped units —
it **cannot be rotated on existing hardware**. There is also **no authentication on the AT
channel**, which exposes `FACTORY`, `FORMAT`, `DELETE`, `WIPE` and `DFU` to anyone with a cable.

## Documentation

`SETUP.md` and [CLAUDE.md](CLAUDE.md) are current. Most of `docs/` still describes the v0.0.8
device and is being replaced — treat the source as authoritative over any of it. The code comments
in `applications/clip/src/` and the help text in `applications/clip/Kconfig` are the best
documentation in the project, because they record *why*, including the attempts that failed.

## License

[Apache License 2.0](LICENSE). Vendored libraries (Opus, SpeexDSP) retain their own licenses.

## Acknowledgements

[Nordic Semiconductor](https://www.nordicsemi.com/) · [Zephyr Project](https://zephyrproject.org/) ·
[Seeed Studio](https://www.seeedstudio.com/)
