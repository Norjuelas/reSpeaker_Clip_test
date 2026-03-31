#!/usr/bin/env python3
"""
UDP Sync Tool for Clip

Downloads recording sessions via UDP with per-file CRC32 verification.

WiFi AP Configuration:
  SSID:     ClipAP_XXXX  (XXXX = last 4 hex digits of chip ID)
  Password: 12345678
  IP:       192.168.4.1
  UDP Port: 8089

Usage:
  python udp_sync.py                          # list sessions
  python udp_sync.py --all-sessions           # download all sessions
  python udp_sync.py --session 20260326120000 # download one session
  python udp_sync.py --session 20260326120000 --file 0015.opus  # start from file
  python udp_sync.py --session 20260326120000 --delete          # delete after sync
"""

import sys
import argparse
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from clip.wifi import WiFiSync
from clip.utils import format_bytes

DEFAULT_HOST = "192.168.4.1"
DEFAULT_PORT = 8089


def main():
    parser = argparse.ArgumentParser(description="UDP Sync Tool for Clip")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--session", "-s", help="Session ID to download")
    parser.add_argument("--all-sessions", "-a", action="store_true", help="Download all sessions")
    parser.add_argument("--output", "-o", type=Path, default=Path("recordings"),
                       help="Output directory (default: recordings/)")
    parser.add_argument("--file", "-f", help="Start from specific file (e.g., 0015.opus)")
    parser.add_argument("--delete", action="store_true",
                       help="Delete session from device after sync")
    parser.add_argument("--no-ogg", action="store_true",
                       help="Skip OGG conversion")
    args = parser.parse_args()

    sync = WiFiSync(args.host, args.port, args.timeout)
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
                sid = s.get('id', '?')
                size = s.get('size', 0)
                files = s.get('files', 0)
                print(f"  {sid}: {files} files, {format_bytes(size)}")
            print("\nUse --session or --all-sessions to download")
            sys.exit(0)

        print(f"\nOutput: {args.output}")
        ok = 0
        for idx, s in enumerate(sessions_to_sync, 1):
            sid = s.get("id", "?")
            print(f"\n[{idx}/{len(sessions_to_sync)}] {sid}")
            if sync.download_session(
                sid, args.output,
                convert_ogg=not args.no_ogg,
                start_file=args.file,
                delete_after=args.delete,
            ):
                ok += 1
            else:
                print(f"  Failed")

        print(f"\nDone: {ok}/{len(sessions_to_sync)}")

    except KeyboardInterrupt:
        print("\nInterrupted, stopping transfer...")
        try:
            sync._send_at_command("CANCEL")
        except Exception:
            pass
    except Exception as e:
        print(f"\nError: {e}")
    finally:
        sync.disconnect()


if __name__ == "__main__":
    main()
