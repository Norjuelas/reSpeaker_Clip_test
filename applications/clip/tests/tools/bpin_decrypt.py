#!/usr/bin/env python3
"""Descifra el audio en reposo del device (formato BPE2, Doc 09).

Formato en disco:
    cabecera (18 B): b"BPE2" | version 0x02 | keyid | nonce_base (12 B)
    trozo:           u16-LE len | ciphertext+tag (len bytes, tag = ultimos 16)

Nonce del trozo i: nonce_base con i (u32 BE) XOR en los ultimos 4 bytes.

Un fichero truncado por un corte de energia pierde solo el trozo final
incompleto: los completos se descifran y AUTENTICAN individualmente.

Uso:
    bpin_decrypt.py --key <32 hex> entrada.opus [salida.opus]

Necesita el paquete `cryptography` (el python del toolchain de NCS lo trae).
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.exceptions import InvalidTag
except ImportError:
    sys.exit("Falta el paquete 'cryptography' (pip install cryptography, "
             "o usar el python del toolchain de NCS que ya lo trae)")

MAGIC = b"BPE2"
HDR_LEN = 18
TAG_LEN = 16


def es_bpe2(data: bytes) -> bool:
    return len(data) >= HDR_LEN and data[:4] == MAGIC


def descifrar(data: bytes, key: bytes):
    """Devuelve (plaintext, avisos). Lanza ValueError si no es BPE2 o la
    clave no corresponde (fallo de autenticacion en el primer trozo)."""
    if not es_bpe2(data):
        raise ValueError("No es un contenedor BPE2")
    version, keyid = data[4], data[5]
    if version != 0x02:
        raise ValueError(f"Version BPE2 desconocida: {version}")
    nonce_base = data[6:HDR_LEN]

    gcm = AESGCM(key)
    out = bytearray()
    avisos = []
    off = HDR_LEN
    i = 0
    while off < len(data):
        if off + 2 > len(data):
            avisos.append(f"trozo {i}: prefijo truncado (corte de energia)")
            break
        (ct_len,) = struct.unpack_from("<H", data, off)
        off += 2
        if off + ct_len > len(data):
            avisos.append(f"trozo {i}: incompleto ({len(data)-off}/{ct_len} B), descartado")
            break
        ct = data[off:off + ct_len]
        off += ct_len

        nonce = bytearray(nonce_base)
        for j, b in enumerate(struct.pack(">I", i)):
            nonce[8 + j] ^= b
        try:
            out += gcm.decrypt(bytes(nonce), ct, None)
        except InvalidTag:
            if i == 0:
                raise ValueError("Autenticacion fallida en el primer trozo: "
                                 "clave incorrecta o fichero manipulado")
            avisos.append(f"trozo {i}: autenticacion fallida (tarjeta alterada?), descartado")
        i += 1

    return bytes(out), avisos


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--key", required=True, help="clave AES-128 en hex (32 chars)")
    ap.add_argument("entrada", type=Path)
    ap.add_argument("salida", type=Path, nargs="?")
    args = ap.parse_args()

    key = bytes.fromhex(args.key)
    if len(key) != 16:
        sys.exit("La clave debe ser AES-128: 32 caracteres hex")

    data = args.entrada.read_bytes()
    plaintext, avisos = descifrar(data, key)
    for a in avisos:
        print(f"aviso: {a}", file=sys.stderr)

    salida = args.salida or args.entrada.with_suffix(".dec.opus")
    salida.write_bytes(plaintext)
    print(f"{args.entrada} -> {salida} ({len(plaintext)} B en claro)")


if __name__ == "__main__":
    main()
