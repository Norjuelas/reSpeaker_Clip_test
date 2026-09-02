#!/usr/bin/env python3
"""Cliente AT por USB CDC. Uso:  at.py "AT+DEVICE" "AT+BATT?" ...

Existe porque la campana de bateria consulta el device decenas de veces y
hacerlo a mano con un terminal serie pierde la respuesta: hay ordenes que
tardan mas de 30 s (AT+DELETE=all sobre ~100 sesiones) y una ventana de
lectura corta las corta por la mitad sin decir que las corto.

--json imprime solo el campo data de cada respuesta, para encadenar con jq.
"""
import argparse
import json
import sys
import time

import serial

DEFAULT_PORT = "/dev/ttyACM0"


def send(ser, cmd, timeout):
    """Manda una orden y devuelve (texto_crudo, dict_o_None)."""
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()

    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            # La respuesta es una linea JSON. Se corta al tener un objeto
            # completo y no al primer \n: los logs del device se intercalan.
            for line in buf.split(b"\n"):
                line = line.strip()
                if line.startswith(b"{") and line.endswith(b"}"):
                    try:
                        return line.decode(errors="replace"), json.loads(line)
                    except json.JSONDecodeError:
                        pass
        else:
            time.sleep(0.05)
    return buf.decode(errors="replace"), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmds", nargs="+")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=float, default=8.0,
                    help="segundos por orden (subir para DELETE/FORMAT)")
    ap.add_argument("--json", action="store_true",
                    help="solo el campo data, una linea JSON por orden")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, 115200, timeout=0.2)
    except serial.SerialException as e:
        print(f"no se pudo abrir {args.port}: {e}", file=sys.stderr)
        return 2

    # El device puede estar dormido; una linea vacia lo despierta sin efecto.
    ser.write(b"\n")
    time.sleep(0.2)

    rc = 0
    with ser:
        for cmd in args.cmds:
            raw, obj = send(ser, cmd, args.timeout)
            if obj is None:
                print(f"{cmd}  -> SIN RESPUESTA en {args.timeout}s"
                      f"{'  crudo: ' + raw.strip()[:200] if raw.strip() else ''}")
                rc = 1
                continue
            # AT+DEVICE devuelve sus campos en la raiz, no bajo "data", pese
            # a que el contrato de at_server.c dice {"ok":..,"data":..}. Si no
            # hay "data" se usa el resto del objeto en vez de imprimir null.
            def payload_of(o):
                if "data" in o:
                    return o["data"]
                rest = {k: v for k, v in o.items() if k not in ("ok", "msg")}
                return rest or None

            if args.json:
                print(json.dumps(payload_of(obj)))
            else:
                ok = obj.get("ok")
                mark = "ok " if ok else "ERR"
                payload = payload_of(obj) if ok else obj.get("msg")
                print(f"{mark} {cmd}")
                print(f"    {json.dumps(payload, ensure_ascii=False)}")
            if not obj.get("ok"):
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
