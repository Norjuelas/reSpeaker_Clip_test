#!/usr/bin/env python3
"""
UDP Sync Tool for Clip2

Downloads recording sessions via UDP file transfer (WiFi AP).
Uses stop-and-wait protocol with sequence numbers for reliability.

WiFi AP Configuration:
  SSID:     ClipAP_XXXX  (XXXX = last 4 hex digits of chip ID)
  Password: 12345678
  IP:       192.168.4.1
  UDP Port: 8089

Protocol (stop-and-wait):
  Sender -> Receiver: FILE_START | filename_len | filename | file_size(4)
  Receiver -> Sender: ACK | seq=0
  Sender -> Receiver: FILE_DATA | seq=N | length(2) | data
  Receiver -> Sender: ACK | seq=N
  ... (repeat until file complete)
  Sender -> Receiver: FILE_END | filename_len | filename
  Receiver -> Sender: ACK | seq=MAX
  Sender -> Receiver: TRANSFER_DONE | session_len | session_id | file_count(4)
  Receiver -> Sender: ACK | seq=MAX

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
from pathlib import Path

# Frame types (must match transport_udp.c)
FRAME_ACK             = 0x80
FRAME_FILE_START_UDP  = 0x11
FRAME_FILE_DATA_UDP   = 0x12
FRAME_FILE_END_UDP    = 0x13
FRAME_TRANSFER_DONE_UDP = 0x14

# Protocol config
ACK_TIMEOUT_MS = 500
MAX_RETRIES = 10
MAX_DATA_PER_PKT = 480


# ============================================================================
# OGG CRC32 Implementation (OGG-specific polynomial)
# ============================================================================

def _ogg_crc32_init():
    """Generate CRC32 lookup table for OGG (polynomial 0x04C11DB7)."""
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
    """Calculate OGG CRC32 (uses different polynomial than standard CRC32)."""
    crc = 0
    for byte in data:
        crc = ((crc << 8) ^ _OGG_CRC_TABLE[((crc >> 24) ^ byte) & 0xFF]) & 0xFFFFFFFF
    return crc


# ============================================================================
# Simple OGG Opus Writer (no external dependencies)
# ============================================================================

class OggOpusWriter:
    """Simple OGG Opus file writer."""

    OPUS_INTERNAL_RATE = 48000

    def __init__(self, filename: str, sample_rate: int = 16000, channels: int = 1):
        self.file = open(filename, 'wb')
        self.sample_rate = sample_rate
        self.channels = channels
        self.serial = 0x12345678
        self.page_seq = 0
        self.granule = 0
        self.frame_size = self.OPUS_INTERNAL_RATE // 50  # 20ms = 960 samples at 48kHz

    def _write_page(self, granule: int, header_type: int, data: bytes):
        """Write an OGG page."""
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
        header.extend(struct.pack('<I', 0))  # CRC placeholder
        header.append(len(segment_table))
        header.extend(bytes(segment_table))

        page_data = bytes(header) + data
        crc = ogg_crc32(page_data)
        struct.pack_into('<I', header, 22, crc)

        self.file.write(bytes(header) + data)
        self.page_seq += 1

    def write_header(self):
        """Write OpusHead and OpusTags pages."""
        opus_head = bytearray()
        opus_head.extend(b'OpusHead')
        opus_head.append(1)
        opus_head.append(self.channels)
        opus_head.extend(struct.pack('<H', 312))  # Pre-skip
        opus_head.extend(struct.pack('<I', self.sample_rate))
        opus_head.extend(struct.pack('<H', 0))  # Output gain
        opus_head.append(0)  # Channel mapping family

        self._write_page(0, 0x02, bytes(opus_head))

        opus_tags = bytearray()
        opus_tags.extend(b'OpusTags')
        vendor = b'ReSensor Clip'
        opus_tags.extend(struct.pack('<I', len(vendor)))
        opus_tags.extend(vendor)
        opus_tags.extend(struct.pack('<I', 0))

        self._write_page(0, 0x00, bytes(opus_tags))

    def write_packet(self, opus_data: bytes):
        """Write an Opus audio packet."""
        self.granule += self.frame_size
        self._write_page(self.granule, 0x00, opus_data)

    def close(self):
        """Close file."""
        self.file.close()


def parse_raw_opus_frames(raw_data: bytes):
    """Parse raw Opus frames from device format."""
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

        frame_data = raw_data[offset:offset+frame_len]
        offset += frame_len
        frames.append(frame_data)

    return frames


def convert_to_ogg_opus(input_file: Path, output_file: Path,
                        sample_rate: int = 16000, channels: int = 1) -> bool:
    """Convert raw Opus frames to OGG Opus format."""
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
        print(f"    Error converting to OGG: {e}")
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


class UDPSync:
    """Download sessions from Clip2 device over UDP."""

    def __init__(self, host="192.168.4.1", port=8089, timeout=120.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.device_name = "Clip2"
        self.expected_seq = 0
        self.last_recv_time = time.time()
        self.bytes_received = 0

    def connect(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.settimeout(self.timeout)
            # Send initial packet to discover server
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

    def _recv_ack(self, expected_seq, timeout_ms=ACK_TIMEOUT_MS):
        """Receive ACK for expected sequence number."""
        retries = 0
        while retries < MAX_RETRIES:
            try:
                data, addr = self.sock.recvfrom(1024)
                if not data:
                    continue

                if data[0] == FRAME_ACK and len(data) >= 4:
                    ack_seq = data[1] | (data[2] << 8)
                    if ack_seq == expected_seq:
                        return 0
                    else:
                        print(f"  ACK seq mismatch: got {ack_seq}, expected {expected_seq}")
                retries += 1
                if retries >= MAX_RETRIES:
                    print(f"  Max retries exceeded for seq {expected_seq}")
                    return -1
            except socket.timeout:
                retries += 1
                if retries >= MAX_RETRIES:
                    print(f"  ACK timeout for seq {expected_seq}")
                    return -1
        return -1

    def _send_frame(self, frame_type, seq, data=b""):
        """Send frame and wait for ACK (stop-and-wait)."""
        # Build header: type(1) + seq(2) + length(1)
        header = bytes([frame_type, seq & 0xFF, (seq >> 8) & 0xFF, len(data) & 0xFF])

        for retry in range(MAX_RETRIES):
            try:
                # Send header
                self.sock.sendto(header, (self.host, self.port))
                # Send data if any
                if data:
                    self.sock.sendto(data, (self.host, self.port))

                # Wait for ACK
                ret = self._recv_ack(seq)
                if ret == 0:
                    return 0

                print(f"  Retry {retry + 1}/{MAX_RETRIES}")
            except Exception as e:
                print(f"  Send error: {e}")
                return -1

        return -1

    def _send_cmd(self, cmd):
        """Send AT command, return JSON response."""
        if not cmd.upper().startswith("AT"):
            cmd = f"AT+{cmd}"
        self.sock.sendto((cmd + "\n").encode(), (self.host, self.port))

        try:
            data, addr = self.sock.recvfrom(4096)
            if data:
                line = data.decode('utf-8', errors='replace').strip()
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    return {"ok": True, "raw": line}
        except socket.timeout:
            return {"ok": False, "error": "Timeout"}

        return {"ok": False, "error": "No response"}

    def list_sessions(self):
        """List sessions from AT+LIST."""
        result = self._send_cmd("AT+LIST")
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
        """Download session using UDP protocol."""
        session_dir = out_dir / self.device_name / session_id
        session_dir.mkdir(parents=True, exist_ok=True)

        # Track downloaded opus files for later conversion
        downloaded_files = []

        # Send download command
        self.sock.sendto(f"AT+DOWNLOAD={session_id}\n".encode(), (self.host, self.port))

        try:
            info_data, addr = self.sock.recvfrom(4096)
            print(f"  First packet: {len(info_data)} bytes, first 20 bytes: {info_data[:20]}")
            # Check if this is JSON or binary frame
            if info_data and len(info_data) > 0:
                # If first byte is '{', it's JSON
                if info_data[0] == ord('{'):
                    info_line = info_data.decode('utf-8', errors='replace').strip()
                    try:
                        info = json.loads(info_line)
                        print(f"  JSON parsed: {info}")
                    except json.JSONDecodeError as e:
                        print(f"  JSON parse error: {e}, data: {info_line[:100]}")
                        # Continue anyway to try receiving binary frames
                        info = {"ok": True, "data": {}}
                else:
                    # First byte is not '{', might be a binary frame - skip parsing
                    print(f"  First packet is binary (0x{info_data[0]:02x}='{chr(info_data[0]) if info_data[0] < 128 else '?'}'), skipping JSON info")
                    info = {"ok": True, "data": {}}
            else:
                info = {"ok": True, "data": {}}
        except socket.timeout:
            print(f"  Timeout waiting for download info")
            return False

        if not info.get("ok"):
            print(f"  Error: {info.get('error', 'unknown')}")
            return False

        data = info.get("data", {})
        total_files = data.get("files", "?")
        total_bytes = data.get("bytes", "?")
        print(f"  Session {session_id}: {total_files} file(s), {_fmt_bytes(total_bytes) if isinstance(total_bytes, int) else total_bytes}")

        # Receive files using UDP protocol
        files_received = 0
        current_name = None
        current_data = bytearray()
        current_size = 0
        self.expected_seq = 0

        try:
            frame_count = 0
            last_progress_time = time.time()
            last_frame_count = 0
            while True:
                # Receive frame
                try:
                    frame_data, addr = self.sock.recvfrom(4096)
                except socket.timeout:
                    elapsed = time.time() - last_progress_time
                    if elapsed > 10 and frame_count == last_frame_count:
                        print(f"\n  Timeout after {elapsed:.0f}s of no progress (received {self.bytes_received} bytes)")
                        return False
                    print("  Timeout waiting for frame")
                    continue

                if not frame_data or len(frame_data) < 4:
                    continue

                self.bytes_received += len(frame_data)
                now = time.time()
                if now - last_progress_time > 5:
                    print(f"\n  Progress: {frame_count} frames, {self.bytes_received} bytes")
                    last_progress_time = now
                    last_frame_count = frame_count

                frame_count += 1
                if frame_count <= 5:
                    print(f"  Frame {frame_count}: {len(frame_data)} bytes, hex: {frame_data[:16].hex()}")

                frame_type = frame_data[0]

                # Skip non-binary frames (JSON responses, etc)
                if frame_type != FRAME_FILE_START_UDP and frame_type != FRAME_FILE_DATA_UDP and \
                   frame_type != FRAME_FILE_END_UDP and frame_type != FRAME_TRANSFER_DONE_UDP and \
                   frame_type != FRAME_ACK:
                    if frame_count > 5:
                        print(f"  Skipping non-frame byte 0x{frame_type:02x}")
                    continue

                seq = frame_data[1] | (frame_data[2] << 8)

                # Send ACK first
                ack_frame = bytes([FRAME_ACK, seq & 0xFF, (seq >> 8) & 0xFF, 0, 0])
                self.sock.sendto(ack_frame, (self.host, self.port))

                if frame_type == FRAME_FILE_START_UDP:
                    # Format: type(1) + seq(2) + fn_len(1) + filename(fn_len) + size(4)
                    fn_len = frame_data[3]  # byte 3 is fn_len (1 byte)
                    filename_start = 4
                    if len(frame_data) >= filename_start + fn_len + 4:
                        filename = frame_data[filename_start:filename_start+fn_len].decode('utf-8', errors='replace')
                        file_size = struct.unpack("<I", frame_data[filename_start+fn_len:filename_start+fn_len+4])[0]
                        current_name = filename
                        current_size = file_size
                        current_data = bytearray()
                        print(f"  <- {filename} ({_fmt_bytes(file_size)})", end="", flush=True)
                    else:
                        print(f"  FILE_START parse error: len={len(frame_data)}, fn_len={fn_len}, need={filename_start + fn_len + 4}")

                elif frame_type == FRAME_FILE_DATA_UDP:
                    # Format: type(1) + seq(2) + length(2) + data
                    data_len = frame_data[3] | (frame_data[4] << 8)  # 2-byte length
                    # Data follows header at offset 5
                    if len(frame_data) >= 5 + data_len:
                        current_data.extend(frame_data[5:5+data_len])
                        # Progress indicator every 64KB
                        if len(current_data) % (64 * 1024) < MAX_DATA_PER_PKT:
                            print(".", end="", flush=True)
                    else:
                        print(f"  Incomplete DATA frame: have {len(frame_data)}, need {5 + data_len}")

                elif frame_type == FRAME_FILE_END_UDP:
                    # Format: type(1) + seq(2) + fn_len(1) + filename(fn_len)
                    fn_len = frame_data[3]  # byte 3 is fn_len (1 byte)
                    if len(frame_data) >= 4 + fn_len:
                        received_name = frame_data[4:4+fn_len].decode('utf-8', errors='replace')
                        print(f"  FILE_END: {received_name}")
                        if current_name and current_data:
                            out_file = session_dir / current_name
                            out_file.write_bytes(bytes(current_data))
                            print(f" OK ({_fmt_bytes(len(current_data))})")
                            # Track file for merging
                            if current_name.endswith('.opus'):
                                downloaded_files.append((current_name, session_dir / current_name))
                            files_received += 1
                        current_name = None
                        current_data = bytearray()

                elif frame_type == FRAME_TRANSFER_DONE_UDP:
                    # Format: type(1) + seq(2) + sid_len(1) + session_id(sid_len) + file_count(4)
                    sid_len = frame_data[3]  # byte 3 is sid_len (1 byte)
                    if len(frame_data) >= 4 + sid_len + 4:
                        session = frame_data[4:4+sid_len].decode('utf-8', errors='replace')
                        file_count = struct.unpack("<I", frame_data[4+sid_len:4+sid_len+4])[0]
                        print(f"  Done: {file_count} file(s) transferred")
                    break

                else:
                    print(f"  Unknown frame 0x{frame_type:02x}")
                    return False

        except socket.timeout:
            print("  Connection timeout")
            return False
        except Exception as e:
            print(f"  Error: {e}")
            return False

        # Merge all .opus files into one and convert to OGG
        if convert_ogg and downloaded_files:
            print(f"\n  Merging {len(downloaded_files)} file(s)...")
            # Sort by filename to maintain order
            downloaded_files.sort(key=lambda x: x[0])

            # Merge all opus data
            merged_data = bytearray()
            for filename, filepath in downloaded_files:
                with open(filepath, 'rb') as f:
                    merged_data.extend(f.read())

            # Write merged opus file
            merged_opus_path = session_dir / f"{session_id}.opus"
            merged_opus_path.write_bytes(bytes(merged_data))
            print(f"    Merged: {merged_opus_path.name} ({_fmt_bytes(len(merged_data))})")

            # Convert merged opus to ogg
            ogg_path = session_dir / f"{session_id}.ogg"
            print(f"    Converting to OGG...", end="", flush=True)
            if convert_to_ogg_opus(merged_opus_path, ogg_path):
                print(f" OK")
            else:
                print(f" Failed")

        return files_received > 0


def main():
    parser = argparse.ArgumentParser(description="UDP Sync Tool for Clip2")
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=8089)
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