#!/usr/bin/env python3
"""Receptor HTTP de audios del reSpeaker Clip, con panel de salud.

El device hace POST /upload/<session>/<archivo> con el .opus en el cuerpo y su
identificador en la cabecera X-Device-Id.

    python3 bpin_http_receiver.py --out ./recordings

Sin TLS a proposito: es la prueba de concepto. Los audios son conversaciones, y
mandarlos en claro no es defendible en campo — el plan de mTLS con CA de flota
esta en el Doc 13. La ruta a HTTPS esta abierta pero justa: sacar el firmware del
nRF70 dejo ~52KB, y el cliente HTTP con TCP se llevo la mayor parte. Quedan 27KB
contra los ~45KB que cuesta un TLS por defecto.

Subir a S3 es el siguiente paso: se hace en store(), que es el unico sitio que
toca disco.
"""

import argparse
import datetime
import json
import sys
import threading
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

OUT_DIR = Path("./recordings")
MAX_BODY = 8 * 1024 * 1024        # un .opus de una sesion larga no pasa de esto
MAX_JSON = 8 * 1024

# Ultimo latido de cada device, en memoria. No hay base de datos a proposito:
# esto es el banco de pruebas, y un dict basta para ver 100 devices. Cuando
# haya servicio de verdad, esto se sustituye por la tabla que registra tambien
# el certificado de cada device (Doc 13).
DEVICES = {}
DEVICES_LOCK = threading.Lock()

# Las ultimas transferencias, para poder ver que viajo y si llego. Un latido
# cada 5 minutos no cuenta nada de lo que paso en medio.
TRANSFERS = deque(maxlen=200)

# El .opus del Clip son tramas Opus en crudo, sin contenedor: ningun reproductor
# lo abre. Se envuelve en Ogg al recibirlo, que es cuando se sabe que esta
# completo. clip/codec.py lo hace sin dependencias externas.
# Se carga el modulo por ruta, no como paquete: clip/__init__.py arrastra bleak
# (el cliente BLE) y aqui no pinta nada. Importarlo normalmente hacia fallar la
# conversion en silencio, que es la peor forma de fallar.
try:
    import importlib.util as _ilu
    _spec = _ilu.spec_from_file_location(
        "clip_codec", Path(__file__).resolve().parents[1] / "clip" / "codec.py")
    _mod = _ilu.module_from_spec(_spec)
    _spec.loader.exec_module(_mod)
    convert_to_ogg_opus = _mod.convert_to_ogg_opus
except Exception as _e:
    print(f"[aviso] sin conversion a Ogg: {_e}", flush=True)
    convert_to_ogg_opus = None


def log(msg):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


def store(device_id, session, filename, data):
    """Escribe el audio recibido. Aqui es donde ira la subida a S3."""
    # El device controla estos nombres; tratarlos como no confiables antes de
    # construir una ruta con ellos.
    safe_session = Path(session).name or "unknown"
    safe_name = Path(filename).name or "unnamed.opus"

    target = OUT_DIR / (device_id or "unknown-device") / safe_session
    target.mkdir(parents=True, exist_ok=True)

    path = target / safe_name
    if path.exists():
        stem, suffix = path.stem, path.suffix
        n = 1
        while path.exists():
            path = target / f"{stem}_{n}{suffix}"
            n += 1

    path.write_bytes(data)

    # Y una copia reproducible al lado. Si falla, el .opus crudo sigue estando:
    # perder el audio por un problema de empaquetado seria absurdo.
    if convert_to_ogg_opus is not None:
        try:
            convert_to_ogg_opus(path, path.with_suffix(".ogg"))
        except Exception as e:
            log(f"no se pudo envolver en Ogg ({e}); el .opus crudo esta intacto")

    return path



PANEL_HTML = """<!doctype html>
<meta charset="utf-8">
<title>B\u00b7Pin \u2014 devices</title>
<meta http-equiv="refresh" content="15">
<style>
 body{font:14px/1.5 -apple-system,system-ui,sans-serif;margin:2rem;color:#1a1a1a;background:#fafafa}
 h1{font-size:1.2rem;font-weight:600;margin:0 0 .3rem}
 .sub{color:#666;margin-bottom:1.5rem}
 table{border-collapse:collapse;width:100%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.1)}
 th,td{text-align:left;padding:.55rem .8rem;border-bottom:1px solid #eee;white-space:nowrap}
 th{background:#f4f4f4;font-weight:600;font-size:.8rem;text-transform:uppercase;letter-spacing:.03em;color:#555}
 td.id{font-family:ui-monospace,Menlo,monospace;font-size:.85rem}
 .warn{color:#b45309;font-weight:600}
 .bad{color:#b91c1c;font-weight:600}
 .ok{color:#15803d}
 .stale{opacity:.45}
 .test{background:#fde68a;color:#78350f;padding:.05rem .3rem;border-radius:3px;font-size:.7rem;font-weight:600}
 .empty{padding:3rem;text-align:center;color:#888;background:#fff}
 @media (prefers-color-scheme:dark){
  body{background:#16181c;color:#e6e6e6} table{background:#1e2126;box-shadow:none}
  th{background:#24272d;color:#aaa} th,td{border-bottom:1px solid #2c3037} .empty{background:#1e2126}
 }
</style>
<h1>Devices</h1>
<div class="sub">@@COUNT@@ con latido \u00b7 refresco cada 15 s \u00b7 @@NOW@@</div>
@@TABLE@@
<h1 style="margin-top:2rem">Transferencias</h1>
@@XFER@@
"""


