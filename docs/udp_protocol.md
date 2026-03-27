# CLIP UDP Transfer Protocol v2

## Overview

Reliable UDP file transfer protocol for the CLIP embedded device. Designed for
efficiency (sliding window, high throughput) and safety (per-frame CRC32,
selective ACK, automatic retransmission).

**Network**: Device acts as WiFi AP (IP: 192.168.4.1), client connects to UDP
port 8089. All communication is bidirectional on a single socket.

## Frame Types

| Type | Value | Direction | Size | Description |
|------|-------|-----------|------|-------------|
| DATA | 0x01 | S→C | 9+N | File data with seq + per-frame CRC32 |
| ACK | 0x02 | C→S | 5 | Cumulative ACK + selective bitmap + window |
| FILE_START | 0x10 | S→C | 2+N+4 | Begin file transfer |
| FILE_END | 0x11 | S→C | 5 | End file transfer (full-file CRC32) |
| TRANSFER_DONE | 0x12 | S→C | 2+N+4 | All files complete |
| AT_RESP | 0x20 | S→C | 3+N | AT command response (JSON) |
| HEARTBEAT | 0x30 | Both | 5 | Keepalive |

All multi-byte fields are **little-endian**.

---

## Frame Definitions

### DATA (0x01) — Server→Client

Carries a chunk of file data with independent CRC verification.

```
Byte:  [0]   [1]  [2]  [3]  [4]  [5] [6] [7] [8]  [9 ... 9+N-1]
Field: type  seq_lo seq_hi len_lo len_hi crc32 (4 bytes)     data (N)
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | 0x01 |
| seq | 2 | Per-file sequence number (uint16 LE, starts at 0) |
| len | 2 | Data length, max 486 bytes (uint16 LE) |
| crc32 | 4 | IEEE CRC32 of data field (uint32 LE) |
| data | N | Raw file data |

**Header size**: 9 bytes. **Max payload**: 486 bytes. **Max frame**: 495 bytes.

**CRC calculation**:
```
crc = crc32_ieee_update(0, data, len)   // Zephyr (handles ~crc internally)
crc = binascii.crc32(data)                // Python
```
Both produce identical results. Zephyr's function applies `~crc` at start and end internally.

### ACK (0x02) — Client→Server

Cumulative acknowledgment with selective bitmap and flow control window.

```
Byte:  [0]   [1]  [2]  [3]    [4]
Field: type  ack_seq (2)  window  bitmap
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | 0x02 |
| ack_seq | 2 | Cumulative ACK: all frames with seq < ack_seq received (uint16 LE) |
| window | 1 | Available receive buffer in frames (uint8, 0 = pause) |
| bitmap | 1 | Selective ACK: bit i = 1 → frame (ack_seq + i) received |

**Frame size**: 5 bytes (fixed).

**Example**: ack_seq=10, bitmap=0b00001010 means frames 0-9 confirmed,
frame 12 and 14 also received (bitmap bits 2 and 4 set).

### FILE_START (0x10) — Server→Client

```
Byte:  [0]   [1]    [2 ... 2+N-1]  [2+N ... 2+N+3]
Field: type  fn_len  filename       file_size (4)
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | 0x10 |
| fn_len | 1 | Filename length (0-63) |
| filename | fn_len | UTF-8 encoded filename |
| file_size | 4 | File size in bytes (uint32 LE) |

### FILE_END (0x11) — Server→Client

```
Byte:  [0]   [1] [2] [3] [4]
Field: type  crc32 (4)
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | 0x11 |
| crc32 | 4 | IEEE CRC32 of complete file data (uint32 LE) |

Client independently computes CRC over all received DATA payloads and compares.

### TRANSFER_DONE (0x12) — Server→Client

```
Byte:  [0]   [1]    [2 ... 2+N-1]  [2+N ... 2+N+3]
Field: type  sid_len  session_id   file_count (4)
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | 0x12 |
| sid_len | 1 | Session ID length |
| session_id | sid_len | UTF-8 session ID |
| file_count | 4 | Total files transferred (uint32 LE) |

### AT_RESP (0x20) — Server→Client

```
Byte:  [0]   [1] [2]  [3 ... 3+N-1]
Field: type  len (2)   json_data
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | 0x20 |
| len | 2 | Response length (uint16 LE) |
| json_data | len | UTF-8 encoded JSON |

