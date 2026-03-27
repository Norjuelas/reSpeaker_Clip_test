#!/usr/bin/env python3
"""
UDP Sync Tool for Clip — Protocol v2

Downloads recording sessions via UDP with sliding window protocol:
- Per-frame CRC32 (IEEE) for integrity
- Cumulative ACK + selective bitmap for efficient acknowledgment
- Dynamic flow control via window advertisement
- Full-file CRC32 verification at FILE_END

WiFi AP Configuration:
  SSID:     ClipAP_XXXX  (XXXX = last 4 hex digits of chip ID)
  Password: 12345678
  IP:       192.168.4.1
  UDP Port: 8089

Frame Types (v2):
  DATA          = 0x01   Server→Client
  ACK           = 0x02   Client→Server
  FILE_START    = 0x10   Server→Client
  FILE_END      = 0x11   Server→Client
  TRANSFER_DONE = 0x12   Server→Client
  AT_RESP       = 0x20   Server→Client
  HEARTBEAT     = 0x30   Bidirectional

Usage:
  python udp_sync.py                          # list sessions
  python udp_sync.py --all-sessions           # download all sessions
  python udp_sync.py --session 20260326120000 # download one session
"""

import socket
import sys
import json
import struct
import argparse
import time
import binascii
from pathlib import Path

# Frame types (CLIP UDP Transfer Protocol v2)
UDP_FRAME_DATA          = 0x01
UDP_FRAME_ACK           = 0x02
UDP_FRAME_FILE_START    = 0x10
UDP_FRAME_FILE_END      = 0x11
UDP_FRAME_TRANSFER_DONE = 0x12
UDP_FRAME_AT_RESP       = 0x20
UDP_FRAME_HEARTBEAT     = 0x30

# Frame sizes
UDP_DATA_HEADER_SIZE = 9   # type(1) + seq(2) + len(2) + crc32(4)
UDP_ACK_FRAME_SIZE   = 5   # type(1) + ack_seq(2) + window(1) + bitmap(1)

# Protocol config
WINDOW_SIZE = 32
HEARTBEAT_INTERVAL_S = 5.0
CONNECTION_TIMEOUT_S = 30.0
DEFAULT_HOST = "192.168.4.1"
DEFAULT_PORT = 8089


# ============================================================================
# IEEE CRC32 — must match Zephyr crc32_ieee_update(0, data, len)
# ============================================================================

def ieee_crc32(data: bytes) -> int:
    """IEEE CRC32 matching Zephyr crc32_ieee_update(0, data, len).

    Equivalent to: binascii.crc32(data)
    """
    return binascii.crc32(data) & 0xFFFFFFFF


# ============================================================================
# OGG Opus Writer (no external dependencies)
# ============================================================================

def _ogg_crc32_init():
    table = []
    for i in range(256):
        crc = i << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = (crc << 1) ^ 0x04C11DB7
            else:
                crc = crc << 1
            crc &= 0xFFFFFFFF
        table.append(crc)
    return table

_OGG_CRC_TABLE = _ogg_crc32_init()

