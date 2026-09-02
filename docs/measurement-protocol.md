# Measurement protocol — battery and transmission

How to build, flash and measure so that two results taken a week apart mean something
next to each other. Written for the 2026-09 battery campaign.

**The one rule: one change, one measurement.** Front C is three independent fixes and it
will be tempting to flash them together. Do that and none of them is attributable.

---

## 0. Environment

```sh
cd <repo>
source ./env.sh
```

That is the Linux equivalent of `nrfutil sdk-manager toolchain launch` on the Mac. It puts
the toolchain bundle on `PATH` **and** on `LD_LIBRARY_PATH` — without the second, `west`
dies with `libpython3.12.so.1.0: cannot open shared object file`, which reads like a broken
install and is not.

Expect: west 1.5.0 · Python 3.12.4 · CMake 4.2.1 · gcc 12.2.0 (Zephyr SDK 0.17.0).
These match the versions recorded on the Mac. The toolchain bundle hash differs by OS
(`911f4c5c26` here, `0c0f19d91c` on the Mac); that is expected and not a discrepancy.

**Do not update NCS mid-campaign.** "Version A drew 3.2 mA, version B drew 2.8 mA" only
means something if the compiler did not change underneath. If you must update, re-baseline.

### One-time, before the first flash

```sh
# MCUboot patches into the NCS tree, or the build fails with
# "undefined symbol MCUBOOT_DISPLAY"
MB=$HOME/ncs/v3.3.0/bootloader/mcuboot
for p in "$PWD"/patches/mcuboot/000*.patch; do git -C "$MB" apply "$p"; done

# Serial access without sudo. Requires log out / log in, or `newgrp dialout`.
sudo usermod -aG dialout "$USER"

# Flashing over USB serial recovery
nrfutil install mcu-manager
```

---

## 1. The loop

```
   build baseline ──▶ measure ──▶ record ──▶ change ONE thing ──▶ build ──▶ measure ──▶ compare
                                                   ▲                                    │
                                                   └────────────────────────────────────┘
```

### Build

```sh
# Production. Use this for every current measurement.
west build --build-dir build-prod --board clip/nrf5340/cpuapp applications/clip -- \
  -DSNIPPET_ROOT="$PWD/applications/clip" -DSNIPPET=production

# Size, every time
west build -d build-prod/clip -t rom_report | tail -20
west build -d build-prod/clip -t ram_report | tail -20
```

Record the FLASH and RAM totals with every measurement. The app slot is `0xe9e00` =
**957,952 bytes** and the image runs near 97%; a change that does not fit is a result too.

Use `--pristine` after any change to Kconfig, devicetree, sysbuild, partitions or the board.

**Always measure on the production image.** The debug UART leaks ~570 µA and swamps
everything else — larger than several of the effects being hunted.

### Flash

```sh
# Enter recovery (or AT+DFU, or hold the button while plugging in)
python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',1200); time.sleep(0.5); s.close()"

nrfutil mcu-manager serial image-upload \
  --firmware build-prod/clip/zephyr/zephyr.signed.bin --serial-port /dev/ttyACM0
nrfutil mcu-manager serial reset --serial-port /dev/ttyACM0
```

Two ports appear in recovery; SMP answers on **vcom 0**, the lower number. Confirm with
`image-list`: the slot-0 hash changing is the only proof the upload landed.

---

## 2. What to measure

### Per-state current

Production image, PPK2 on the 3V3 rail. Fill this in per firmware version:

| State | How to reach it | mA | Notes |
|---|---|---|---|
| Idle, display off | boot, wait for the screen to sleep | | |
| Idle, charging | plug USB | | |
| Recording, radio off | `AT+STA=off` then `AT+START` | | |
| Recording, radio associated | `AT+STA=on` then `AT+START` | | |
| Uploading | `AT+HTTPUP=<session>` | | peak and mean |
| Ship mode | `AT+POWEROFF` | | should be near zero |

**Idle life is computed, not observed.** At the 170 µA target a 170 mAh cell runs ~1,000
hours — about 41 days. Measure current, then divide. Only recording and upload are short
enough to time directly.

### On-device current

`battery_ma` in the heartbeat and `current_ma` in `AT+BATT?` report the NPM1300 IBAT sense.
Negative = discharging.

