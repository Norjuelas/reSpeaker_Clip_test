#!/usr/bin/env python3
"""
TCP AT Command Terminal for reSpeaker Clip WiFi AP

Interactive terminal for sending AT commands over TCP.
Connect your computer to the Clip's WiFi AP first, then run this script.

WiFi AP Configuration:
  SSID:     ClipAP_XXXX  (XXXX = last 4 hex digits of chip ID)
  Password: 12345678
  IP:       192.168.4.1
  TCP Port: 8080

Usage:
  python tcp_terminal.py [--host 192.168.4.1] [--port 8080]
"""

import socket
import sys
import json
import argparse


class TCPTerminal:
    """Interactive TCP terminal for AT commands."""

    def __init__(self, host: str = "192.168.4.1", port: int = 8080, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.running = True
        self._recv_buf = b""

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    def connect(self) -> bool:
        """Connect to the TCP server on the Clip device."""
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect((self.host, self.port))

            # Read the ready banner sent by the device on connect
            banner = self._recv_line()
            if banner:
                try:
                    data = json.loads(banner)
                    device = data.get("device", "unknown")
                    print(f"Connected to device: {device}")
                except json.JSONDecodeError:
                    print(f"Connected  (banner: {banner})")
            else:
                print(f"Connected to {self.host}:{self.port}")
            return True
        except Exception as e:
            print(f"Connection failed: {e}")
            return False

    def disconnect(self):
        """Close the TCP connection."""
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
            print("Disconnected")

    # ------------------------------------------------------------------
    # Low-level I/O
    # ------------------------------------------------------------------

    def _recv_line(self) -> str:
        """Receive bytes until newline from socket; returns stripped line."""
        while b"\n" not in self._recv_buf:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return ""
            if not chunk:
                return ""
            self._recv_buf += chunk

        line, self._recv_buf = self._recv_buf.split(b"\n", 1)
        return line.decode("utf-8", errors="replace").rstrip("\r")

    def _send(self, data: str):
        """Send text over TCP (appends newline)."""
        self.sock.sendall((data + "\n").encode("utf-8"))

    # ------------------------------------------------------------------
    # AT command interface
    # ------------------------------------------------------------------

    def send_command(self, cmd: str) -> dict:
        """Send an AT command and return the parsed JSON response."""
        cmd = cmd.strip()
        if not cmd:
            return {"ok": False, "error": "Empty command"}

        # Add AT+ prefix if not present
        if not cmd.upper().startswith("AT"):
            cmd = f"AT+{cmd}"

        try:
            self._send(cmd)
            line = self._recv_line()
            if not line:
                return {"ok": False, "error": "Timeout waiting for response"}
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                return {"ok": True, "raw": line}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    # ------------------------------------------------------------------
    # Interactive loop
    # ------------------------------------------------------------------

    def run_interactive(self):
        """Main interactive terminal loop."""
        print("=" * 52)
        print("TCP AT Command Terminal  —  reSpeaker Clip")
        print("=" * 52)
        print(f"\nTarget: {self.host}:{self.port}")
        print("\nType AT commands and press Enter.")
        print("Special: quit/exit/q=exit  help=commands  status/version/test")
        print()

        while self.running:
            try:
                raw = input(">> ").strip()
            except EOFError:
                print("\nExiting...")
                break
            except KeyboardInterrupt:
                print()
                continue

            if not raw:
                continue

            if raw.lower() in ("quit", "exit", "q"):
                print("Exiting...")
                break
            elif raw.lower() == "help":
                self._show_help()
            elif raw.lower() == "status":
                self._send_and_show("AT+GSTAT")
            elif raw.lower() == "version":
                self._send_and_show("AT+VERSION")
            elif raw.lower() == "test":
                self._run_test()
            else:
                result = self.send_command(raw)
                print(json.dumps(result, indent=2))
                print()

    def _send_and_show(self, cmd: str):
        """Send command and pretty-print response."""
        result = self.send_command(cmd)
        if result.get("ok"):
            data = result.get("data", result)
            if isinstance(data, dict):
                print(json.dumps(data, indent=2))
            else:
                print(data)
        else:
            print(f"Error: {result.get('error', 'Unknown error')}")
        print()

    def _run_test(self):
        """Run a quick connection test."""
        print("\n--- Connection Test ---")
        for cmd, label in [("AT+GSTAT", "GSTAT"), ("AT+VERSION", "VERSION")]:
            print(f"Sending {cmd}...")
            result = self.send_command(cmd)
            if result.get("ok"):
                print(f"  ✓ {label} OK")
                data = result.get("data", {})
                if isinstance(data, dict):
                    for key, val in list(data.items())[:4]:
                        print(f"    {key}: {val}")
            else:
                print(f"  ✗ Error: {result.get('error')}")
        print()

    def _show_help(self):
        """Print AT command reference."""
        print("""
════════════════════════════════════════════════
AT Commands  (reSpeaker Clip TCP)
════════════════════════════════════════════════

Basic
  AT+VERSION             Firmware version
  AT+GSTAT               Device status (state, battery, …)
  AT+TIME?               Current time
  AT+TIME=<unix_ts>      Set time

Recording
  AT+START               Start recording (default mode)
  AT+START=stereo        Start stereo recording
  AT+START=mono          Start mono+DSP recording
  AT+STOP                Stop recording
  AT+PAUSE               Pause recording
  AT+RESUME              Resume recording
  AT+MARK=<note>         Add bookmark

Configuration
  AT+BITRATE?            Get bitrate
  AT+BITRATE=<bps>       Set bitrate (16000–32000)
  AT+MODE?               Get mode
  AT+MODE=<mode>         Set mode (normal / enhanced)
  AT+COMPLEXITY?         Get encoder complexity
  AT+COMPLEXITY=<0-10>   Set encoder complexity
  AT+CHUNKSIZE?          BLE chunk size

File Operations (over TCP)
  AT+LIST                List all sessions
  AT+LIST=<session>      List files in session
  AT+DOWNLOAD=<session>  Stream session as binary frames
                         (use tcp_sync.py for automated sync)

WiFi
  AT+WIFI?               WiFi AP status
  AT+WIFI=off            Turn off WiFi AP (disconnects you!)

Storage Management
  AT+DELETE=<session>    Delete a session
  AT+PURGE               Delete all sessions
════════════════════════════════════════════════
""")


def main():
    parser = argparse.ArgumentParser(
        description="TCP AT Command Terminal for reSpeaker Clip",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Default (192.168.4.1:8080)
  python tcp_terminal.py

  # Custom host / port
  python tcp_terminal.py --host 192.168.4.10 --port 9090

Steps:
  1. On the device: AT+WIFI=on  (via BLE or button)
  2. Connect your computer to the ClipAP_XXXX WiFi network
  3. Run: python tcp_terminal.py
""")

    parser.add_argument("--host", default="192.168.4.1",
                        help="Device IP address (default: 192.168.4.1)")
    parser.add_argument("--port", type=int, default=8080,
                        help="TCP port (default: 8080)")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="Socket timeout in seconds (default: 5.0)")
    args = parser.parse_args()

    terminal = TCPTerminal(host=args.host, port=args.port, timeout=args.timeout)

    if not terminal.connect():
        sys.exit(1)

    try:
        terminal.run_interactive()
    finally:
        terminal.disconnect()


if __name__ == "__main__":
    main()