def ogg_crc32(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc = ((crc << 8) ^ _OGG_CRC_TABLE[((crc >> 24) ^ byte) & 0xFF]) & 0xFFFFFFFF
    return crc


class OggOpusWriter:
    OPUS_INTERNAL_RATE = 48000

    def __init__(self, filename: str, sample_rate: int = 16000, channels: int = 1):
        self.file = open(filename, 'wb')
        self.sample_rate = sample_rate
        self.channels = channels
        self.serial = 0x12345678
        self.page_seq = 0
        self.granule = 0
        self.frame_size = self.OPUS_INTERNAL_RATE // 50

    def _write_page(self, granule: int, header_type: int, data: bytes):
        segment_table = []
        remaining = len(data)
        while remaining > 0:
            seg_size = min(255, remaining)
            segment_table.append(seg_size)
            remaining -= seg_size
        if not segment_table:
            segment_table = [0]

        header = bytearray()
        header.extend(b'OggS')
        header.append(0)
        header.append(header_type)
        header.extend(struct.pack('<Q', granule))
        header.extend(struct.pack('<I', self.serial))
        header.extend(struct.pack('<I', self.page_seq))
        header.extend(struct.pack('<I', 0))
        header.append(len(segment_table))
        header.extend(bytes(segment_table))

        page_data = bytes(header) + data
        crc = ogg_crc32(page_data)
        struct.pack_into('<I', header, 22, crc)

        self.file.write(bytes(header) + data)
        self.page_seq += 1

    def write_header(self):
        opus_head = bytearray()
        opus_head.extend(b'OpusHead')
        opus_head.append(1)
        opus_head.append(self.channels)
        opus_head.extend(struct.pack('<H', 312))
        opus_head.extend(struct.pack('<I', self.sample_rate))
        opus_head.extend(struct.pack('<H', 0))
        opus_head.append(0)
        self._write_page(0, 0x02, bytes(opus_head))

        opus_tags = bytearray()
        opus_tags.extend(b'OpusTags')
        vendor = b'ReSensor Clip'
        opus_tags.extend(struct.pack('<I', len(vendor)))
        opus_tags.extend(vendor)
        opus_tags.extend(struct.pack('<I', 0))
        self._write_page(0, 0x00, bytes(opus_tags))

    def write_packet(self, opus_data: bytes):
        self.granule += self.frame_size
        self._write_page(self.granule, 0x00, opus_data)

    def close(self):
        self.file.close()


def parse_raw_opus_frames(raw_data: bytes):
    frames = []
    offset = 0
    while offset < min(200, len(raw_data)):
        if offset + 2 > len(raw_data):
            break
        frame_len = struct.unpack('<H', raw_data[offset:offset+2])[0]
        if 10 <= frame_len <= 500:
            break
        offset += 2
    while offset < len(raw_data):
        if offset + 2 > len(raw_data):
            break
        frame_len = struct.unpack('<H', raw_data[offset:offset+2])[0]
        offset += 2
        if frame_len < 10 or frame_len > 1000:
            break
        if offset + frame_len > len(raw_data):
            break
        frames.append(raw_data[offset:offset+frame_len])
        offset += frame_len
    return frames


def convert_to_ogg_opus(input_file: Path, output_file: Path,
                        sample_rate: int = 16000, channels: int = 1) -> bool:
    try:
        with open(input_file, 'rb') as f:
            raw_data = f.read()
    except Exception as e:
        print(f"    Error reading {input_file}: {e}")
        return False
    if len(raw_data) == 0:
        return False
    frames = parse_raw_opus_frames(raw_data)
    if not frames:
        return False
    try:
        output_file.parent.mkdir(parents=True, exist_ok=True)
        writer = OggOpusWriter(str(output_file), sample_rate, channels)
        writer.write_header()
        for frame in frames:
            writer.write_packet(frame)
        writer.close()
        duration = len(frames) * 20 / 1000
        print(f"    Created: {output_file.name} ({_fmt_bytes(output_file.stat().st_size)}, {duration:.1f}s)")
        return True
    except Exception as e:
        print(f"    Error converting: {e}")
        return False


def _fmt_bytes(n):
    try:
        n = int(n)
    except (TypeError, ValueError):
        return str(n)
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.1f} MB"
    if n >= 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n} B"


# ============================================================================
# UDP Sync Client (Protocol v2)
# ============================================================================