**It excludes the radio.** The nRF7002 taps VBAT upstream of the PMIC, so its current never
crosses the sense resistor. Consequences:

- Trustworthy for idle, recording, display, SD, motor.
- **Blind to WiFi** — the largest suspected consumer. Radio work must be measured with the
  PPK, not on-device.
- The fuel gauge is fed this same blind current, so its state of charge is optimistic
  whenever the radio is on. `wifi_load_estimate_a()` was meant to compensate, but it only
  fires when `wifi_ap_is_running()`, and AP mode is compiled out — so in every shipping
  build it returns 0 mA regardless of what the radio is doing.

### Transmission

`AT+HTTPUP?` gives `files_done`, `bytes`, `stack_free`. The heartbeat additionally carries
`up_kbps`, `up_ok`, `up_fail`, `pending_files`.

**Treat `up_kbps` as suspect until T2.6.1 lands.** `t0` is taken outside
`http_upload_file()`, so the number folds the TLS handshake, headers, body and response
together — and `CONNECT_TIMEOUT_MS` is 20 s with TLS. For a small file it is mostly
handshake.

---

## 3. Recording results

Two places, both already exist:

**Fleet series** — the receiver writes every heartbeat to SQLite:

```sh
python3 applications/clip/tests/tools/bpin_http_receiver.py --port 8080 --out ./recordings
curl "http://localhost:8080/health.csv?hours=24" > runs/<version>-<state>.csv
```

**Bench numbers** — commit a row per measurement so comparisons survive the session:

```
version,commit,image,state,mA,flash_bytes,flash_pct,ram_pct,cell,notes
0.2.0,4a2fd94,production,idle-display-off,,,,,unit-1,baseline
```

Record the **commit hash** and the **cell** with every row. Different cells are different
experiments — see below.

---

## 4. Traps

**The test cell may not be honest.** `term-microvolt = 4.25 V` on a 4.20 V cell is a
deliberate overcharge, and Doc 22 suspects it has degraded the only test unit. The signature
is an instant voltage sag under load, read by the gauge as a collapse in charge. If it is
degraded, absolute figures describe a bad cell rather than the fleet — **relative comparisons
on the same cell stay valid regardless.** Characterise it once and carry a correction factor.

**170 µA is a target, not a measurement.** The SD power-gate never executes, USB stays
enabled 10 minutes after unplug, three rails are never gated, and RPU power-save is off.
Expect idle to come back several times the target; that gap is the backlog.

**`stack_free` reads 0 if `CONFIG_INIT_STACKS` is off.** It is on for this campaign, and
should be turned off again at the end to reclaim flash. If it still reads 0 with the setting
on, add `CONFIG_THREAD_STACK_INFO=y`.

**No shell metacharacters anywhere in the workspace path.** CMake writes paths unquoted
into the generated ninja rules, so an `&`, a space, a `(` or a `;` in any parent directory
name is re-parsed by `/bin/sh`. This cost an hour: with the repo under
`.../trabajo/mic&pose/code/...`, the build failed on `keygen.py` with exit 127 and the real
clue was buried — `/bin/sh: 1: pose/code/...: not found`, i.e. the shell had split at the `&`
and run `.../trabajo/mic` in the background. **A symlink does not fix it**; CMake resolves
some targets back to the real path and the build fails later, on `app_version.h`, which is
harder to read. Rename the directory.

**Do not enable `AT+LOG` while USB MSC is mounted.** Two writers on the same FAT volume; it
has taken the device down.

**Long operations need a long read window.** `AT+DELETE=all` over ~100 sessions takes >30 s
and the reply is lost to short timeouts.

---

## 5. Acceptance for a firmware change

Before recording a result as real:

1. Builds clean, **zero warnings**.
2. FLASH and RAM recorded, and within the slot.
3. `AT+HEALTH?` returns a **complete** response — a truncated snapshot returns `-ENOMEM` and
   silently stops the heartbeat, which has happened before.
4. `AT+BATT?` reports a plausible `current_ma` — negative on battery, positive on charger.
   A constant 0 means the sensor path is not populating.
5. One beat reaches the receiver and lands in SQLite.
6. Measurement taken on the same cell as the baseline it is compared against.
