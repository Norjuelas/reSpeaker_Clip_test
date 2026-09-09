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

**Flash is the binding constraint, and there are two denominators — use the honest one.**
Partition Manager gives the `app` partition `0xe9e00` = 957,952 B, but MCUboot reserves
24,576 B of that (header + trailer/swap status), so the linker only offers **933,376 B**.
The image is at **98.8%** of what it can actually use — about **11 KB free**, not the ~37 KB
the "96%" figure in older commit messages implies. Before adding any library, measure it:
`west build -d build-clip/clip -t rom_report`. This single number explains most of the
architecture — why Bluetooth is compiled out, why the JSON parser in `health.c` is hand-rolled,
why the nRF7002 firmware patch lives in a flash partition instead of the image.

**Static RAM is at roughly 95%.** `-t ram_report`. About 22 KB free of 440 KB. Any buffer over ~1 KB goes on the **heap**
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
- `applications/clip/overlay-tls.conf` — **deleted**. It contained no configuration at all, so
  the "with TLS" and "without TLS" recipes in `SETUP.md` produced identical firmware. TLS is on
  by default; `overlay-dev-radio.conf` is the one you pass to turn it *off*.
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
| **WiFi MAC = `0xB2` + last 5 bytes of `FICR.DEVICEID`** | `wifi_apply_stable_mac()` in `wifi.c` | corporate networks whitelist these addresses; changing the derivation silently locks every fielded unit out of every network that filters by MAC |

---

## The WiFi MAC address — do not change this

Corporate and store networks admit these devices by MAC allow-list, so the address is a
contract with every network that has ever whitelisted a unit.

**Where it comes from.** `wifi_apply_stable_mac()` in `wifi.c`, called on every radio bring-up
just before `net_if_up()` (it can only be set while the interface is down):

```c
len = hwinfo_get_device_id(chip_id, sizeof(chip_id));   /* FICR.DEVICEID, 8 bytes */
mac[0] = 0xB2;                                          /* B de B-Pin; U/L=1, I/G=0 */
memcpy(&mac[1], &chip_id[len - 5], 5);                  /* últimos 5 del chip id */
net_if_set_link_addr(iface, mac, sizeof(mac), NET_LINK_ETHERNET);
```

```
chip id  62 51 8A 2B 20 63 EA E0
                      └─────────┘
MAC      B2 :2B :20 :63 :EA :E0
```

`FICR.DEVICEID` is burned by Nordic at manufacture and is read-only, so the address survives
reboots, flat batteries, `AT+FACTORY`, and reflashing. **Verified on hardware 2026-09-08:**
identical before and after a full power cycle, and the phone hotspot's client list showed the
same address on air, with the same DHCP lease reissued.