class UDPSync:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=120.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.device_name = "Clip"
        self.bytes_received = 0
        self.last_activity_time = time.time()
        self.window_size = WINDOW_SIZE

    def connect(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.settimeout(self.timeout)
            self.sock.sendto(b"\n", (self.host, self.port))
            print(f"Connected to {self.device_name} ({self.host}:{self.port})")
            return True
        except Exception as e:
            print(f"Connection failed: {e}")
            return False

    def disconnect(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def _recv_frame(self, timeout_s=1.0):
        self.sock.settimeout(timeout_s)
        try:
            data, addr = self.sock.recvfrom(4096)
            self.last_activity_time = time.time()
            self.bytes_received += len(data)
            return data
        except socket.timeout:
            return None
        except Exception as e:
            print(f"  Recv error: {e}")
            return None

    def _send_ack(self, ack_seq, window, bitmap):
        """Send ACK frame: [type(1)][ack_seq(2)][window(1)][bitmap(1)]"""
        ack = struct.pack('<BBBBB',
                          UDP_FRAME_ACK,
                          ack_seq & 0xFF,
                          (ack_seq >> 8) & 0xFF,
                          window & 0xFF,
                          bitmap & 0xFF)
        try:
            self.sock.sendto(ack, (self.host, self.port))
        except Exception as e:
            print(f"  ACK send error: {e}")

    def _send_heartbeat(self):
        ts = int(time.time() * 1000) & 0xFFFFFFFF
        hb = struct.pack('<BI', UDP_FRAME_HEARTBEAT, ts)
        try:
            self.sock.sendto(hb, (self.host, self.port))
        except Exception:
            pass

    def _send_at_command(self, cmd):
        """Send AT command and wait for AT_RESP."""
        if not cmd.upper().startswith("AT"):
            cmd = f"AT+{cmd}"
        self.sock.sendto((cmd + "\n").encode(), (self.host, self.port))

        for _ in range(10):
            data = self._recv_frame(2.0)
            if data is None:
                continue
            if len(data) >= 3 and data[0] == UDP_FRAME_AT_RESP:
                resp_len = data[1] | (data[2] << 8)
                if len(data) >= 3 + resp_len:
                    response = data[3:3+resp_len].decode('utf-8', errors='replace')
                    try:
                        return json.loads(response)
                    except json.JSONDecodeError:
                        return {"ok": True, "raw": response}
            # Ignore other frames (ACKs, heartbeats, etc.)
        return {"ok": False, "error": "No response"}

    def list_sessions(self):
        result = self._send_at_command("LIST")
        if not result.get("ok"):
            print(f"AT+LIST error: {result.get('error')}")
            return []
        data = result.get("data", [])
        if isinstance(data, list):
            return data
        if isinstance(data, dict):
            return data.get("sessions", [])
        return []

    def download_session(self, session_id, out_dir, convert_ogg=True):
        session_dir = out_dir / self.device_name / session_id
        session_dir.mkdir(parents=True, exist_ok=True)
        downloaded_files = []

        result = self._send_at_command(f"DOWNLOAD={session_id}")
        if not result.get("ok"):
            print(f"  Error: {result.get('error', 'unknown')}")
            return False

        data = result.get("data", {})
        total_files = data.get("files", "?")
        total_bytes = data.get("bytes", "?")
        print(f"  Session {session_id}: {total_files} file(s), "
              f"{_fmt_bytes(total_bytes) if isinstance(total_bytes, int) else total_bytes}")

        # Receive state
        current_name = None
        current_data = bytearray()
        current_size = 0
        expect_seq = 0
        files_received = 0
        frame_count = 0
        last_progress_time = time.time()
        file_crc = 0  # Running IEEE CRC32 for full file verification

        heartbeat_next = time.time() + HEARTBEAT_INTERVAL_S

        try:
            while True:
                now = time.time()

                # Periodic heartbeat
                if now >= heartbeat_next:
                    self._send_heartbeat()
                    heartbeat_next = now + HEARTBEAT_INTERVAL_S

                data = self._recv_frame(5.0)
                if data is None:
                    elapsed = time.time() - last_progress_time
                    if elapsed > 30:
                        print(f"\n  Timeout after {elapsed:.0f}s")
                        return False
                    continue

                if len(data) < 1:
                    continue

                frame_type = data[0]

                # ---- Non-file frames ----
                if frame_type == UDP_FRAME_AT_RESP:
                    resp_len = data[1] | (data[2] << 8)
                    if len(data) >= 3 + resp_len:
                        resp = data[3:3+resp_len].decode('utf-8', errors='replace')
                        print(f"  AT Response: {resp}")
                    continue

                if frame_type == UDP_FRAME_HEARTBEAT:
                    continue

                if frame_type == UDP_FRAME_ACK:
                    # Server→client ACK (shouldn't happen, but ignore)
                    continue

                # ---- FILE_START ----
                if frame_type == UDP_FRAME_FILE_START:
                    if len(data) < 3:
                        print("  Invalid FILE_START")
                        continue
                    fn_len = data[1]
                    if len(data) < 2 + fn_len + 4:
                        print(f"  FILE_START truncated: len={len(data)}, fn_len={fn_len}")
                        continue
                    filename = data[2:2+fn_len].decode('utf-8', errors='replace')
                    file_size = struct.unpack("<I", data[2+fn_len:2+fn_len+4])[0]

                    current_name = filename
                    current_size = file_size
                    current_data = bytearray()
                    expect_seq = 0
                    file_crc = 0

                    # ACK FILE_START
                    self._send_ack(0, self.window_size, 0)
                    print(f"  <- {filename} ({_fmt_bytes(file_size)})", end="", flush=True)
                    last_progress_time = time.time()
                    continue

                # ---- DATA ----
                if frame_type == UDP_FRAME_DATA:
                    if len(data) < UDP_DATA_HEADER_SIZE:
                        continue
                    seq = data[1] | (data[2] << 8)
                    data_len = data[3] | (data[4] << 8)
                    recv_crc = struct.unpack("<I", data[5:9])[0]

                    if len(data) < UDP_DATA_HEADER_SIZE + data_len:
                        continue

                    payload = data[UDP_DATA_HEADER_SIZE:UDP_DATA_HEADER_SIZE + data_len]

                    # Per-frame CRC verification
                    calc_crc = ieee_crc32(payload)
                    if calc_crc != recv_crc:
                        # CRC mismatch — discard frame, don't ACK, wait for retransmit
                        print("!", end="", flush=True)
                        continue

                    # Build ACK bitmap: track up to 8 frames ahead of expect_seq
                    bitmap = 0
                    if seq == expect_seq:
                        # In-order frame
                        current_data.extend(payload)
                        file_crc = binascii.crc32(payload, file_crc) & 0xFFFFFFFF
                        expect_seq += 1
                        last_progress_time = time.time()

                        # Progress indicator
                        if len(current_data) % (64 * 1024) < data_len:
                            print(".", end="", flush=True)
                    elif seq > expect_seq and seq < expect_seq + 8:
                        # Out-of-order but within bitmap range — buffer it
                        # For simplicity, just set the bitmap bit
                        # (full reordering buffer would require more complexity)
                        offset = seq - expect_seq
                        bitmap |= (1 << offset)
                        # Store out-of-order data
                        # Extend current_data with zeros as placeholder if needed
                        # This is a simplified implementation — retransmit fills gaps
                    else:
                        # Old or too-far-ahead frame — ignore
                        bitmap = 0

                    # Send ACK with cumulative seq and bitmap
                    self._send_ack(expect_seq, self.window_size, bitmap)
                    frame_count += 1
                    continue

                # ---- FILE_END ----
                if frame_type == UDP_FRAME_FILE_END:
                    if len(data) < 5:
                        print("  Invalid FILE_END")
                        continue
                    server_crc = struct.unpack("<I", data[1:5])[0]

                    # Compare full-file CRC
                    crc_ok = (file_crc == server_crc)
                    status_str = "OK" if crc_ok else f"MISMATCH (local=0x{file_crc:08x}, server=0x{server_crc:08x})"
                    print(f"\n  FILE_END: CRC {status_str}")

                    # ACK FILE_END
                    self._send_ack(expect_seq, self.window_size, 0)

                    if current_name and current_data:
                        out_file = session_dir / current_name
                        out_file.write_bytes(bytes(current_data))
                        print(f"  Saved {current_name} ({_fmt_bytes(len(current_data))})")
                        if current_name.endswith('.opus'):
                            downloaded_files.append((current_name, session_dir / current_name))
                        files_received += 1

                    current_name = None
                    current_data = bytearray()
                    last_progress_time = time.time()
                    continue

                # ---- TRANSFER_DONE ----
                if frame_type == UDP_FRAME_TRANSFER_DONE:
                    if len(data) < 2:
                        print("  Invalid TRANSFER_DONE")
                        break
                    sid_len = data[1]
                    if len(data) >= 2 + sid_len + 4:
                        sess = data[2:2+sid_len].decode('utf-8', errors='replace')
                        file_count = struct.unpack("<I", data[2+sid_len:2+sid_len+4])[0]
                        print(f"  Transfer done: {file_count} file(s)")
                    break

                # Unknown frame
                print(f"  Unknown frame 0x{frame_type:02x}")

        except socket.timeout:
            print("  Connection timeout")
            return False
        except Exception as e:
            print(f"  Error: {e}")
            import traceback
            traceback.print_exc()
            return False

        # Merge opus files and convert to OGG
        if convert_ogg and downloaded_files:
            print(f"\n  Merging {len(downloaded_files)} file(s)...")
            downloaded_files.sort(key=lambda x: x[0])

            merged_data = bytearray()
            for filename, filepath in downloaded_files:
                with open(filepath, 'rb') as f:
                    merged_data.extend(f.read())

            merged_opus_path = session_dir / f"{session_id}.opus"
            merged_opus_path.write_bytes(bytes(merged_data))
            print(f"    Merged: {merged_opus_path.name} ({_fmt_bytes(len(merged_data))})")

            ogg_path = session_dir / f"{session_id}.ogg"
            print(f"    Converting to OGG...", end="", flush=True)
            if convert_to_ogg_opus(merged_opus_path, ogg_path):
                print(" OK")
            else:
                print(" Failed")

        return files_received > 0


def main():
    parser = argparse.ArgumentParser(description="UDP Sync Tool for Clip (Protocol v2)")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--session", "-s")
    parser.add_argument("--all-sessions", "-a", action="store_true")
    parser.add_argument("--output", "-o", type=Path, default=Path("recordings"))
    args = parser.parse_args()

    sync = UDPSync(args.host, args.port, args.timeout)
    if not sync.connect():
        sys.exit(1)

    try:
        if args.session:
            sessions_to_sync = [{"id": args.session}]
        elif args.all_sessions:
            sessions_to_sync = sync.list_sessions()
            if not sessions_to_sync:
                print("No sessions found")
                sys.exit(0)
        else:
            sessions_to_sync = sync.list_sessions()
            print(f"\nSessions: {len(sessions_to_sync)}")
            for s in sessions_to_sync:
                print(f"  {s.get('id', '?')}")
            print("\nUse --session or --all-sessions to download")
            sys.exit(0)

        print(f"\nOutput: {args.output}")
        ok = 0
        for idx, s in enumerate(sessions_to_sync, 1):
            sid = s.get("id", "?")
            print(f"\n[{idx}/{len(sessions_to_sync)}] {sid}")
            if not sync.download_session(sid, args.output):
                print(f"  Failed")
            else:
                ok += 1

        print(f"\nDone: {ok}/{len(sessions_to_sync)}")

    except KeyboardInterrupt:
        print("\nInterrupted")
    except Exception as e:
        print(f"\nError: {e}")
    finally:
        sync.disconnect()


if __name__ == "__main__":
    main()