### HEARTBEAT (0x30) — Bidirectional

```
Byte:  [0]   [1] [2] [3] [4]
Field: type  timestamp (4)
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | 0x30 |
| timestamp | 4 | Sender's uptime in milliseconds (uint32 LE) |

---

## Protocol Flow

```
Client                              Server
  |                                   |
  |--- "AT+DOWNLOAD=session\n" -------|  (plain text)
  |                                   |
  |<-- AT_RESP (JSON) ----------------|
  |                                   |
  |<-- FILE_START (fn, size) ---------|
  |--- ACK(seq=0, win=32, 0x00) -----|
  |                                   |
  |<-- DATA(seq=0, crc, payload) -----|
  |<-- DATA(seq=1, crc, payload) -----|   sliding window (up to 32)
  |<-- DATA(seq=2, crc, payload) -----|
  |--- ACK(seq=3, win=32, 0b111) ----|   cumulative + bitmap
  |                                   |
  |<-- DATA(seq=3, ...) --------------|
  |                                   |
  |  ... (repeat until file done) ... |
  |                                   |
  |<-- FILE_END(crc32) ---------------|
  |--- ACK(seq=N, win=32, 0x00) -----|
  |                                   |
  |  ... (next file, if any) ...     |
  |                                   |
  |<-- TRANSFER_DONE(sid, count) -----|
  |                                   |
```

---

## Sliding Window

### Server Side

- **send_base**: oldest unACKed sequence number
- **next_seq**: next sequence number to assign
- **Window size**: `min(next_seq - send_base, peer_window)`
- Server **must not** exceed `peer_window` unACKed frames

### Client Side

- **expect_seq**: next expected in-order sequence number
- Sends ACK immediately upon receiving each DATA frame
- ACK contains cumulative ack_seq + 8-bit selective bitmap

### Retransmission

- Server checks every 100ms for frames unACKed > 200ms
- Retransmits from a buffer (no SD card re-read needed)
- Max 5 retries per frame, then transfer is aborted
- Lost frames are detected via cumulative ACK not advancing

### Flow Control

- Client advertises `window` in every ACK frame
- Server pauses when window = 0
- Default window: 32 frames
- Window can be dynamically adjusted by client

---

## CRC32

### Algorithm

IEEE Ethernet CRC32 (polynomial 0xEDB88320, reflected).

### Usage

1. **Per-frame CRC**: Each DATA frame includes CRC of its payload. Client verifies
   immediately and discards corrupted frames (no ACK sent → triggers retransmit).
2. **Full-file CRC**: FILE_END includes CRC of complete file data. Client
   independently computes and compares for end-to-end verification.

### Important

Both server (Zephyr) and client (Python) must use the **same algorithm**:
- Server: `crc32_ieee_update(0, data, len)` — Zephyr handles `~crc` internally
- Client: `binascii.crc32(data)` — standard Python CRC32
- For accumulation across chunks, both support passing the running CRC:
  - Server: `crc = crc32_ieee_update(crc, chunk, chunk_len)` (starting from 0)
  - Client: `crc = binascii.crc32(chunk, crc)` (starting from 0)

---

## Timing Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| Window size | 32 | Max unACKed frames in flight |
| Retransmit check interval | 100 ms | How often server checks for lost frames |
| Retransmit timeout | 200 ms | Time before a frame is considered lost |
| Max retries | 5 | Per-frame retransmission limit |
| Heartbeat interval | 5000 ms | Keepalive frequency |
| Connection timeout | 30000 ms | No activity = disconnected |
| Control ACK timeout | 2000 ms | Max wait for FILE_START/FILE_END ACK |

---

## Error Handling

| Scenario | Detection | Recovery |
|----------|-----------|----------|
| Corrupted DATA frame | Per-frame CRC mismatch | Client doesn't ACK → server retransmits |
| Lost DATA frame | Cumulative ACK stops advancing | Server detects timeout → retransmits |
| Lost ACK | Server retransmit timeout | Server resends the frame |
| Connection lost | Heartbeat timeout (30s) | Server resets transport state |

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 2.0 | 2026-03-27 | Complete redesign: sliding window, per-frame CRC, selective ACK |
| 1.0 | 2026-03-27 | Initial protocol definition |
