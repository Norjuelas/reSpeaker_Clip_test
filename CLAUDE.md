# CLAUDE.md

Guidance for Claude Code working in this repository. Keep this file short and
invariant-shaped — detail belongs in `docs/`, which this file points to.

**Branch:** `clean-repo`, from `feat/https`. This is the active line.
`main` is stale (v0.0.8, July 2026) and describes a different device — do not use it as a
reference for anything.

---

## What the device is

A wearable voice recorder (Seeed reSpeaker Clip, nRF5340) that records Opus audio to an
encrypted file on a microSD card and pushes it to an HTTPS endpoint over WiFi **station mode**.
Control is by AT commands over **USB CDC serial**. The enclosure is sealed: no SWD access, so
firmware is installed over USB serial recovery.

There is **no Bluetooth, no WiFi access point, and no phone app** in this build. If you are
thinking about BLE, GATT, `ClipAP_XXXX`, or `192.168.4.1`, you are thinking about the old device.

---

## Hard invariants

Violate these and the build fails, the device bricks, or a fleet in the field breaks.

**Flash is the binding constraint.** The app partition is `0xe9e00` = **957,952 bytes** and the
image runs at roughly 97%. Before adding any library, measure it:
`west build -d build-clip/clip -t rom_report`. This single number explains most of the
architecture — why Bluetooth is compiled out, why the JSON parser in `health.c` is hand-rolled,
why the nRF7002 firmware patch lives in a flash partition instead of the image.

**Static RAM is at roughly 92%.** `-t ram_report`. Any buffer over ~1 KB goes on the **heap**
(`k_malloc`/`k_free`), never on a thread stack. This firmware has crashed three times from stack
sizing.

**Measure stacks, do not reason about them.** Anything that performs a TLS handshake needs
≥12 KB. `http_upload.c` uses 14,336 with TLS; `health.c` uses 12,288. Both numbers were arrived
at by crashing first.

**Route side effects through `clip_event.c`.** Never call `audio_*` directly — not from an AT
handler, not from a button, not from a server command. Going through the event system is what
keeps the state machine, display and haptics coherent.

**Zero compiler warnings.** Fix them before committing.

