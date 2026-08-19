#!/usr/bin/env python3
"""Mete el PEM de la CA de flota dentro de src/ca_builtin.c.

    curl -sk https://<ip>/v1/ca/root.pem | python3 tools/gen_ca_builtin.py
    python3 tools/gen_ca_builtin.py ca.pem

Reescribe solo lo que hay entre las marcas @@BPIN_CA_BEGIN@@ y @@BPIN_CA_END@@,
asi que los comentarios del fichero sobreviven a cada rotacion de CA.
"""
import re
import sys
from pathlib import Path

DEST = Path(__file__).resolve().parents[1] / "src" / "ca_builtin.c"


def main() -> int:
    pem = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else sys.stdin.read()
    pem = pem.strip()

    n = pem.count("-----BEGIN CERTIFICATE-----")
    if n == 0:
        sys.exit("no hay ningun certificado PEM en la entrada")
    if n == 1:
        print("aviso: un solo certificado. Se esperaban dos (raiz + intermedia):\n"
              "       si el servidor no manda la intermedia en el handshake, el\n"
              "       device fallara la validacion.", file=sys.stderr)

    # Una cadena C por linea: legible en el diff y sin pasarse del limite de
    # longitud de literal de ningun compilador.
    cuerpo = "\n".join(f'\t"{l}\\n"' for l in pem.splitlines())
    nuevo = f"const char clip_ca_builtin_pem[] =\n{cuerpo};"

    t = DEST.read_text()
    t2, subs = re.subn(
        r"(@@BPIN_CA_BEGIN@@.*?\*/\n).*?(\n/\* @@BPIN_CA_END@@)",
        lambda m: m.group(1) + nuevo + m.group(2),
        t, flags=re.S)
    if subs != 1:
        sys.exit("no se encontraron las marcas @@BPIN_CA_BEGIN@@/@@BPIN_CA_END@@")

    DEST.write_text(t2)
    print(f"{DEST.name}: {n} certificado(s), {len(pem)} bytes")
    print("Recuerda: cambiar la CA compilada exige firmar y reinstalar firmware.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