def _ago(sec):
    """Cuanto hace del ultimo latido, en palabras."""
    sec = int(sec)
    if sec < 90:
        return f"hace {sec}s"
    if sec < 5400:
        return f"hace {sec // 60} min"
    if sec < 172800:
        return f"hace {sec // 3600} h"
    return f"hace {sec // 86400} dias"


def _fmt_uptime(sec):
    try:
        sec = int(sec)
    except (TypeError, ValueError):
        return "?"
    d, r = divmod(sec, 86400)
    h, r = divmod(r, 3600)
    m = r // 60
    return f"{d}d {h}h" if d else (f"{h}h {m}m" if h else f"{m}m")


DASH = "\u2014"


def _render(count, now, table):
    # Marcadores en vez de % o .format(): el CSS de arriba lleva both, un
    # width:100% y llaves de reglas, y cualquiera de los dos mecanismos se
    # atraganta con el.
    return (PANEL_HTML
            .replace("@@COUNT@@", str(count))
            .replace("@@NOW@@", now.strftime("%H:%M:%S"))
            .replace("@@TABLE@@", table)
            .replace("@@XFER@@", _xfer_html()))


def _xfer_html():
    """Que viajo y si llego. El latido dice como esta el device ahora; esto dice
    que paso en medio, que es lo que se pregunta cuando falta un audio."""
    with DEVICES_LOCK:
        rows = list(TRANSFERS)[:25]
    if not rows:
        return '<div class="empty">Ninguna transferencia todavia.</div>'

    out = ["<table><tr><th>hora</th><th>device</th><th>sesion</th><th>fichero</th>"
           "<th>bytes</th><th>resultado</th></tr>"]
    for t in rows:
        if t["ok"]:
            res = '<span class="ok">llego</span>'
            if t.get("playable"):
                res += ' <small>+ogg</small>'
        else:
            res = f'<span class="bad">fallo</span> <small>{t.get("error","")}</small>'
        out.append(
            f'<tr><td>{t["at"][11:]}</td><td class="id">{t["device"]}</td>'
            f'<td>{t["session"]}</td><td>{t["file"]}</td>'
            f'<td>{t["bytes"]}</td><td>{res}</td></tr>')
    out.append("</table>")
    return "".join(out)


