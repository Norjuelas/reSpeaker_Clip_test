# Build & Flash Reference

## Environment Setup

```sh
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)   # MUST be env var, not CMake
```

`ZEPHYR_EXTRA_MODULES` must be an environment variable because Kconfig module discovery runs before CMake configuration.

## Building

### Main Application

```sh
# Incremental build
west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip

# Clean (pristine) build — required after Kconfig/device-tree changes
west build --build-dir build-clip --pristine --board clip/nrf5340/cpuapp applications/clip
```

**Board identifier**: `clip/nrf5340/cpuapp` (NOT `respeaker/...`)

### Samples

```sh
west build --build-dir build-hello --pristine --board clip/nrf5340/cpuapp samples/hello_world
```

### Build Snippets

```sh
# Apply a snippet (e.g. production)
west build ... -- -DSNIPPET_ROOT=applications/clip -DSNIPPET=production
```

Snippets live in `applications/clip/snippets/`. Each has a conf file, optional overlay, and `snippet.yml`.

## Flashing

```sh
# Flash + reset (west flash --reset does NOT work on this board)
west flash --build-dir build-clip && nrfutil device reset

# First-time flash or after MCUboot changes — erases ALL flash (both cores)
west flash --build-dir build-clip --recover && nrfutil device reset
```

Use `--recover` when: readback protection is enabled, flashing MCUboot itself, or you see "Network core access port is protected".

## Serial Output

```sh
minicom -D /dev/ttyACM0 -b 115200
```

## Output Firmware Files

| File | Description |
|------|-------------|
| `build-clip/merged.hex` | MCUboot + Application core (combined) |
| `build-clip/merged_CPUNET.hex` | Network core firmware |
| `build-clip/dfu_application.zip` | OTA update package (BLE/USB DFU) |
| `build-clip/zephyr/zephyr.hex` | App only (when MCUboot disabled) |

## Firmware Packaging Script

```sh
VERSION=$(grep APP_VERSION_STRING build-clip/clip/zephyr/include/generated/zephyr/app_version.h | cut -d'"' -f2)
mkdir -p output/$VERSION

cp build-clip/merged.hex output/$VERSION/
cp build-clip/merged_CPUNET.hex output/$VERSION/
cp build-clip/dfu_application.zip output/$VERSION/clip-$VERSION-ota.zip
```

Version string comes from `applications/clip/CMakeLists.txt` (`VERSION`/`APP_VERSION_*`).

## Common Build Errors

| Error | Cause | Fix |
|-------|-------|-----|
| `region FLASH overflowed by N bytes` | Missing `pm_static_clip_nrf5340_cpuapp.yml` symlink → dynamic (smaller) partition layout | Add the symlink (see `mcuboot.md`) |
| `unknown SB_CONFIG_*` variable | Zephyr env not sourced | Run `source .../zephyr-env.sh` + `export ZEPHYR_EXTRA_MODULES=$(pwd)` |
| "Signature verification failed" (at boot) | Build signing key ≠ key baked into on-device MCUboot | Reference `samples/_mcuboot/sysbuild/root-rsa-2048.pem`; `west flash --recover` |
| Boot loop | App image too large for slot | Enable `CONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE=y` + `CONFIG_LTO=y` |
| "Network core access port is protected" | Readback protection on | `west flash --recover` |

## Compiler Warning Policy (from CLAUDE.md)

Code must compile with **zero warnings**. Fix all compiler warnings before committing. Do not add `Co-Authored-By` lines to commits.

## Key Config Files

- `applications/clip/prj.conf` — main Kconfig (358 lines, all subsystems)
- `boards/seeed/clip/clip_nrf5340_cpuapp.dts` — device tree
- `applications/clip/CMakeLists.txt` — build + version
