#!/usr/bin/env python3
"""
BLE Terminal for Clip

Interactive terminal for sending AT commands to Clip device via BLE.
Requires: bleak, asyncio
"""

import asyncio
import json
import sys
import threading
import time
from typing import Optional
from bleak import BleakClient, BleakScanner

# UUIDs - Matching the Clip BLE service
SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
CMD_RECV_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
RESP_SEND_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"


class ClipTerminal:
    """Clip BLE Terminal"""

    def __init__(self, address: str):
        self.address = address
        self.client: Optional[BleakClient] = None
        self.cmd_chr = None
        self.resp_chr = None
        self.connected = False
        self.notification_queue = []
        self.notification_event = threading.Event()

    async def connect(self):
        """Connect to the device"""
        print(f"Connecting to {self.address}...")
        self.client = BleakClient(self.address)
        await self.client.connect()
        print(f"Connected!")

        # Get service and characteristics
        services = self.client.services
        for service in services:
            for char in service.characteristics:
                if str(char.uuid).lower() == RESP_SEND_UUID.lower():
                    self.resp_chr = char
                    # Subscribe to notifications
                    await self.client.start_notify(char.uuid, self._resp_notification)
                    print(f"Subscribed to notifications")

        # Find command characteristic
        for service in services:
            if str(service.uuid).lower() == SERVICE_UUID.lower():
                for char in service.characteristics:
                    if str(char.uuid).lower() == CMD_RECV_UUID.lower():
                        self.cmd_chr = char

        if not self.cmd_chr or not self.resp_chr:
            raise Exception("Could not find required characteristics")

        self.connected = True

    def _resp_notification(self, sender, data):
        """Handle notification from response characteristic"""
        try:
            text = data.decode('utf-8').strip()
            self.notification_queue.append(text)
            self.notification_event.set()
        except Exception as e:
            print(f"Notification error: {e}")

    async def disconnect(self):
        """Disconnect from the device"""
        if self.client:
            await self.client.disconnect()
            self.connected = False
            print("Disconnected")

    async def send_command(self, cmd: str, timeout: float = 5.0) -> str:
        """
        Send AT command and wait for response

        Args:
            cmd: AT command string (e.g., "AT+GSTAT")
            timeout: Maximum time to wait for response (seconds)

        Returns:
            Response text
        """
        if not self.client or not self.client.is_connected:
            raise Exception("Not connected")

        # Clear previous notifications
        self.notification_queue.clear()
        self.notification_event.clear()

        # Send command
        print(f"> {cmd}")
        await self.client.write_gatt_char(self.cmd_chr.uuid, cmd.encode())

        # Wait for response
        start_time = time.time()
        while time.time() - start_time < timeout:
            if self.notification_queue:
                response = self.notification_queue.pop(0)
                print(f"< {response}")
                return response
            await asyncio.sleep(0.05)

        return ""  # Timeout, no response


async def scan_for_device(name_prefix: str = "CLIP_", timeout: float = 5.0) -> Optional[str]:
    """Scan for Clip device"""
    print(f"Scanning for {name_prefix}...")
    devices = await BleakScanner.discover(timeout=timeout)
    for device in devices:
        if device.name and device.name.startswith(name_prefix):
            print(f"Found: {device.name} ({device.address})")
            return device.address
    return None


async def interactive_mode(terminal: ClipTerminal):
    """Interactive terminal mode"""
    print("\n" + "="*50)
    print("Clip BLE Terminal - Interactive Mode")
    print("="*50)
    print("Commands:")
    print("  AT+<command>    - Send AT command")
    print("  START           - Alias for AT+START")
    print("  STOP            - Alias for AT+STOP")
    print("  PAUSE           - Alias for AT+PAUSE")
    print("  RESUME          - Alias for AT+RESUME")
    print("  MARK            - Alias for AT+MARK")
    print("  GSTAT           - Alias for AT+GSTAT")
    print("  VERSION         - Alias for AT+VERSION")
    print("  STATUS          - Alias for AT+GSTAT")
    print("  quit/exit       - Exit terminal")
    print("="*50 + "\n")

    # Show initial status
    try:
        await terminal.send_command("AT+GSTAT")
    except Exception as e:
        print(f"Error: {e}")

    while True:
        try:
            # Get input from user
            cmd = input("clip> ").strip()

            if not cmd:
                continue

            # Handle exit commands
            if cmd.lower() in ['quit', 'exit', 'q']:
                print("Exiting...")
                break

            # Handle aliases
            cmd_upper = cmd.upper()
            if cmd_upper == "START":
                cmd = "AT+START"
            elif cmd_upper == "STOP":
                cmd = "AT+STOP"
            elif cmd_upper == "PAUSE":
                cmd = "AT+PAUSE"
            elif cmd_upper == "RESUME":
                cmd = "AT+RESUME"
            elif cmd_upper == "MARK":
                cmd = "AT+MARK"
            elif cmd_upper in ["GSTAT", "STATUS"]:
                cmd = "AT+GSTAT"
            elif cmd_upper == "VERSION":
                cmd = "AT+VERSION"

            # Add AT prefix if not present
            if not cmd.upper().startswith("AT+") and not cmd.upper().startswith("AT "):
                cmd = "AT+" + cmd

            # Send command
            try:
                await terminal.send_command(cmd)
            except Exception as e:
                print(f"Error sending command: {e}")

        except EOFError:
            print("\nExiting...")
            break
        except KeyboardInterrupt:
            print("\nUse 'quit' to exit")
        except Exception as e:
            print(f"Error: {e}")


async def main():
    """Main entry point"""
    address = None

    # Check arguments
    if len(sys.argv) > 1:
        address = sys.argv[1]
    else:
        # Scan for device
        address = await scan_for_device("Clip")
        if not address:
            print("No Clip device found!")
            print("Usage: python ble_terminal.py <BLE_ADDRESS>")
            sys.exit(1)

    print(f"Using device: {address}")

    # Create terminal
    terminal = ClipTerminal(address)

    try:
        # Connect
        await terminal.connect()

        # Enter interactive mode
        await interactive_mode(terminal)

    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
    finally:
        # Disconnect
        await terminal.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
