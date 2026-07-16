from __future__ import annotations

import struct
import zlib

import pytest

from clip.exceptions import ProtocolError
from clip.protocol import (
    DataFrame,
    FileEndFrame,
    FileStartFrame,
    JsonNotificationDecoder,
    TransferDoneFrame,
    decode_file_frame,
)


def test_json_decoder_handles_split_and_multiple_ble_notifications() -> None:
    decoder = JsonNotificationDecoder()
    assert decoder.feed(b'{"ok":tr') == []
    assert decoder.feed(b'ue}\n{"event":"state"}\n') == [
        {"ok": True},
        {"event": "state"},
    ]


def test_decodes_all_ble_file_frame_types() -> None:
    payload = b"abc"
    data = b"\x01" + struct.pack("<HH", 7, len(payload)) + payload
    assert decode_file_frame(data) == DataFrame(sequence=7, payload=payload)

    name = b"0001.opus"
    start = b"\x10" + bytes((len(name),)) + name + struct.pack("<I", 123)
    assert decode_file_frame(start) == FileStartFrame(filename="0001.opus", size=123)

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    assert decode_file_frame(b"\x11" + struct.pack("<I", crc)) == FileEndFrame(crc32=crc)

    sid = b"20260716022113"
    done = b"\x12" + bytes((len(sid),)) + sid + struct.pack("<I", 1)
    assert decode_file_frame(done) == TransferDoneFrame(session_id=sid.decode(), file_count=1)


def test_udp_data_frame_crc_is_checked() -> None:
    payload = b"udp payload"
    frame = b"\x01" + struct.pack("<HHI", 0, len(payload), zlib.crc32(payload)) + payload
    assert decode_file_frame(frame, udp=True) == DataFrame(sequence=0, payload=payload)
    with pytest.raises(ProtocolError, match="CRC32"):
        decode_file_frame(frame[:-1] + b"!", udp=True)


@pytest.mark.parametrize("frame", [b"", b"\x01", b"\x10\x00\x00\x00\x00\x00"])
def test_rejects_malformed_frames(frame: bytes) -> None:
    with pytest.raises(ProtocolError):
        decode_file_frame(frame)
