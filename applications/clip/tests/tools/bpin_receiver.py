#!/usr/bin/env python3
"""Receptor de audios del reSpeaker Clip para BPIN.

Escucha en UDP y habla el protocolo de transferencia del Clip
(ver docs/udp_protocol.md), escribiendo cada .opus recibido a disco.

Se diferencia de udp_sync.py en el sentido de la conexion: udp_sync.py se
conecta al AP del Clip y le pide archivos. Aqui el Clip esta en modo estacion,
dentro de la red local, y es el quien empuja los archivos hacia este proceso.

    python3 bpin_receiver.py --out ./recordings

Fase 3b: cuando el firmware hable HTTP, este mismo modulo se reusa cambiando
solo la capa de transporte — save_file() y la verificacion CRC no cambian.
"""

import argparse
import binascii
import datetime
import socket
import struct
import sys
from pathlib import Path

# Tipos de frame (docs/udp_protocol.md). Todo little-endian.
FRAME_DATA = 0x01
FRAME_FILE_ACK = 0x03
FRAME_FILE_START = 0x10
FRAME_FILE_END = 0x11
FRAME_TRANSFER_DONE = 0x12
FRAME_AT_RESP = 0x20
FRAME_HEARTBEAT = 0x30

ACK_OK = 0x00
ACK_CRC_MISMATCH = 0x01

DATA_HEADER_LEN = 9
MAX_PAYLOAD = 1024
RECV_BUF = DATA_HEADER_LEN + MAX_PAYLOAD + 64