**Diff the generated `.config` after any large addition.** Kconfig defaults cascade in ways
nobody documents — three separate bugs in this project's history came in that way. See
[Kconfig discipline](#kconfig-discipline).

**Do not rename the Zephyr module.** `zephyr/module.yml` says `name: respeaker_clip`, and
`boards/seeed/clip/Kconfig.sysbuild` references `$(ZEPHYR_RESPEAKER_CLIP_MODULE_DIR)`. A rename
expands that to the empty string, which may make NCS silently substitute an auto-generated
network-core signing key and break OTA for every shipped unit.

---

## Commit rules

- No `Co-Authored-By` lines.
- Zero warnings.
- Commit messages carry hardware evidence when a change was verified on a device — a hash, a byte
  count, a log line. That convention is why the history is usable; keep it.

---

## Build and flash

```sh
source ~/ncs/v3.3.0/zephyr/zephyr-env.sh     # SDK may also live at /opt/nordic/ncs/v3.3.0
export ZEPHYR_EXTRA_MODULES=$(pwd)           # MUST be an env var, not -D: Kconfig
                                             # discovers modules before CMake exists
west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip
```

Board identifier is `clip/nrf5340/cpuapp` — not `respeaker/...`.

**Production image** (console off, needed for any current measurement):

```sh
west build --build-dir build-clip-prod --board clip/nrf5340/cpuapp applications/clip -- \
  -DSNIPPET_ROOT=$(pwd)/applications/clip -DSNIPPET=production
```

`--pristine` after any change to Kconfig, devicetree, sysbuild, partitions or the board. If a
build directory came from another machine, **delete it** — `--pristine` does not clear the dead
absolute paths in `CMakeCache.txt`, and the symptom is the self-contradicting
`No board named 'clip' found. Did you mean: clip`.

### Installing firmware — USB only, no probe

There is no J-Link on this bench, which is an advantage: every flash exercises the real field
update path.

```sh
# 1. Enter MCUboot recovery (or send AT+DFU, or hold the button while plugging in)
python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',1200); time.sleep(0.5); s.close()"

# 2. Upload the signed app image, and reset
nrfutil mcu-manager serial image-upload \
  --firmware build-clip/clip/zephyr/zephyr.signed.bin --serial-port /dev/ttyACM0
nrfutil mcu-manager serial reset --serial-port /dev/ttyACM0
```

- In recovery **two ports appear**; SMP answers on **vcom 0** (the lower number). The other gives
  `Timeout occured`. `nrfutil device list` distinguishes the modes: `reSpeaker Clip` vs
  `reSpeaker Clip DFU`.
- Upload `zephyr.signed.bin`, **not** `merged.hex` — the latter contains the bootloader and only
  goes in over a probe.
- Confirm with `image-list`: the slot-0 hash changing is the only proof the upload landed.
- Long operations need a long read window: `AT+DELETE=all` over ~100 sessions takes >30 s.
- `west flash --reset` does not work on this board; use `west flash && nrfutil device reset`.

### Provisioning, by cable, once per device

```
AT+STACFG="<ssid>","<psk>"     WiFi credentials
AT+UPCFG="<host>",<port>       upload endpoint
AT+KEYCFG=<32 hex>             at-rest audio encryption key
AT+TIME=<epoch>                clock — there is no autonomous time source
```

The fleet CA goes on the card as `/SD:/ca.pem` (copy over USB MSC), and `nrf70.bin` **must** be on
the card before first boot or WiFi never comes up.

### Build variants

| Variant | How | Posture |
|---|---|---|
| Default | no extra args | TLS on, BLE off, AP off — **this is the shipping posture** |
| Production | `-DSNIPPET=production` | as above, console off, ~170 µA idle target |
| Dev radio | `-DEXTRA_CONF_FILE=applications/clip/overlay-dev-radio.conf` | BLE + AP back, **TLS off**. Bench only — mark any device that receives one and reflash before returning it |

TLS (~38 KB) and BLE (~15 KB) do not both fit. The removal of BLE was a **security decision**, not
a space one: if space appeared tomorrow, BLE stays out.

---

## Never suggest

- Bluetooth as a transport, an AT channel, or an OTA path. Removed deliberately.
- WiFi AP mode, `ClipAP_XXXX`, or a static `192.168.4.1`.
- The UDP AT channel as a control path. `wifi_udp.c`/`transport_udp.c` are compiled out
  (`CLIP_UDP_TRANSPORT` defaults `n`) because the server answered `AT+FACTORY` and `AT+FORMAT` to
  anyone on the network. UDP survives only as a bench file-transfer path.
- `applications/clip/overlay-tls.conf` — it contains no config at all and is being deleted.
- `mobile/`, `tests/ble_test.py`, `tests/otp/`, `docs/whitepaper.md` — the first is obsolete, the
  rest do not exist.
- Putting the nRF7002 firmware patch back into the application image. It was there, it cost 87 KB
  of a 936 KB slot, and that is what made TLS impossible.

---

## Architecture

Event-driven. One state machine, one real-time thread, several work queues.

**States:** `UNINITIALIZED → IDLE → RECORDING → PAUSED`, plus `ERROR` and `OTA`.
`TRANSMITTING` and `WIFI_SYNC` exist in the enum but are **unreachable** in this build.

**The spine — one recording, end to end:**

```
button (own thread) → clip_event.c → audio.c → audio_crypto.c → storage.c → http_upload.c
```

`applications/clip/src/`, grouped by role:

| Role | Files |
|---|---|
| Lifecycle | `main.c`, `clip_event.c` (state machine, all side effects), `config.c` |
| Audio | `audio.c` (the one real-time thread, prio 0, 32 KB stack), `audio_crypto.c` |
| Storage | `storage.c`, `upload_registry.c`, `transfer.c` (legacy pull engine) |
| Control | `at_server.c`, `at_commands.c` (40 commands), `transport.c`, `usb_cdc.c` |
| Network | `wifi.c`, `http_upload.c`, `health.c`, `mtls.c`, `ca_builtin.c`, `nrf70_fw_provision.c` |
| UI, power | `display.c`, `icons.c`, `battery.c`, `button.c`, `haptic.c` |
| Stubs | `ble_stub.c`, `udp_stub.c` — empty surfaces swapped in by `CMakeLists.txt` when the real implementation is compiled out |

**Threads and stacks:** audio 32 KB · upload work queue 14,336 (TLS) · heartbeat 12,288 ·
AT server 8 KB · main 6 KB · wifi STA work queue 6 KB · transfer 4 KB · display 2 KB · button 512 B.

**Responses go back out the channel they came in on** — `at_server.c` routes by the queued item's
`transport_type`, not through `transport_get_active()` (which returns `NULL` in this build).

**Health heartbeat.** Every 300 s the device POSTs its state and reads a **whitelisted** command
list from the response (`stop_recording`, `start_recording`, `upload_now`, `wipe`, `reboot`,
`health_now`). The device asks; nothing tells it. There is no listening port.

**nRF7002 firmware patch.** Lives in the `nrf70_wifi_fw` partition at `0x7C0000` on the **external
SPI flash**, not the SD card. `nrf70_fw_provision.c` reads a 20-byte header from the card at every
boot and only rewrites the 128 KB partition on mismatch. During WiFi operation the SD card is not
involved.

---

## Contracts you cannot casually change

Devices in the field carry these.

| Contract | Where | Why frozen |
|---|---|---|
| AT response shape `{"ok":true,"data":…}` / `{"ok":false,"msg":…}` | `at_server.c` | every host client parses it; no numeric error codes |
| Session ID = exactly 14 digits `YYYYMMDDHHMMSS` | `storage.c` | validated at every path builder; physical FAT paths are never exposed |
| SD layout `/SD:/REC/YYYYMMDD/HH/MM/SS/` + `session.json`, 100 files per subdir | `storage.c` | date-bucketed to keep FAT dirs small; field cards must still mount |
| BPE2 container (18-byte header + per-flush AES-128-GCM chunks) | `audio_crypto.c`, `applications/clip/tests/tools/bpin_decrypt.py` | recorded audio must stay decryptable; the Python tool is the spec |
| Upload ledger `/SD:/UPLOADED.TXT` | `upload_registry.c` | fails **open** by design — unreadable means re-upload |
| `lfs_storage` at `0x130000` | `pm_static_clip_nrf5340_cpuapp.yml` | chosen so existing device settings survive upgrades |
| TLS credential tags: CA = 42, client = 43 | `mtls.c` | Zephyr indexes by tag; reusing one overwrites the other |

---

## Known pitfalls

- **`%llu` is not supported.** Zephyr's minimal printf prints `"lu"` literally. Use `%u` with an
  `(unsigned int)` cast.
- **`send()` may accept fewer bytes than you gave it.** A partial write is not an error. Loop
  until the chunk is drained, or you silently under-deliver against `Content-Length` and the
  failure surfaces as an unrelated `ETIMEDOUT`.
- **The TLS layer returns its error code directly and does not set `errno`.** Reading `errno`
  after a failed `zsock_connect()` on a TLS socket gives a misleading answer.
- **Server certificates must be EC P-256 and TLS 1.2.** The device negotiates ECDHE-ECDSA only;
  an RSA certificate or a TLS 1.3-only server fails the handshake without saying why. The cert's
  `subjectAltName` must carry the endpoint address.
- **FAT directory order is not chronological.** Session listing uses a cached sorted buffer
  invalidated on mutation.
- **Thread safety across AT and transfer.** Use volatile flags (e.g. `transfer_cancel_requested`).
- **Corrupt settings boot loop.** A damaged `/lfs/settings/run` blocks `settings_load` ~40 s; a
  watchdog wipes the file and reboots after `CLIP_SETTINGS_LOAD_TIMEOUT_MS`.
- **Logs go to the SD card** (`/SD:/LOG`, rotating). This is the only window into a device that
  has stopped responding.
- **Do not enable `AT+LOG` while USB MSC is mounted** — two writers on the same FAT volume. It has
  taken the device down.
- **Crystal load capacitors come from Kconfig, not the devicetree.** `clip_xo_cap_init()` in
  `main.c` runs at `POST_KERNEL` and overwrites what SoC init wrote from the DTS, using
  `CLIP_HFXO_CAPVALUE` and `CLIP_LFXO_INTCAP`. The DTS values are not what the chip runs.

### Known-wrong things in the current tree

Do not "fix" these casually; each has a task in the backlog.

- **The SD idle power-gate never executes.** `CONFIG_CLIP_LOG_FS_DEFAULT_ON=y` is set explicitly
  in `prj.conf`, which beats the Kconfig default the production snippet relies on, so
  `clip_sd_busy()` is true from boot and `storage_idle_poweroff()` always returns `-EBUSY`.
- **CPU boost leaks across `PAUSED`.** Acquired in `audio.c`, released only on stop paths — the
  core stays at 128 MHz through a pause.
- **RPU power-save is fully disabled** (`CONFIG_NRF_WIFI_LOW_POWER=n`) as a deliberate workaround
  for an association bug. The STA link is never torn down automatically.
- **`ble_is_bonded()` always returns false**, so the display cannot reach its sleep path and sits
  in a pairing screen refreshing once per second.
- **`prj.conf` has 22 inert `CONFIG_BT_*` lines** and four contradictory duplicate pairs.

---

## Kconfig discipline

Asking for a large feature is asking for a catalogue, not one thing. When adding one:

1. Build, then **diff the generated `.config`** against the previous one.
2. Justify every new line. The first TLS attempt overflowed by 17,688 bytes largely on things
   never used — PSK key exchanges, CRL and CSR parsing, session serialization.
3. Watch for **ranges beating defaults**. A `default 2` was silently clamped to 3 by a range
   imposed elsewhere; the fix was clearing the intermediate symbol
   (`MCUBOOT_WIFI_PATCHES_HAS_UPDATE_SLOT`) in `boards/seeed/clip/Kconfig.sysbuild`.
4. Watch for **defaults activating when a dependency changes**. Removing `NRF70_AP_MODE` let
   `NRF_WIFI_LOW_POWER` default itself on, which slept the radio mid-association.
5. `SB_CONFIG_*` (sysbuild) and `CONFIG_*` (app) are different namespaces. Sysbuild propagates and
   overrides — setting `CONFIG_NRF_WIFI_PATCHES_EXT_FLASH_STORE` in `prj.conf` alone does nothing.
6. Snippet config is broadcast to **every** image including MCUboot, where `CLIP_*` symbols do not
   exist and an unknown symbol is an abort, not a warning.

---

## Board and sysbuild

Every app targeting this board builds as a **sysbuild** (MCUboot + app core + network-core radio)
with no per-app sysbuild config, because `boards/seeed/clip/Kconfig.sysbuild` supplies the
defaults. A sample is `CMakeLists.txt` + `prj.conf` + `src/` and still boots under the signed
bootloader.

Key board files:
- `clip_nrf5340_cpuapp.dts` + `clip-pinctrl.dtsi`, `clip-cpuapp_partitioning.dtsi`,
  `clip-shared_sram.dtsi`, `nrf70_common*.dtsi`
- `pm_static_clip_nrf5340_cpuapp.yml` — the **authoritative** partition map. The DTS also declares
  external-flash partitions; Partition Manager wins, and nothing enforces that they agree.
- `Kconfig.sysbuild` — bootloader, netcore, WiFi driver, and both signing keys
- `sysbuild/` — shared MCUboot config plus the signing keys

**Signing keys — two separate problems, neither yet resolved.** `root-rsa-2048.pem` is MCUboot's
*published example key*, so signature verification currently protects nothing: anyone with a cable
can install firmware this bootloader accepts. `b0-ecdsa-p256.pem` is a real private key committed
to this repo whose public hash is in immutable boot on shipped units — it **cannot be rotated on
existing hardware**. Rotating the MCUboot key is irreversible for fielded units and needs a staged
plan (dev board → bridge image signed with the old key → sacrificial unit → production).

Hardware: nRF5340 dual-core · nRF7002 WiFi (QSPI) · NPM1300 PMIC + nRF Fuel Gauge (custom "240" /
HSZ 362123 cell model, 170 mAh) · CH1115 OLED 88×48 (I2C2) · PDM mic pair (PDM0) · microSD
(SPI4, SDHC-SPI) · PY25Q64H 8 MB SPI flash (SPI3) · USB CDC + MSC · user button P1.15.

Out-of-tree drivers live in `drivers/input/` (button) and `drivers/display/` (CH1115), with
bindings in `dts/bindings/`. Vendored libraries in `lib/`: opus, speexdsp, lua (unused),
`clip_usb_dfu` (the 1200-baud trigger, board-level so every app is recoverable).

---

## MCUboot patches

Source lives in the NCS tree, not here. Five patches in `patches/mcuboot/` add VBUS-gated
recovery, an OLED UI in the bootloader, upload and swap progress hooks, and custom mcumgr
commands. Workflow — edit the NCS source, build, verify on hardware, export the diff back — is in
`patches/mcuboot/README.md`. Requires `--pristine`. **The bootloader on shipped units cannot be
replaced over USB**, only over SWD, so a bootloader change applies to future production only.

---

## Repository map

| Path | What |
|---|---|
| `applications/clip/` | the product firmware |
| `applications/clip/tests/tools/` | host side: `bpin_http_receiver.py` (the receiving service + fleet panel), `panel_admin.py` (cable provisioning, localhost only), `bpin_decrypt.py` (BPE2 reference decryptor), `decode_opus.py` (adds the Ogg container the device omits) |
| `applications/clip/tests/audio_test/` | ASR-scored audio quality harness — **use it before and after any codec or DSP change** |
| `boards/seeed/clip/` | board support package |
| `drivers/`, `lib/`, `dts/`, `include/`, `sysbuild/`, `zephyr/module.yml` | module wiring |
| `patches/mcuboot/` | bootloader patches |
| `tests/` | standalone firmware images, flashed *instead of* the product, over SWD: `clip` (HW bench + `lfxo`/`hfxo` crystal tuning shell), `dtm` (BLE RF cert), `wifi_radio` (nRF70 RF cert), `battery_cycle` (charge/discharge cycler), `re` (older duplicate) |
| `samples/` | one-idea reference apps. **This is where to prototype** — you cannot try things in a 97%-full image |
| `sdk/` | installable Python package `clip` (BLE and UDP transports only — no USB CDC transport yet, so it cannot talk to this firmware) |
| `mobile/` | obsolete Flutter/Android/iOS BLE SDKs, slated for deletion |

Nothing in `tests/` is a unit test. Host-side tests are in `sdk/tests/` and
`applications/clip/tests/tests/`.

---

## Documentation

| Doc | Status |
|---|---|
| `SETUP.md` | **current** — build, install, run the service, provision, verify. Its final section is the most useful page in the repo |
| `docs/architecture.md` | stale — describes the triple-transport device |
| `docs/protocol.md` | stale — titled "BLE AT Protocol Specification" |
| `docs/udp_protocol.md` | stale — asserts the device is an access point |
| `docs/usb_dfu.md` | mostly current |
| `docs/audio_quality_standard.md` | current, but duplicated by a longer copy under `applications/clip/tests/audio_test/` |
| `docs/requirements.md`, `docs/custom_app_guide.md`, `docs/development.md` | Seeed-era, being removed |
| `docs/release_notes/` | v0.0.5–v0.1.0; nothing for the current 0.2.0 |

**Treat the source as authoritative over any of these.** Code comments in
`applications/clip/src/` and the help text in `applications/clip/Kconfig` are the best
documentation in the project — they record *why*, including failed attempts.

Numbered design documents cited in the source ("Doc 09", "Doc 13", "Doc 23") live **outside this
repo**, in a separate `docs-respeaker/` tree on the development machine.