def _panel_html():
    now = datetime.datetime.now()
    with DEVICES_LOCK:
        rows = sorted(DEVICES.items())

    if not rows:
        table = ('<div class="empty">Ning\u00fan device ha latido todav\u00eda.<br>'
                 'Config\u00faralo con <code>AT+UPCFG</code> y <code>AT+HEALTH=now</code>.</div>')
        return _render(0, now, table)

    out = ["<table><tr><th>device</th><th>visto</th><th>uptime</th><th>reinicio</th>"
           "<th>bater\u00eda</th><th>SD libre</th><th>SD usada</th><th>red</th>"
           "<th>estado</th><th>subida</th></tr>"]

    for dev, b in rows:
        seen = datetime.datetime.fromisoformat(b["_seen"])
        age = (now - seen).total_seconds()
        # Dos latidos perdidos y el device se marca apagado: con 300 s de
        # intervalo, 900 s es la se\u00f1al de que algo pasa, no un retraso.
        stale = ' class="stale"' if age > 900 else ""
        tag = ' <span class="test">prueba</span>' if b.get("_test") else ""
        # La antiguedad, no la hora del reloj. Mostrar solo "13:45:16" hacia que
        # un latido de ayer se leyera igual que uno de hace un minuto — paso, y
        # se tomo por dato fresco. El gris atenuado no basta: no sobrevive a
        # copiar la tabla a otro sitio.
        age_cls = "bad" if age > 3600 else ("warn" if age > 900 else "ok")

        pct = b.get("battery_pct", 0)
        bat_cls = "bad" if pct < 15 else ("warn" if pct < 30 else "ok")
        chg = " \u26a1" if b.get("charging") else ""

        free = b.get("sd_free_mb", 0)
        sd_cls = "bad" if free < 50 else ("warn" if free < 200 else "")

        reset = b.get("reset", "?")
        rst_cls = "bad" if reset in ("watchdog", "brownout") else ""

        up_state = b.get("upload_state", "idle")
        up_txt = up_state
        if up_state == "running":
            up_txt = f"{b.get('upload_done',0)}/{b.get('upload_total',0)}"
        elif up_state == "failed":
            up_txt = f'<span class="bad">error {b.get("upload_err","")}</span>'

        out.append(
            f"<tr{stale}>"
            f'<td class="id">{dev}{tag}</td>'
            f'<td class="{age_cls}">{_ago(age)}</td>'
            f"<td>{_fmt_uptime(b.get('uptime_s'))}</td>"
            f'<td class="{rst_cls}">{reset}</td>'
            f'<td class="{bat_cls}">{pct}%{chg} <small>({b.get("battery_mv",0)}mV)</small></td>'
            f'<td class="{sd_cls}">{free} MB</td>'
            f"<td>{b.get('sd_used_mb', 0)} MB</td>"
            f"<td>{b.get('ip') or DASH}</td>"
            f"<td>{b.get('state', '?')}</td>"
            f"<td>{up_txt}</td>"
            f"</tr>")

    out.append("</table>")
    return _render(len(rows), now, "".join(out))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass  # el log propio basta

    def _reply(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _read_body(self, limit):
        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            return None, "bad Content-Length"
        if length <= 0 or length > limit:
            return None, f"Content-Length fuera de rango: {length}"

        # rfile.read(n) puede volver corto; el device manda en trozos de 1KB.
        chunks, got = [], 0
        while got < length:
            block = self.rfile.read(min(65536, length - got))
            if not block:
                break
            chunks.append(block)
            got += len(block)

        if got != length:
            return None, f"cuerpo incompleto: {got}/{length} bytes"
        return b"".join(chunks), None

    def do_POST(self):
        parts = [p for p in self.path.split("/") if p]

        if parts == ["health"]:
            body, err = self._read_body(MAX_JSON)
            if err:
                log(err)
                self._reply(400, err.encode() + b"\n")
                return
            try:
                beat = json.loads(body)
            except json.JSONDecodeError as e:
                log(f"latido ilegible: {e}")
                self._reply(400, b"bad json\n")
                return

            dev = beat.get("device") or self.headers.get("X-Device-Id", "?")
            beat["_seen"] = datetime.datetime.now().isoformat(timespec="seconds")
            # De donde vino. Un latido inyectado a mano para probar el panel se
            # ve igual que uno real, y eso ya confundio una vez: dos devices de
            # prueba pasaron por unidades desplegadas. La cabecera la pone el
            # firmware; lo que no la trae, no es un device.
            beat["_test"] = self.headers.get("X-Device-Id", "") != dev
            with DEVICES_LOCK:
                DEVICES[dev] = beat
            log(f"latido {dev}  bat {beat.get('battery_pct')}%  "
                f"sd {beat.get('sd_free_mb')}MB  {beat.get('state')}")

            # La respuesta es donde iran las ordenes pendientes para el device.
            # Por ahora vacia: el device ya sabe leerla, falta decidir el
            # formato y quien las encola (Doc 14).
            self._reply(200, b'{"commands":[]}\n')
            return

        if len(parts) != 3 or parts[0] != "upload":
            log(f"ruta inesperada: {self.path}")
            self._reply(404, b"expected /upload/<session>/<file>\n")
            return

        _, session, filename = parts
        device_id = self.headers.get("X-Device-Id", "")

        data, err = self._read_body(MAX_BODY)
        if err:
            # Un viaje fallido tiene que verse. Es el caso que importa.
            with DEVICES_LOCK:
                TRANSFERS.appendleft({
                    "at": datetime.datetime.now().isoformat(timespec="seconds"),
                    "device": device_id or "?", "session": session,
                    "file": filename, "bytes": 0, "ok": False, "error": err,
                })
            log(err)
            self._reply(400, err.encode() + b"\n")
            return

        path = store(device_id, session, filename, data)
        with DEVICES_LOCK:
            TRANSFERS.appendleft({
                "at": datetime.datetime.now().isoformat(timespec="seconds"),
                "device": device_id or "?", "session": session,
                "file": filename, "bytes": len(data), "ok": True,
                "playable": path.with_suffix(".ogg").exists(),
            })
        log(f"{device_id or '?'}  {session}/{filename}  {len(data)} bytes -> {path}")
        self._reply(200, b"ok\n")

    def do_GET(self):
        if self.path in ("/", "/panel"):
            body = _panel_html().encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/purge-test":
            with DEVICES_LOCK:
                gone = [k for k, v in DEVICES.items() if v.get("_test")]
                for k in gone:
                    del DEVICES[k]
            log(f"borrados {len(gone)} latidos de prueba")
            self._reply(200, f"borrados {len(gone)}\n".encode())
            return

        if self.path == "/devices.json":
            with DEVICES_LOCK:
                body = json.dumps(DEVICES, indent=2).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # util para comprobar alcanzabilidad desde el propio device
        self._reply(200, b"bpin http receiver\n")


def main():
    global OUT_DIR

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--out", type=Path, default=Path("./recordings"))
    args = ap.parse_args()

    OUT_DIR = args.out
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    log(f"Escuchando en http://{args.bind}:{args.port} -> {OUT_DIR.resolve()}")
    log(f"Panel de salud: http://localhost:{args.port}/")
    log("Configura el device con: AT+UPCFG=\"<ip-de-esta-maquina>\",%d" % args.port)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("Cerrando.")
    finally:
        server.server_close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
