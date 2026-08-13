"""Configuracion por cable y flasheo, para el panel.

Separado del receptor a proposito: el receptor recibe audio de la red y no
deberia tener nada que ver con lo que puede reescribir el firmware. Aqui
tambien: estas funciones solo se sirven en localhost.

Cubre lo que en tienda se hace una vez, con el device en la mano y un cable:
poner la red WiFi, poner el endpoint, y dejar instalado el firmware correcto.
"""

import glob
import json
import subprocess
import time
from pathlib import Path

# tools -> tests -> clip -> applications -> raiz del repo
REPO = Path(__file__).resolve().parents[4]


def serial_ports():
    """Los puertos que parecen un Clip. cu.* y no tty.*: en macOS tty.* bloquea
    esperando portadora."""
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def _at(port, cmd, wait=3.0):
    import serial
    with serial.Serial(port, 115200, timeout=1) as s:
        s.reset_input_buffer()
        s.write(cmd.encode() + b"\r\n")
        t0, buf = time.time(), b""
        while time.time() - t0 < wait:
            c = s.read(600)
            if c:
                buf += c
                if b"\n" in c:
                    break
        return (buf or b"").decode(errors="replace").strip()


def device_info(port):
    """Que firmware lleva puesto y donde esta. Sin esto, saber que hay en un
    device concreto es abrirlo o adivinarlo."""
    out = {"port": port}
    try:
        v = json.loads(_at(port, "AT+VERSION") or "{}")
        out["firmware"] = v.get("firmware", "?")
    except Exception as e:
        return {"port": port, "error": str(e)}
    for cmd, key in (("AT+STA?", "sta"), ("AT+HEALTH?", "health")):
        try:
            out[key] = json.loads(_at(port, cmd) or "{}").get("data", {})
        except Exception:
            out[key] = {}
    return out


def configure(port, ssid, psk, host, upload_port):
    """La configuracion de entrega: red y endpoint, por cable.

    No se registra la contrasena en ningun log ni se devuelve al navegador —
    entra, va al device y se olvida.
    """
    steps = []
    if ssid:
        r = _at(port, f'AT+STACFG="{ssid}","{psk}"', wait=5)
        steps.append({"paso": "wifi", "ssid": ssid,
                      "ok": '"ok":true' in r, "respuesta": r[:120]})
    if host:
        r = _at(port, f'AT+UPCFG="{host}",{int(upload_port)}', wait=5)
        steps.append({"paso": "endpoint", "ok": '"ok":true' in r,
                      "respuesta": r[:120]})
    if ssid:
        r = _at(port, "AT+STA=on", wait=8)
        steps.append({"paso": "conectar", "ok": '"ok":true' in r,
                      "respuesta": r[:120]})
    return steps


def builds():
    """Los binarios que hay, cruzados con las etiquetas de git.

    Un binario suelto en un directorio de build no dice de que version es. Las
    etiquetas si, y son las que se decidieron como puntos de retorno.
    """
    out = []
    try:
        tags = subprocess.run(["git", "tag", "--sort=-creatordate"],
                              cwd=REPO, capture_output=True, text=True,
                              timeout=10).stdout.split()
    except Exception:
        tags = []

    for p in sorted(REPO.glob("output/hitos/*.bin")) + \
             sorted(REPO.glob("build-*/clip/zephyr/zephyr.signed.bin")):
        rel = str(p.relative_to(REPO))
        match = next((t for t in tags if t.replace("fw-", "") in p.name), None)
        out.append({
            "ruta": rel,
            "nombre": p.name,
            "bytes": p.stat().st_size,
            "modificado": time.strftime("%Y-%m-%d %H:%M",
                                        time.localtime(p.stat().st_mtime)),
            "tag": match or ("(sin etiqueta)" if "output/hitos" in rel
                             else "(build de trabajo)"),
        })
    return {"builds": out, "tags": tags[:12]}


def flash(binary_rel, port):
    """Instala un binario por recovery serie.

    El device tiene que estar ya en modo recovery — no se entra solo. Se intenta
    AT+DFU primero por si esta arrancado; si no responde, se asume que ya lo
    esta.
    """
    path = (REPO / binary_rel).resolve()
    if not str(path).startswith(str(REPO)) or not path.exists():
        return {"ok": False, "error": "ruta fuera del repositorio o inexistente"}

    log = []
    try:
        _at(port, "AT+DFU", wait=2)
        log.append("AT+DFU enviado; esperando a que enumere en recovery")
        time.sleep(8)
    except Exception:
        log.append("sin respuesta a AT+DFU: se asume que ya estaba en recovery")

    tty = port.replace("/dev/cu.", "/dev/tty.")
    try:
        r = subprocess.run(
            ["nrfutil", "mcu-manager", "serial", "image-upload",
             "--firmware", str(path), "--serial-port", tty],
            capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            return {"ok": False, "log": log, "error": (r.stderr or r.stdout)[-400:]}
        log.append("imagen subida")
        subprocess.run(["nrfutil", "mcu-manager", "serial", "reset",
                        "--serial-port", tty], capture_output=True, timeout=60)
        log.append("reiniciado")
        return {"ok": True, "log": log}
    except subprocess.TimeoutExpired:
        return {"ok": False, "log": log, "error": "nrfutil no termino a tiempo"}
    except FileNotFoundError:
        return {"ok": False, "log": log, "error": "nrfutil no esta en el PATH"}
