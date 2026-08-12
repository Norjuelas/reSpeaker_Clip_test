#!/usr/bin/env python3
"""Manda comandos AT al Clip por UDP, sin cable.

Sirve para pilotar el device en modo estacion. Tambien demuestra el problema:
no hay autenticacion de ninguna clase — cualquiera en la red puede mandar
AT+FACTORY, AT+FORMAT o AT+POWEROFF.
"""
import socket, sys, json

IP = "192.168.2.34"
PORT = 8089

def at(cmd, timeout=8, tries=4):
    """UDP pierde paquetes, y mas con el device grabando y la radio en ahorro
    de energia. Un solo envio falla la mitad de las veces; con reintentos el
    canal es perfectamente usable."""
    for i in range(tries):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        try:
            s.sendto(cmd.encode() + b"\r\n", (IP, PORT))
            data, _ = s.recvfrom(4096)
            # trama: tipo(1) + longitud(2) + carga
            body = data[3:] if data[:1] == b"\x20" else data
            return body.decode(errors="replace").strip()
        except socket.timeout:
            continue
        finally:
            s.close()
    return "(sin respuesta tras %d intentos)" % tries

def js(t):
    try: return json.loads(t)
    except json.JSONDecodeError: return {}

if __name__ == "__main__":
    for c in sys.argv[1:]:
        print(f"{c:<28} {at(c)}")