class Transfer:
    """Un archivo en vuelo: acumula payloads por numero de secuencia."""

    def __init__(self, filename, size):
        self.filename = filename
        self.size = size
        self.chunks = {}  # seq -> bytes
        self.started = datetime.datetime.now()

    @property
    def received_bytes(self):
        return sum(len(c) for c in self.chunks.values())

    def add(self, seq, payload):
        self.chunks[seq] = payload

    def assemble(self):
        """Concatena en orden de seq. Devuelve (datos, lista_de_huecos)."""
        if not self.chunks:
            return b"", []
        highest = max(self.chunks)
        missing = [s for s in range(highest + 1) if s not in self.chunks]
        data = b"".join(self.chunks[s] for s in sorted(self.chunks))
        return data, missing

    def missing_bitmap(self, total_seqs):
        """Bitmap de selective-repeat: bit i puesto = falta el seq i."""
        bitmap = bytearray((total_seqs + 7) // 8)
        for seq in range(total_seqs):
            if seq not in self.chunks:
                bitmap[seq // 8] |= 1 << (seq % 8)
        return bytes(bitmap)


def log(msg):
    stamp = datetime.datetime.now().strftime("%H:%M:%S")
    print(f"[{stamp}] {msg}", flush=True)


def save_file(out_dir, session, transfer, data):
    """Escribe el archivo recibido. Comun a UDP y a un futuro POST HTTP."""
    target_dir = out_dir / (session or "unsorted")
    target_dir.mkdir(parents=True, exist_ok=True)

    # Nombre plano: el firmware manda 0001.opus, que se repite entre sesiones.
    safe = Path(transfer.filename).name or "unnamed.opus"
    path = target_dir / safe

    if path.exists():
        stem, suffix = path.stem, path.suffix
        n = 1
        while path.exists():
            path = target_dir / f"{stem}_{n}{suffix}"
            n += 1

    path.write_bytes(data)
    return path


def handle_datagram(state, sock, peer, packet, out_dir):
    frame_type = packet[0]

    if frame_type == FRAME_FILE_START:
        fn_len = packet[1]
        filename = packet[2 : 2 + fn_len].decode("utf-8", errors="replace")
        (size,) = struct.unpack_from("<I", packet, 2 + fn_len)
        state["transfer"] = Transfer(filename, size)
        log(f"FILE_START  {filename} ({size} bytes)")

    elif frame_type == FRAME_DATA:
        transfer = state.get("transfer")
        if transfer is None:
            return  # DATA sin FILE_START: sobra de una transferencia abortada
        seq, length, crc = struct.unpack_from("<HHI", packet, 1)
        payload = packet[DATA_HEADER_LEN : DATA_HEADER_LEN + length]
        if binascii.crc32(payload) & 0xFFFFFFFF != crc:
            # Se descarta: el CRC de archivo completo en FILE_END lo detecta
            # y dispara la reparacion selectiva.
            log(f"  seq {seq}: CRC de frame malo, descartado")
            return
        transfer.add(seq, payload)

    elif frame_type == FRAME_FILE_END:
        transfer = state.get("transfer")
        if transfer is None:
            return
        (expected_crc,) = struct.unpack_from("<I", packet, 1)
        data, missing = transfer.assemble()
        actual_crc = binascii.crc32(data) & 0xFFFFFFFF

        if actual_crc == expected_crc and not missing:
            path = save_file(out_dir, state.get("session"), transfer, data)
            elapsed = (datetime.datetime.now() - transfer.started).total_seconds()
            rate = len(data) / elapsed / 1024 if elapsed > 0 else 0
            log(f"FILE_END    OK -> {path} ({len(data)} B, {rate:.1f} KB/s)")
            sock.sendto(bytes([FRAME_FILE_ACK, ACK_OK]), peer)
            state["transfer"] = None
        else:
            # Selective repeat: se pide solo lo que falta. total_seqs se estima
            # con el seq mas alto visto; si se perdio la cola, el reintento
            # siguiente la corrige.
            highest = max(transfer.chunks) if transfer.chunks else 0
            total_seqs = highest + 1
            bitmap = transfer.missing_bitmap(total_seqs)
            log(
                f"FILE_END    CRC {actual_crc:08x} != {expected_crc:08x}, "
                f"faltan {len(missing)}/{total_seqs} frames — pidiendo reenvio"
            )
            sock.sendto(
                bytes([FRAME_FILE_ACK, ACK_CRC_MISMATCH])
                + struct.pack("<H", total_seqs)
                + bitmap,
                peer,
            )

    elif frame_type == FRAME_TRANSFER_DONE:
        sid_len = packet[1]
        session = packet[2 : 2 + sid_len].decode("utf-8", errors="replace")
        (count,) = struct.unpack_from("<I", packet, 2 + sid_len)
        log(f"DONE        sesion {session}: {count} archivos")
        state["session"] = session

    elif frame_type == FRAME_AT_RESP:
        (length,) = struct.unpack_from("<H", packet, 1)
        log(f"AT_RESP     {packet[3 : 3 + length].decode('utf-8', errors='replace')}")

    elif frame_type == FRAME_HEARTBEAT:
        sock.sendto(packet, peer)  # eco, para que el device sepa que seguimos

    else:
        log(f"frame desconocido 0x{frame_type:02x} ({len(packet)} bytes)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8089,
                    help="puerto UDP a escuchar (default: 8089, el del firmware)")
    ap.add_argument("--bind", default="0.0.0.0",
                    help="interfaz donde escuchar (default: todas)")
    ap.add_argument("--out", type=Path, default=Path("./recordings"),
                    help="directorio de salida (default: ./recordings)")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))

    log(f"Escuchando en {args.bind}:{args.port} -> {args.out.resolve()}")
    log("Apunta el Clip aqui y lanza la transferencia. Ctrl-C para salir.")

    state = {"transfer": None, "session": None}
    peers_seen = set()

    try:
        while True:
            packet, peer = sock.recvfrom(RECV_BUF)
            if not packet:
                continue
            if peer not in peers_seen:
                log(f"Primer contacto desde {peer[0]}:{peer[1]}")
                peers_seen.add(peer)
            try:
                handle_datagram(state, sock, peer, packet, args.out)
            except (struct.error, IndexError) as exc:
                log(f"frame malformado de {peer[0]}: {exc}")
    except KeyboardInterrupt:
        log("Cerrando.")
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
