#!/usr/bin/env python3
"""Receptor HTTP de audios del reSpeaker Clip.

El device hace POST /upload/<session>/<archivo> con el .opus en el cuerpo y su
identificador en la cabecera X-Device-Id.

    python3 bpin_http_receiver.py --out ./recordings

Sin TLS a proposito: es la prueba de concepto. Los audios son conversaciones, y
mandarlos en claro no es defendible en campo — el plan de mTLS con CA de flota
esta en el Doc 12. La ruta a HTTPS ya esta abierta: sacar el firmware del nRF70
de la imagen dejo ~52KB libres y TLS mide ~45KB.

Subir a S3 es el siguiente paso: se hace en store(), que es el unico sitio que
toca disco.
"""

import argparse
import datetime
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

OUT_DIR = Path("./recordings")
MAX_BODY = 8 * 1024 * 1024        # un .opus de una sesion larga no pasa de esto


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
    return path


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

    def do_POST(self):
        parts = [p for p in self.path.split("/") if p]

        if len(parts) != 3 or parts[0] != "upload":
            log(f"ruta inesperada: {self.path}")
            self._reply(404, b"expected /upload/<session>/<file>\n")
            return

        _, session, filename = parts
        device_id = self.headers.get("X-Device-Id", "")

        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            self._reply(411, b"bad Content-Length\n")
            return

        if length <= 0 or length > MAX_BODY:
            log(f"Content-Length fuera de rango: {length}")
            self._reply(413, b"body too large\n")
            return

        # rfile.read(n) puede volver corto; el device manda en trozos de 1KB.
        chunks, got = [], 0
        while got < length:
            block = self.rfile.read(min(65536, length - got))
            if not block:
                break
            chunks.append(block)
            got += len(block)

        if got != length:
            log(f"cuerpo incompleto: {got}/{length} bytes")
            self._reply(400, b"short body\n")
            return

        path = store(device_id, session, filename, b"".join(chunks))
        log(f"{device_id or '?'}  {session}/{filename}  {got} bytes -> {path}")
        self._reply(200, b"ok\n")

    def do_GET(self):
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
