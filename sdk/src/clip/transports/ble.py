"""BLE GATT transport for the reSpeaker Clip Nordic UART-style service."""

from __future__ import annotations

import asyncio
from typing import Any

from ..exceptions import CommandTimeoutError, ConnectionError, ProtocolError
from ..protocol import (
    AUDIO_VIS_UUID,
    COMMAND_UUID,
    FILE_DATA_UUID,
    RESPONSE_UUID,
    JsonNotificationDecoder,
)
from .base import BaseTransport


class BleTransport(BaseTransport):
    """BLE transport using the optional `bleak` dependency.

    Commands are serialized here as well as by :class:`clip.ClipClient`.  The
    Clip protocol has no request identifier, therefore a timed-out command
    invalidates the transport until the caller reconnects; otherwise a late
    response could be incorrectly assigned to a later command.
    """

    def __init__(
        self,
        address: str | None = None,
        *,
        name: str = "Clip",
        connect_timeout: float = 15.0,
    ) -> None:
        super().__init__()
        self.address = address
        self.name = name
        self.connect_timeout = connect_timeout
        self._client: Any | None = None
        self._responses: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._command_lock = asyncio.Lock()
        self._decoder = JsonNotificationDecoder()
        self._connected = False
        self._desynchronized = False

    @property
    def is_connected(self) -> bool:
        return bool(self._connected and self._client is not None and self._client.is_connected)

    async def connect(self) -> None:
        if self.is_connected:
            return
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError as exc:
            raise ConnectionError("BLE support requires: pip install 'respeaker-clip-sdk[ble]'") from exc

        address = self.address
        if address is None:
            device = await BleakScanner.find_device_by_filter(
                lambda candidate, _advertisement: bool(candidate.name and self.name in candidate.name),
                timeout=self.connect_timeout,
            )
            if device is None:
                raise ConnectionError(f"no BLE device with name containing {self.name!r} found")
            address = device.address
            self.address = address

        client = BleakClient(address, timeout=self.connect_timeout)
        try:
            await client.connect()
            await client.start_notify(RESPONSE_UUID, self._on_response_notification)
            await client.start_notify(FILE_DATA_UUID, self._on_file_notification)
            # Audio visualization is intentionally not required for command/file
            # operation.  Users that need it can add a callback in a later API.
        except Exception as exc:
            try:
                if client.is_connected:
                    await client.disconnect()
            finally:
                pass
            raise ConnectionError(f"BLE connection failed: {exc}") from exc

        self._client = client
        self._connected = True
        self._desynchronized = False
        self._decoder = JsonNotificationDecoder()
        self._drain_responses()

    async def disconnect(self) -> None:
        client = self._client
        self._connected = False
        self._client = None
        self._desynchronized = False
        self._drain_responses()
        if client is None:
            return
        try:
            if client.is_connected:
                for uuid in (RESPONSE_UUID, FILE_DATA_UUID):
                    try:
                        await client.stop_notify(uuid)
                    except Exception:
                        pass
                await client.disconnect()
        except Exception as exc:
            raise ConnectionError(f"BLE disconnect failed: {exc}") from exc

    async def send_command(self, command: str, *, timeout: float) -> dict[str, Any]:
        async with self._command_lock:
            if not self.is_connected:
                raise ConnectionError("BLE transport is not connected")
            if self._desynchronized:
                raise ConnectionError("a previous BLE command timed out; disconnect and reconnect before retrying")
            self._drain_responses()
            assert self._client is not None
            try:
                await self._client.write_gatt_char(COMMAND_UUID, command.encode("utf-8"), response=False)
                return await asyncio.wait_for(self._responses.get(), timeout=timeout)
            except asyncio.TimeoutError as exc:
                self._desynchronized = True
                raise CommandTimeoutError(f"no response to {command}") from exc
            except CommandTimeoutError:
                self._desynchronized = True
                raise
            except Exception as exc:
                raise ConnectionError(f"BLE command write failed: {exc}") from exc

    def _on_response_notification(self, _sender: Any, data: bytearray) -> None:
        try:
            for response in self._decoder.feed(bytes(data)):
                if "event" in response:
                    self._emit_event(response)
                else:
                    self._responses.put_nowait(response)
        except ProtocolError:
            # No request can safely continue after an undecodable response.
            self._desynchronized = True

    def _on_file_notification(self, _sender: Any, data: bytearray) -> None:
        self._emit_file_frame(bytes(data))

    def _drain_responses(self) -> None:
        while True:
            try:
                self._responses.get_nowait()
            except asyncio.QueueEmpty:
                return
