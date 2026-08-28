# Build, flash, and release

NCS v3.3.0. Work on `clean-repo` (branched from `feat/https`); `main` is stale and
describes a different device.

```sh
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh   # SDK may also live at /opt/nordic/ncs/v3.3.0
export ZEPHYR_EXTRA_MODULES="$PWD"          # env var, NOT -D: Kconfig discovers
                                            # modules before CMake exists
west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip
```

Use `--pristine` after Kconfig, devicetree, sysbuild, partition, or board-level
changes. If a build directory came from another machine, **delete it** —
`--pristine` does not clear the dead absolute paths in `CMakeCache.txt`, and the
symptom is the self-contradicting `No board named 'clip' found. Did you mean: clip`.

The board supplies MCUboot, IPC radio, Wi-Fi, signing keys, and static partition
defaults through `boards/seeed/clip/Kconfig.sysbuild`; applications do not normally
need their own sysbuild configuration.

## Images

**The default build is the shipping security posture: TLS on, Bluetooth off, access
point off.** No overlay is needed to get TLS — `CONFIG_CLIP_UPLOAD_TLS=y` lives in
`prj.conf`.

```sh
# Debug — shipping posture, console on, FS log to the microSD
west build --build-dir build-clip --pristine --board clip/nrf5340/cpuapp applications/clip

# Production — same posture, console and UART log backend off (~170 µA idle target)
west build --build-dir build-clip-prod --pristine --board clip/nrf5340/cpuapp \
  applications/clip -- -DSNIPPET_ROOT="$PWD/applications/clip" -DSNIPPET=production

# Bench only, NOT deployable — Bluetooth and AP back, and TLS OFF
west build --build-dir build-dev --pristine --board clip/nrf5340/cpuapp \
  applications/clip -- -DEXTRA_CONF_FILE="$PWD/applications/clip/overlay-dev-radio.conf"
```

Use the production image for any current measurement: the debug UART leaks ~570 µA
at idle and hides everything else.

Check size on every build. The app slot is `0xe9e00` = 957,952 B and the image runs
near 97%:

```sh
west build -d build-clip/clip -t rom_report
west build -d build-clip/clip -t ram_report
```

## Installing firmware

There is no debug probe on this bench, and the production enclosure exposes no SWD.
Firmware goes in over MCUboot USB serial recovery — the same path a field device uses:

```sh
# Enter recovery (or send AT+DFU, or hold the button while plugging in)
python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',1200); time.sleep(0.5); s.close()"

nrfutil mcu-manager serial image-upload \
  --firmware build-clip/clip/zephyr/zephyr.signed.bin --serial-port /dev/ttyACM0
nrfutil mcu-manager serial reset --serial-port /dev/ttyACM0
```

- Upload `zephyr.signed.bin`, **not** `merged.hex` — the latter carries the
  bootloader and only goes in over a probe.
- In recovery **two ports appear**; SMP answers on **vcom 0** (the lower number).
  The other returns `Timeout occured`.
- Confirm with `image-list`: the slot-0 hash changing is the only proof the upload
  landed.
- `nrfutil device list` distinguishes the modes: `reSpeaker Clip` vs
  `reSpeaker Clip DFU`.

If a probe is available: `west flash --build-dir <dir> && nrfutil device reset`.
`west flash --reset` does not work on this board. Use `--recover` only when
necessary — it erases both cores.

## Release artifacts

**There is no release script in the tree.** `scripts/build_release.sh`, referenced
here previously, does not exist and never has — there is no `scripts/` directory.

Releases come from the tag-triggered `.github/workflows/release.yml`, which builds
both variants and exports eight artifacts: merged app and netcore HEX,
`dfu_application.zip`, and `zephyr.signed.bin`, for debug and production. That
workflow reads `docs/release_notes/v<VERSION>.md` and **fails if the file is
missing**, so write it before tagging.

Note the workflow currently triggers only on `main`, so nothing on this line has
ever been built by CI.
