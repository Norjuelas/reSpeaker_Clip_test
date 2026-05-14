#!/usr/bin/env python3
"""Test ReSpeaker Clip MCUboot factory erase commands.

Requires: pip install smpclient

Usage:
    python test_clip_erase.py --port COM3 settings
    python test_clip_erase.py --port COM3 sd
    python test_clip_erase.py --port COM3 all
"""

import asyncio
import argparse
from smpclient import SMPClient
from smpclient.transport.serial import SMPSerialTransport

GROUP_ID = 64
CMD_ERASE_SD = 0
CMD_ERASE_SETTINGS = 1

async def send_command(port, command_id):
    async with SMPClient(SMPSerialTransport(), port) as client:
        response = await client.request(GROUP_ID, command_id, 2, {})
        print(f"Response: {response}")

async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("command", choices=["settings", "sd", "all"])
    args = parser.parse_args()

    if args.command in ("settings", "all"):
        print("Erasing settings...")
        await send_command(args.port, CMD_ERASE_SETTINGS)

    if args.command in ("sd", "all"):
        print("Erasing SD card...")
        await send_command(args.port, CMD_ERASE_SD)

if __name__ == "__main__":
    asyncio.run(main())
