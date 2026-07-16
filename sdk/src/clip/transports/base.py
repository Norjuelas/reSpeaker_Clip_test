"""Common asynchronous transport contract."""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Callable
from typing import Any


FileFrameHandler = Callable[[bytes], None]
EventHandler = Callable[[dict[str, Any]], None]


class BaseTransport(ABC):
    """A connection that carries AT JSON responses and binary file frames."""

    def __init__(self) -> None:
        self._file_frame_handler: FileFrameHandler | None = None
        self._event_handler: EventHandler | None = None

    @property
    @abstractmethod
    def is_connected(self) -> bool:
        """Whether the underlying connection is currently usable."""

    @abstractmethod
    async def connect(self) -> None:
        """Open the transport and enable its receive paths."""

    @abstractmethod
    async def disconnect(self) -> None:
        """Close the transport and release its resources."""

    @abstractmethod
    async def send_command(self, command: str, *, timeout: float) -> dict[str, Any]:
        """Send one complete AT command and wait for its JSON response."""

    def set_file_frame_handler(self, handler: FileFrameHandler | None) -> None:
        self._file_frame_handler = handler

    def set_event_handler(self, handler: EventHandler | None) -> None:
        self._event_handler = handler

    def send_file_ack(self, success: bool) -> None:
        """Acknowledge a UDP file CRC; BLE has no application-level ACK."""

    @property
    def uses_udp_file_frames(self) -> bool:
        """Whether DATA frames include the UDP per-datagram CRC32 field."""
        return False

    def _emit_file_frame(self, frame: bytes) -> None:
        if self._file_frame_handler is not None:
            self._file_frame_handler(frame)

    def _emit_event(self, event: dict[str, Any]) -> None:
        if self._event_handler is not None:
            self._event_handler(event)
