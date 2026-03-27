#!/usr/bin/env python3
"""
TCP Sync Tool for Clip2

Downloads recording sessions via TCP file transfer (WiFi AP).
Uses variable-length binary frame protocol.

WiFi AP Configuration:
  SSID:     ClipAP_XXXX  (XXXX = last 4 hex digits of chip ID)
  Password: 12345678
  IP:       192.168.4.1
  TCP Port: 8080

Usage:
  python tcp_sync.py                          # list sessions
  python tcp_sync.py --all-sessions           # download all sessions
  python tcp_sync.py --session 20260326120000 # download one session
"""

import socket
import sys
import json
import struct
import argparse
from pathlib import Path

# Frame types (must match transport_tcp.c)
FRAME_FILE_START     = 0x01
FRAME_FILE_DATA      = 0x02
FRAME_FILE_END       = 0x03
FRAME_TRANSFER_DONE  = 0x04


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


class TCPSync:
    """Download sessions from Clip2 device over TCP."""

    def __init__(self, host="192.168.4.1", port=8080, timeout=30.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self._buf = b""
        self.device_name = "Clip2"

    def connect(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect((self.host, self.port))
            # Read ready banner
            banner = self._recv_line()
            if banner:
                try:
                    d = json.loads(banner)
                    self.device_name = d.get("device", "Clip2")
                except json.JSONDecodeError:
                    pass
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

    def _recv_exactly(self, n):
        """Read exactly n bytes."""
        while len(self._buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("Connection closed")
            self._buf += chunk
        data, self._buf = self._buf[:n], self._buf[n:]
        return data

    def _recv_line(self):
        """Read until newline."""
        while b"\n" not in self._buf:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return ""
            if not chunk:
                return ""
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8", errors="replace").rstrip("\r")

    def _send_cmd(self, cmd):
        """Send AT command, return JSON response."""
        if not cmd.upper().startswith("AT"):
            cmd = f"AT+{cmd}"
        self.sock.sendall((cmd + "\n").encode())
        line = self._recv_line()
        if not line:
            return {"ok": False, "error": "Timeout"}
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            return {"ok": True, "raw": line}

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

    def download_session(self, session_id, out_dir):
        """Download session using binary frame protocol."""
        session_dir = out_dir / self.device_name / session_id
        session_dir.mkdir(parents=True, exist_ok=True)

        # Send download command
        self.sock.sendall(f"AT+DOWNLOAD={session_id}\n".encode())
        info_line = self._recv_line()

        try:
            info = json.loads(info_line)
        except json.JSONDecodeError:
            print(f"  Unexpected response: {info_line}")
            return False

        if not info.get("ok"):
            print(f"  Error: {info.get('error', 'unknown')}")
            return False

        data = info.get("data", {})
        total_files = data.get("files", "?")
        total_bytes = data.get("bytes", "?")
        print(f"  Session {session_id}: {total_files} file(s), {_fmt_bytes(total_bytes) if isinstance(total_bytes, int) else total_bytes}")

        # Receive binary frames
        files_received = 0
        current_name = None
        current_data = bytearray()

        try:
            while True:
                ftype = self._recv_exactly(1)[0]

                if ftype == FRAME_FILE_START:
                    # Variable-length: type(1) + len(1) + filename + size(4)
                    fn_len = self._recv_exactly(1)[0]
                    filename = self._recv_exactly(fn_len).decode("utf-8", errors="replace")
                    file_size = struct.unpack("<I", self._recv_exactly(4))[0]
                    current_name = filename
                    current_data = bytearray()
                    print(f"  <- {filename} ({_fmt_bytes(file_size)})", end="", flush=True)

                elif ftype == FRAME_FILE_DATA:
                    length = struct.unpack("<H", self._recv_exactly(2))[0]
                    chunk = self._recv_exactly(length)
                    current_data.extend(chunk)

                elif ftype == FRAME_FILE_END:
                    # Variable-length: type(1) + len(1) + filename
                    fn_len = self._recv_exactly(1)[0]
                    filename = self._recv_exactly(fn_len).decode("utf-8", errors="replace")

                    if current_name and current_data:
                        out_file = session_dir / current_name
                        out_file.write_bytes(bytes(current_data))
                        print(f" OK ({len(current_data)} bytes)")
                        files_received += 1
                    else:
                        print()

                    current_name = None
                    current_data = bytearray()

                elif ftype == FRAME_TRANSFER_DONE:
                    # Variable-length: type(1) + len(1) + session + count(4)
                    sid_len = self._recv_exactly(1)[0]
                    session = self._recv_exactly(sid_len).decode("utf-8", errors="replace")
                    file_count = struct.unpack("<I", self._recv_exactly(4))[0]
                    print(f"  Done: {file_count} file(s) transferred")
                    break

                else:
                    print(f"  Unknown frame 0x{ftype:02x}")
                    return False

        except ConnectionError:
            print("  Connection closed")
            return False
        except Exception as e:
            print(f"  Error: {e}")
            return False

        return files_received > 0


def main():
    parser = argparse.ArgumentParser(description="TCP Sync Tool for Clip2")
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--session", "-s")
    parser.add_argument("--all-sessions", "-a", action="store_true")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--output", "-o", type=Path, default=Path("recordings"))
    args = parser.parse_args()

    sync = TCPSync(args.host, args.port, args.timeout)
    if not sync.connect():
        sys.exit(1)

    try:
        if args.status:
            sessions = sync.list_sessions()
            print(f"\nSessions on device: {len(sessions)}")
            for s in sessions:
                sid = s.get("id", "?")
                nfiles = s.get("files", "?")
                nbytes = s.get("size", 0)
                print(f"  {sid}  {nfiles} file(s)  {_fmt_bytes(nbytes)}")
            sys.exit(0)

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
