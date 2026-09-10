# SD recording storage

`storage.c` owns FATFS at `/SD:` and treats the physical directory layout as an
implementation detail. AT and SDK clients must use a 14-digit session ID and
logical `NNNN.opus` chunk names only.

## Physical layout

For session `20260716022113`, current storage is:

```text
/SD:/REC/20260716/02/21/13/
  session.json
  marks.bin
  0/0001.opus
  0/0002.opus
  1/....opus
```

The trailing `SS` directory is the session root. Chunk group is
`(chunk_index - 1) / CONFIG_CLIP_STORAGE_FILES_PER_GROUP`; grouping prevents a
large FAT directory from slowing recording and listing. Never reconstruct paths
outside `storage_build_chunk_path()` or the storage APIs.

## Lifecycle and metadata

`storage_create_session()` creates metadata and directories. Audio opens/writes
chunks through `storage_create_file()` and `storage_write_file()`; closing a
chunk signals transfer readiness. `storage_close_session()` finalizes
`session.json`.

`session.json` stores file count, byte count, synchronized file count, channel
count, sample rate, and mode. Use it before walking directories; a directory
scan is fallback/recovery logic.

## Low-power lifecycle

When storage is idle and not recording, transferring, USB-MSC mounted, or FS
logging, firmware unmounts FATFS, deinitializes the disk, suspends SPI4, parks
CS low, and disables NPM1300 LDO2. New storage access must use
`storage_ensure_mounted()`; never assume LDO2 or SPI4 is ready after idle.

**That rule has been broken once, in production, and it cost 50 minutes of
undelivered audio.** `http_upload.c`'s periodic sweep read the card without
mounting it first, so `storage_list_sessions()` returned `-EINVAL`, both of its
loops iterated zero times, and the device published `pending_files: 0` while
audio sat unsent. It was self-reinforcing: with the card down nothing mounted
it, so nothing uploaded until a reboot or a new recording. Fixed in `1780026`.

Two consequences worth carrying:

- **`storage_ensure_mounted()` is the only thing that re-arms the idle timer.**
  It is the single call site of `sd_activity_cb()` (`storage.c:466`). Reads and
  writes do not re-arm it. What keeps the card alive during a recording is not
  the writing — it is `clip_sd_busy()` returning true.
- **Anything that holds the card open across several operations must appear in
  `clip_sd_busy()`**, or the rail can drop mid-read. A sweep runs with the state
  machine in `IDLE`, so none of the original conditions saw it; the fix added
  `http_upload_is_sweeping()`. Use a lock-free atomic for such a predicate:
  `clip_sd_busy()` runs with `sd_lifecycle_mutex` held, so taking a subsystem
  mutex there inverts the lock order.

**A failed listing is not an empty one.** Treat `found < 0` as "unknown", report
it, and leave the last known `pending_files` alone. Publishing a zero is what
made the field failure invisible for three consecutive windows.
