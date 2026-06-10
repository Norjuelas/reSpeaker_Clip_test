# BLE AT Command Reference

## BLE GATT Service

**Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`

| Characteristic | UUID | Properties | Purpose |
|---------------|------|------------|---------|
| Command Receive | `...0002` | Write | App sends AT commands |
| Response Send | `...0003` | Notify | JSON responses + events |
| File Data | `...0004` | Notify | Binary frame protocol (transfer) |
| Audio Visualization | `...0005` | Notify | 7-byte packed energy levels |

## Connection Parameters

| Parameter | Value |
|-----------|-------|
| Pairing | LE Secure Connections (mandatory) |
| Bonding | Required, 1 max paired device |
| Encryption | AES-128 CCM |
| MTU | Negotiated up to 517 (recommended: 247) |
| Connection Interval | 15-80ms (adaptive) |
| Max notification payload | MTU - 3 (244 for MTU 247) |

## AT Command Syntax

| Type | Format | Example |
|------|--------|---------|
| EXEC | `AT+XX` | `AT+GSTAT` |
| SET | `AT+XX=<value>` | `AT+MODE=enhanced` |
| GET | `AT+XX?` | `AT+MODE?` |

## JSON Response Format

```json
{"ok":true,"data":{...}}        // Success
{"ok":true}                      // Success (no data)
{"ok":false,"error":"message"}   // Error
{"event":"state","state":"RECORDING","session":"..."}  // Unsolicited event
```

Distinguish events from responses by checking for the `"event"` key.

## Key Commands

### Status & System

| Command | Type | Purpose |
|---------|------|---------|
| `AT+GSTAT` | EXEC | Device status (state, battery, session, mode, free_space) |
| `AT+VERSION` | EXEC | Firmware, hardware, SDK versions |
| `AT+DEVICE` / `AT+DEVICE?` | EXEC/GET | Device name |
| `AT+NAME=<n>` / `AT+NAME?` | SET/GET | User-defined name (1-32 chars), `CLEAR` to remove |
| `AT+TIME=<ts>` / `AT+TIME?` | SET/GET | Unix timestamp |
| `AT+POWEROFF` | EXEC | Enter PMIC ship mode |
| `AT+REBOOT` | EXEC | Reboot device |
| `AT+FACTORY=confirm` | SET | Factory reset (clears NVS, formats SD, reboots) |

### Recording

| Command | Type | Purpose |
|---------|------|---------|
| `AT+START[=<mode>]` | EXEC/SET | Start recording (optional mode override) |
| `AT+STOP` | EXEC | Stop recording → IDLE |
| `AT+PAUSE` | EXEC | Pause recording |
| `AT+RESUME` | EXEC | Resume (creates new segment file) |
| `AT+MARK[=<note>]` | EXEC/SET | Add bookmark at current position |

### Sessions & Transfer

| Command | Type | Purpose |
|---------|------|---------|
| `AT+LIST` | GET | List sessions (paginated, newest-first) |
| `AT+LIST=<session>` | SET | Session details (files, synced, channels, sample_rate) |
| `AT+LIST=<session>?<page>&<per_page>` | GET | Paginated file list |
| `AT+DOWNLOAD=<session>` | SET | Download all files |
| `AT+DOWNLOAD=<session>/<file>` | SET | Download single file |
| `AT+DOWNLOAD=<session>:<start_file>` | SET | Resume from file (e.g. `:0016.opus`) |
| `AT+CANCEL` | EXEC | Cancel transfer |
| `AT+DELETE=<session>` | SET | Delete session |
| `AT+PURGEABLE` | EXEC | Query transferred (deletable) sessions |
| `AT+PURGE` | EXEC | Delete all transferred sessions |
| `AT+MARKS=<session>[?<page>&<per_page>]` | GET/SET | Get bookmarks |

### Configuration

| Command | Type | Purpose |
|---------|------|---------|
| `AT+MODE=<normal\|enhanced>` | SET | Recording mode preset |
| `AT+AUTODEL=<off\|0\|1-30>` | SET | Auto-delete policy |
| `AT+BRIGHTNESS=<0-255>` | SET | OLED brightness (default 128) |
| `AT+WIFI=<on\|off>` / `AT+WIFI?` | SET/GET | WiFi AP control |
| `AT+USB=<on\|off>` / `AT+USB?` | SET/GET | USB CDC+MSC control |
| `AT+PAIR=reset` / `AT+PAIR?` | SET/GET | BLE pairing management |
| `AT+FORMAT` | EXEC | Format SD card |

## State Machine

States: UNINITIALIZED → IDLE → RECORDING / WIFI_SYNC / TRANSMITTING / PAUSED / ERROR

Key constraints:
- Cannot start WiFi while recording
- Cannot start recording while WiFi is active
- Only IDLE can transition to RECORDING or WIFI_SYNC

## Audio Visualization (Characteristic `...0005`)

7 bytes → 13 energy values (4 bits each), range 0-10, update rate ~100ms during recording.

```python
values = []
for i in range(6):
    values.append(data[i] >> 4)
    values.append(data[i] & 0x0F)
values.append(data[6] & 0x0F)
```

## Unsolicited Event Notifications

| Event | Format |
|-------|--------|
| State change | `{"event":"state","state":"RECORDING","session":"..."}` |
| State change (stop) | `{"event":"state","state":"IDLE","session":"...","duration":600}` |
| Bookmark | `{"event":"mark","session":"...","mark_count":3}` |
| BLE status | `{"event":"ble","status":"connected"/"disconnected"}` |
| WiFi status | `{"event":"wifi","status":"on"/"off"}` |
| USB status | `{"event":"usb","status":"on"/"off"}` |

## Error Codes

| Range | Category |
|-------|----------|
| 1000-1999 | Protocol (syntax, parsing) |
| 2000-2999 | System (init, memory) |
| 3000-3999 | Storage (SD card, FS) |
| 4000-4999 | Recording (audio, encoding) |
| 5000-5999 | Transfer (BLE, download) |
| 6000-6999 | Configuration (settings) |

Full spec: `docs/protocol.md`. BLE test script: `tests/ble_test.py`.
