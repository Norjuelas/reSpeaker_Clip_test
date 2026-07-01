# WiFi UDP Transfer Reference

## WiFi AP Configuration

| Parameter | Value |
|-----------|-------|
| SSID | `ClipAP_XXXX` (last 4 hex of chip ID) |
| Password | `12345678` (default; random after first BLE pairing) |
| IP Address | `192.168.4.1/24` |
| UDP Port | `8089` |
| Band | 5GHz channel 36 |
| Max Clients | 1 |
| Security | WPA2 |

WiFi is NOT started at boot (`CONFIG_NRF_WIFI_IF_AUTO_START=n`). Must start via `AT+WIFI=on`. Auto-off after 3 min if no client connects (`CONFIG_CLIP_WIFI_TIMEOUT_MS=180000`). Cannot start WiFi while recording.

Source: `docs/udp_protocol.md`, `applications/clip/src/wifi.c`, `applications/clip/src/wifi_udp.c`.

## Frame Types

| Type | Hex | Direction | Description |
|------|-----|-----------|-------------|
| DATA | `0x01` | S→C | File data + per-frame CRC32 |
| FILE_ACK | `0x03` | C→S | File CRC verification result |
| FILE_START | `0x10` | S→C | Begin file (filename + size) |
| FILE_END | `0x11` | S→C | End file (full-file CRC32) |
| TRANSFER_DONE | `0x12` | S→C | All files complete |
| AT_RESP | `0x20` | S→C | AT command response (JSON) |
| HEARTBEAT | `0x30` | Both | Keepalive |

All multi-byte fields are **little-endian**.

## Frame Formats

### DATA (0x01) — 9-byte header

```
[type:1][seq:2 LE][len:2 LE][crc32:4 LE][payload:N]
```
- seq: per-file sequence (starts at 0)
- len: max 1024 bytes
- crc32: IEEE CRC32 of payload only

### FILE_ACK (0x03) — 2 bytes

```
[type:1][result:1]
```
- result: 0x00 = CRC OK, 0x01 = CRC mismatch (retransmit)

### FILE_START (0x10)

```
[type:1][fn_len:1][filename:fn_len][file_size:4 LE]
```

### FILE_END (0x11) — 5 bytes

```
[type:1][crc32:4 LE]
```
- CRC32 of complete file data (all DATA payloads concatenated)

### TRANSFER_DONE (0x12)

```
[type:1][sid_len:1][session_id:sid_len][file_count:4 LE]
```

### AT_RESP (0x20)

```
[type:1][len:2 LE][json_data:N]
```

### HEARTBEAT (0x30) — 5 bytes

```
[type:1][timestamp:4 LE]
```
- timestamp: uptime in milliseconds

## BLE vs WiFi UDP Comparison

| | BLE | WiFi UDP |
|---|---|---|
| AT command | BLE Write char | UDP plain text `"AT+XXX\n"` (trailing `\n` required) |
| AT response | BLE Notify (JSON) | AT_RESP frame (`0x20`) |
| DATA header | 5 bytes | 9 bytes (+4 CRC32) |
| Per-frame CRC | None (link layer) | IEEE CRC32 per frame |
| FILE_ACK | None | Yes (CRC mismatch → retransmit) |
| Heartbeat | None | 5s interval, 30s timeout |
| Throughput | ~15 KB/s | ~500 KB/s |

## Protocol Flow

```
Client                          Device (192.168.4.1:8089)
  |                               |
  |─ "AT+DOWNLOAD=session\n" ───>|
  |<─ AT_RESP (JSON) ────────────|
  |<─ FILE_START(fn, size) ──────|
  |<─ DATA(seq=0, crc, payload) ─|  fire-and-forget
  |<─ DATA(seq=1, crc, payload) ─|  (no per-frame ACK)
  |<─ ...                        |
  |<─ FILE_END(crc32) ───────────|
  |─ FILE_ACK(OK/NACK) ────────>|  CRC mismatch → retransmit
  |  ... (next file) ...         |
  |<─ TRANSFER_DONE(sid, count) ─|
```

## Transfer Strategy

- **Fire-and-forget**: DATA frames sent without per-frame ACK
- **Per-frame CRC32**: Each DATA frame includes CRC of its payload; corrupted frames silently dropped
- **Full-file CRC32**: Server accumulates CRC across all DATA frames; FILE_END carries the accumulated CRC
- **CRC on confirmed send only**: If `sendto()` fails, frame data NOT included in accumulated CRC
- **File-level retransmit**: On NACK or timeout, retransmit entire file (up to 10 retries)
- **Thread-safe cancel**: volatile flag checked in transfer thread

## CRC32 Algorithm

IEEE Ethernet CRC32 (polynomial 0xEDB88320, reflected).
- Server (Zephyr): `crc32_ieee_update(0, data, len)` — Zephyr handles `~crc` internally
- Client (Python): `binascii.crc32(data)` (standard Python CRC32)
- Accumulation: `crc = crc32_ieee_update(crc, chunk, len)` (server) / `crc = binascii.crc32(chunk, crc)` (client)

## Timing Parameters

| Parameter | Default | Kconfig |
|-----------|---------|---------|
| Heartbeat interval | 5000ms | `CONFIG_CLIP_UDP_HEARTBEAT_INTERVAL_MS` |
| Connection timeout | 30000ms | `CONFIG_CLIP_UDP_CONNECTION_TIMEOUT_MS` |
| FILE_ACK timeout | 2000ms | Hard-coded |
| FILE_END retries | 3 | Hard-coded |
| WiFi auto-off | 180000ms | `CONFIG_CLIP_WIFI_TIMEOUT_MS` (0 = disabled) |

## Python Tools

```sh
pip install -r applications/clip/tests/requirements.txt

# UDP file sync
python applications/clip/tests/tools/udp_sync.py --session <session_id>
python applications/clip/tests/tools/udp_sync.py --all-sessions

# Recording tool (real-time sync)
python applications/clip/tests/tools/record.py

# UDP terminal
python applications/clip/tests/tools/udp_terminal.py
```

## Known Issues

- `sendto()` returns success even when WiFi TX queue silently drops packets. CRC is only updated after confirmed send; file-level retry handles lost data.
- `%llu` not supported in Zephyr minimal printf — use `%u` with `(unsigned int)` cast for 64-bit values.
