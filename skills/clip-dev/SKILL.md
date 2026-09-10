---
name: clip-dev
description: Develop, build, flash, test, or debug reSpeaker Clip / B·Pin firmware on the nRF5340 with NCS v3.3.0. Use for Zephyr board work, MCUboot/sysbuild, AT commands over USB CDC, WiFi station mode and HTTPS upload, SD recording storage, audio/DSP, OLED, PMIC, battery, low-power behavior, production images, and hardware tests.
---

# reSpeaker Clip firmware

Treat the checked-out source as authoritative. Read `CLAUDE.md` before changing
firmware: it contains the active NCS version, board sysbuild defaults, power
constraints, release workflow, and current hardware-test commands.

## Route the task

| Area | Source of truth | Read when needed |
|---|---|---|
| AT protocol | `applications/clip/src/at_commands.c` | `references/ble-at.md` (stale: transport is USB CDC now) |
| WiFi STA, upload, heartbeat | `wifi.c`, `http_upload.c`, `health.c` | source; `CLAUDE.md` |
| Bench harnesses | `applications/clip/tests/hil/` | that directory's `README.md` |
| Recording and DSP | `audio.c`, `applications/clip/Kconfig` | `references/audio.md` |
| FATFS sessions | `storage.c` | `references/storage.md` |
| Idle and power-off | `clip_event.c`, `storage.c`, `battery.c`, snippets | `references/power.md` |
| Build, flash, artifacts | `CLAUDE.md`, board `Kconfig.sysbuild` | `references/build-flash.md` |
| Python package | `sdk/` | Use the separate `clip-sdk` skill |

Keep `applications/clip/tests` intact. It is a legacy test/tool collection;
the installable SDK lives at `sdk/` and must not silently inherit legacy
commands or response shapes.

## Firmware workflow

1. Inspect `git status`; preserve unrelated worktree changes.
2. Confirm the command, Kconfig, device-tree, storage, or transport contract in
   source before editing docs or clients.
3. Keep host-visible session IDs as exactly 14 decimal digits
   `YYYYMMDDHHMMSS`; never expose physical FAT paths in the protocol.
4. Validate user-controlled command arguments before storage, path, or transfer
   access. `DOWNLOAD` only accepts `session` or `session:NNNN.opus`.
5. Build and test in proportion to the change. Flash only the image the user
   requested; `--recover` erases the device.
6. Update `docs/protocol.md` and `sdk/` whenever an AT response, command, or
   binary transfer frame changes.

## Current protocol rules

- Success is `{"ok":true,"data":...}` where `data` is command-specific.
  Failures use `{"ok":false,"msg":"..."}`; do not expect `error` or numeric
  error-code fields.
- Use `AT+GSTAT`, not `AT+STATUS?`. Current recording commands are `START`,
  `STOP`, `PAUSE`, `RESUME`, and `MARK`.
- Runtime commands for bitrate, codec complexity, AGC, noise suppression, and
  dereverb do not exist. Audio mode is `normal` or `enhanced`.
- `AT+PAIR=reset` acknowledges first, then erases SD and reboots. Do not make a
  client wait for the erase before receiving the response.
- **BLE, WiFi AP mode and the UDP control channel are gone.** The only control
  channel is USB CDC; the only transport out is HTTPS in station mode. `AT+STA`,
  `AT+STACFG` and `AT+UPCFG` are the relevant commands.
- **`AT+STA=on` takes a radio lease that never expires.** It holds until
  `AT+STA=off`, pinning the radio on (~41 mA instead of ~21) while the device
  still looks perfectly healthy. Always release it before handing a device over.
- **`AT+USB=off` disables CDC as well as MSC** and removes the AT channel; only a
  physical replug brings it back.

## Build and flash

`zephyr-env.sh` does **not** provide `west` — it lives in Nordic's toolchain
bundle with its own Python, and the bundle's `NRFUTIL_HOME` hides the
`mcu-manager` command needed to flash. Read `references/build-flash.md` for the
full environment before building; guessing it costs a cycle every time.

The board is sealed (no SWD), so installation is over USB serial recovery, not
`west flash`. Build production with the `production` snippet; it disables
console/UART logging and is required for meaningful idle-current measurements.

## Validation checkpoints

- Build a pristine image after Kconfig, devicetree, sysbuild, or partition
  changes.
- Flash both app and network-core images when checking BLE behavior.
- For low power, test the production image with SD idle power-off and FS logging
  disabled; debug UART materially changes current.
- **Anything involving the SD card sleeping must be tested on battery.** With a
  cable attached `clip_sd_busy()` stays true (for 10 minutes past VBUS removal),
  so the card never idles off and the behaviour cannot occur. Recording is also
  refused while USB is up and VBUS present. Read such runs from the heartbeat,
  not the AT channel.
- **The SD log only covers the first 120 s of each boot**
  (`CLIP_LOG_FS_BOOT_WINDOW_S`). It is a boot log, not a window into steady
  state; anything that must be diagnosable later belongs in the heartbeat.
- For storage changes, test new recording, `LIST`, `DOWNLOAD`, cancellation,
  deletion, and a power-cycle/remount path.
- For upload changes, check the whole path end to end: recording on the card,
  the `uploads` row on the service with `ok=1`, and `pending_files` returning to
  zero for the right reason. A failed card listing must never be reported as an
  empty backlog.
