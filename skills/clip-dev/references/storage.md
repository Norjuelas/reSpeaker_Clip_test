# Storage Reference

## SD Card (SPI4)

| Parameter | Value |
|-----------|-------|
| Bus | SPI4 (SDHC-SPI mode) |
| Chip Select | GPIO0.9 |
| Filesystem | FAT32 |
| Mount point | `/SD:` |
| Base recording path | `/SD:/REC/` |
| Write buffer | 4KB (`CONFIG_CLIP_STORAGE_CHUNK_SIZE=4096`) |

Source: `applications/clip/src/storage.c`, `docs/architecture.md` §3.6.

**SPI4 power cycle**: PM device API for SPI4 suspend/resume. CS pin must preserve `GPIO_ACTIVE_LOW` flag. See `feedback_spi4_sd_powercycle.md` in memory.

## Directory Structure

```
/SD:/REC/
├── 20260328120530/              # Session (ID = YYYYMMDDHHMMSS, 14 digits)
│   ├── session.json             # Metadata (channels, sample_rate, duration, files, synced)
│   ├── marks.bin                # Binary bookmarks (4B magic "MRK1" + 4B count + entries)
│   ├── 0001.opus                # Segment files ([2B len][Opus frame]...)
│   ├── 0002.opus
│   └── .transferred             # Empty marker: session fully synced
├── 20260328140000/
│   └── ...
/SD:/LOG/
├── log.000001                   # SD card log persistence (64KB each, max 10 files)
├── log.000002
└── ...
```

## Session Lifecycle

```
storage_create_session() → storage_create_file() → storage_write_frame()* →
storage_close_file() → (repeat for each segment) → storage_close_session()
```

## Session Metadata (session.json)

```json
{
  "id": "20240203100000",
  "duration": 600,
  "files": 30,
  "synced": 15,
  "channels": 2,
  "sample_rate": 16000,
  "mode": "normal"
}
```

- `synced`: files successfully transferred (used for resume logic)
- `mode`: "normal" (stereo) or "enhanced" (mono+DSP)

## File List (files.lst)

Plain text, one filename per line (append-only). Used for efficient session file listing.

## File Numbering

`0001.opus`, `0002.opus`, ... — 4-digit zero-padded with `.opus` extension.
On pause/resume, a new file is created with incremented index.

## Transfer Marker (.transferred)

Empty file created on successful transfer completion. Used by `AT+PURGEABLE` and `AT+PURGE` to identify deletable sessions.

## Bookmarks (marks.bin)

Binary format for efficient storage:

| Field | Size | Description |
|-------|------|-------------|
| Magic | 4B | "MRK1" (or "BMRK") |
| Count | 4B (uint16) | Number of bookmark entries |
| Entry | 78B each | timestamp(4) + offset_sec(4) + file_index(2) + file_offset(4) + note(64, null-terminated UTF-8) |

## LittleFS (External Flash)

64MB SPI flash (PY25Q64H on SPI3) has a LittleFS partition (~6.8MB).

| Mount Point | Purpose |
|-------------|---------|
| `/lfs` | Zephyr Settings backend |
| `/lfs/settings/run` | Settings database |

External flash partitions (from `pm_static_clip_nrf5340_cpuapp.yml`):
- Image-0 Secondary: ~960KB (OTA staging)
- Image-1 Secondary: ~256KB (netcore OTA staging)
- LittleFS: ~6.8MB (settings + OTA patches)

## SD Card Log Persistence

Enabled by `CONFIG_LOG_BACKEND_FS=y`:
- Log directory: `/SD:/LOG/`
- Files: `log.000001` to `log.000010` (64KB each, circular overwrite)
- Level: `LOG_LEVEL_WRN` (warnings + errors only)
- Activated after SD card mount in `clip_init()`

## Key Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_LOG_BACKEND_FS` | y | SD card log backend |
| `CONFIG_LOG_BACKEND_FS_FILE_SIZE` | 65536 | Log file size (bytes) |
| `CONFIG_LOG_BACKEND_FS_FILES_LIMIT` | 10 | Max log files |

## Key Functions

| Function | Purpose |
|----------|---------|
| `storage_create_session()` | Create session dir + session.json |
| `storage_create_file()` | Open new segment file |
| `storage_write_frame()` | Write [2B len][Opus packet] |
| `storage_close_file()` | Close segment, signal transfer thread |
| `storage_close_session()` | Finalize session.json |
| `storage_format_card()` | FAT32 format (AT+FORMAT) |

## Key Source Files

- `applications/clip/src/storage.c` — SD card + FAT32 management
- `applications/clip/include/storage.h` — public API
- `boards/seeed/clip/clip_nrf5340_cpuapp.dts` — SPI4 device tree config

## Known Pitfalls

- **FAT directory order**: NOT chronological. Session listing uses cached sorted buffer invalidated on mutations (DELETE/PURGE).
- **Transfer thread safety**: AT commands and transfer run on different threads. Uses volatile flags for coordination (e.g., `transfer_cancel_requested`).
- **`%llu` not supported**: Zephyr minimal printf on nRF5340 outputs `"lu"` literally. Use `%u` with `(unsigned int)` cast.