**`0xB2` is not arbitrary.** Bit 1 set = locally administered ("assigned by software, not an
IEEE-registered vendor prefix"); bit 0 clear = unicast. Both are required for a valid station
address. Some corporate tooling flags non-vendor MACs — that is expected, not a fault.

**Why it must not change.** Alter the prefix or which bytes of the chip ID are used and every
allow-list entry, at every site, stops matching — with no error anywhere. Just devices that no
longer connect, and nothing in any log to say why.

**A failure mode that breaks filtering.** If `hwinfo_get_device_id()` ever returns fewer than
6 bytes, the function logs `"Sin chip id: la MAC queda aleatoria por arranque"` and returns
without setting anything — so the nRF70 driver's own address wins, and that build has
`CONFIG_WIFI_RANDOM_MAC_ADDRESS=y`: a **new random MAC every boot**. Under MAC filtering that
presents as a unit which connects once and is never admitted again. Check for that log line
first if a device becomes intermittently unwelcome on a network that worked before.

**Do not "fix" the random-MAC Kconfig.** `CONFIG_WIFI_RANDOM_MAC_ADDRESS=y` looks wrong and is
not: it is only the driver's default before our address is applied, and the application's
override wins. Reasoning from those symbols alone led to a whole wrong diagnosis on
2026-09-08 that a five-minute hotspot test disproved. If you need to know what a device
actually transmits, connect it to a phone hotspot and read the client list — do not infer it.

**Collecting MACs for provisioning:** `AT+DEVICE` returns an **empty** `mac` until the radio has
been brought up at least once, because the address is applied in the bring-up path rather than
at boot. Run `AT+STA=on` first or you will collect blanks. The `chip` field is always present.

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
- **A guard that reads as obviously correct can be exactly backwards.** `wifi_sta_off()` had
  `if (!sta_associated) return 0;` — *"nothing connected, nothing to disconnect"*. But
  **"interface up, not associated" is precisely the state where the radio must be re-powered**,
  and `net_if_down()` is the only path in the firmware that reaches `rpu_pwroff()` and cuts
  BUCKEN/IOVDD. So after a failed association nothing ever re-powered the chip and the device
  stayed off the network until someone rebooted — which was the whole of "H2". Cost: weeks,
  attributed to Nordic's driver. Association gate went **1/10 → 20/20** when it was removed.
- **`AT+STA=on` takes a radio lease that never expires** — it holds until `AT+STA=off`. A device
  left in that state is silently converted to always-on: the radio never sleeps, ~41 mA, roughly
  4 hours of life doing nothing, and **it looks perfectly healthy on the panel**. This invalidated
  two full battery measurements before `leases` was published in the heartbeat. In a beat,
  `leases` can never be 0 (the beat is sent from inside a window) — what tells the truth is
  **1 versus 2**. Always `AT+STA=off` before handing the device over for a test.
- **Mounting the SD card on the host stops the radio associating**, not just uploads. Desktop
  auto-mount grabs it on every USB re-enumeration, and two whole association gates were
  invalidated before this was spotted. `gsettings set org.gnome.desktop.media-handling automount false`.
- **The fuel gauge cannot see the radio.** Any current figure from `battery_ua` excludes the
  nRF7002. Get real consumption from the SoC slope between heartbeats, never from `battery_ua`.
- **Crystal load capacitors come from Kconfig, not the devicetree.** `clip_xo_cap_init()` in
  `main.c` runs at `POST_KERNEL` and overwrites what SoC init wrote from the DTS, using
  `CLIP_HFXO_CAPVALUE` and `CLIP_LFXO_INTCAP`. The DTS values are not what the chip runs.

### Known-wrong things in the current tree

Do not "fix" these casually; each has a task in the backlog.

- **RPU power-save is fully disabled** (`CONFIG_NRF_WIFI_LOW_POWER=n`), and this is now a
  *measured* decision, not an inherited workaround. Three arms on hardware, ten cold
  associations each: power-save off **10/10**; on **0/12**; on with `NRF_WIFI_RPU_RECOVERY`
  forced off **0/10**. Power-save breaks association on this board by itself. Note the
  dependency: `RPU_RECOVERY` has `depends on NRF_WIFI_LOW_POWER` and `default y`, so the two
  symbols move together unless you stop them.
- **`wifi_load_estimate_a()` reads `wifi_ap_is_running()`**, which is compiled out — so the
  fuel-gauge WiFi compensation **never applies** and SoC reads high whenever the radio is on.
  Quantified: on an idle discharge the gauge reported 1.1 mA while the real drain was 42.5 mA.
  The nRF7002 taps VBAT upstream of the PMIC's sense resistor, so the gauge is structurally
  blind to it. Its constants (59/99 mA) are uncalibrated AP-era guesses — calibrate before
  enabling, or the gauge will read low instead.
- **`prj.conf` has 22 inert `CONFIG_BT_*` lines** and four contradictory duplicate pairs.
- **The network core still runs a full Bluetooth controller** (`ipc_radio`: `CONFIG_BT=y`,
  `MPSL`, `MPSL_CX`) that the app core never talks to. Not a casual removal — that image is
  signed by `b0n` and has its own OTA path — and `NRF70_SR_COEX` on the app core is the *other
  half* of the same two-core mechanism, so removing one alone is untested and suspect.

Fixed since, do not re-report: the SD idle power-gate (`CLIP_LOG_FS_BOOT_WINDOW_S` retires the
FS log backend so `clip_sd_busy()` can go false), CPU boost leaking across `PAUSED`, and the
STA link never being torn down (the reference-counted lease in `wifi.c` does that now).

**And one thing that was never true:** the display does *not* refresh at 1 Hz during a
recording. `display_thread_fn` waits `K_SECONDS(1)` only in `STATUS_BAR` and `PAIRING_GUIDE`;
in `REC_DOT` — where a recording spends all its time after the first 5 s — it waits
`K_FOREVER` and pushes no frames. The cost during a recording is the **panel being powered**
(`oled_reg` is `regulator-boot-on` and never disabled), which is a different fix.

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
