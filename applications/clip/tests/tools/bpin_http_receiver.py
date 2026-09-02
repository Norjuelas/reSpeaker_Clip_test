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
import sqlite3
import sys
import threading
from collections import deque
from urllib.parse import parse_qs, urlparse

# Configuracion por cable y flasheo. Opcional: si falta pyserial o nrfutil el
# panel de salud sigue funcionando igual, solo desaparece la pestana.
try:
    import panel_admin
except Exception as _e:
    print(f"[aviso] pestana de configuracion no disponible: {_e}", flush=True)
    panel_admin = None
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

OUT_DIR = Path("./recordings")
MAX_BODY = 8 * 1024 * 1024        # un .opus de una sesion larga no pasa de esto
MAX_JSON = 8 * 1024

# Ultimo latido de cada device, en memoria. Es lo que pinta el panel y lo que
# sirve /devices.json: una foto del ahora, barata de leer y de sobreescribir.
# El historico va aparte, en SQLite (ver mas abajo) — este dict sigue siendo
# exactamente lo que era.
DEVICES = {}
DEVICES_LOCK = threading.Lock()

# ---------------------------------------------------------------------------
# Historico de latidos
#
# Antes esto no existia: el latido llegaba, se sobreescribia DEVICES[dev] y los
# 30 campos del anterior se perdian. Con un latido cada 5 minutos eso significa
# que la bateria, el heap, la pila del hilo de subida y los ficheros pendientes
# se estaban midiendo y tirando.
#
# Y son justo los numeros que solo dicen algo en serie: un device que se
# reinicia por watchdog cada tres horas, pending_files subiendo dia tras dia
# (se graba mas rapido de lo que se sube), heap_free bajando poco a poco (fuga).
# Ninguno se ve en una foto.
#
# Columnas promovidas + el latido entero en `raw`: el payload ya desbordo su
# buffer una vez creciendo, asi que un campo nuevo no puede perderse por no
# tener columna. Las consultas usan las columnas; lo que no se previo, `raw`.
DB = None
DB_LOCK = threading.Lock()

# Campos numericos/texto que se promueven a columna. El resto viaja en `raw`.
# battery_ua en MICROamperios, no en mA: el objetivo de reposo son 170 uA y en
# miliamperios enteros eso es 0, justo el numero que interesa. El firmware lo
# publica asi desde T2.1.1.
DB_COLUMNS = [
    ("uptime_s", "INTEGER"), ("reset", "TEXT"),
    ("battery_pct", "INTEGER"), ("battery_mv", "INTEGER"),
    ("battery_ua", "INTEGER"), ("battery_temp_c", "INTEGER"),
    ("charging", "INTEGER"), ("state", "TEXT"), ("recording", "INTEGER"),
    ("sd_free_mb", "INTEGER"), ("sd_used_mb", "INTEGER"),
    ("wifi", "INTEGER"), ("rssi", "INTEGER"), ("ip", "TEXT"),
    ("wifi_err", "TEXT"), ("heap_free", "INTEGER"),
    ("up_stack_free", "INTEGER"), ("up_kbps", "INTEGER"),
    ("up_ok", "INTEGER"), ("up_fail", "INTEGER"),
    ("pending_files", "INTEGER"), ("upload_state", "TEXT"),
    ("upload_err", "INTEGER"),
]


def _as_int(v):
    """JSON true/false llegan como bool; SQLite no los tiene. None se conserva."""
    if v is None:
        return None
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)):
        return int(v)
    return None


def db_init(path):
    """Abre el historico. Si falla, el receptor sigue sin el."""
    global DB
    cols = ",\n            ".join(f"{n} {t}" for n, t in DB_COLUMNS)
    try:
        # check_same_thread=False: ThreadingHTTPServer atiende cada peticion en
        # un hilo. Una conexion compartida con lock basta y sobra — 100 devices
        # a un latido cada 5 min son 0,3 escrituras por segundo.
        conn = sqlite3.connect(str(path), check_same_thread=False)
        # WAL para que un lector (un script que grafica) no bloquee al escritor.
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute(f"""
            CREATE TABLE IF NOT EXISTS health (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            at TEXT NOT NULL,
            device TEXT NOT NULL,
            is_test INTEGER NOT NULL DEFAULT 0,
            {cols},
            raw TEXT NOT NULL)""")
        # CREATE TABLE IF NOT EXISTS no toca una tabla que ya existe, asi que
        # una columna nueva en DB_COLUMNS no aparecia sola en un historico ya
        # creado y db_record_beat() la descartaba en silencio (no levanta por
        # diseno). Se anaden las que falten -- ALTER TABLE ADD COLUMN en SQLite
        # es instantaneo y las filas viejas quedan con NULL, que es la verdad:
        # ese dato no se midio.
        have = {r[1] for r in conn.execute("PRAGMA table_info(health)")}
        for name, typ in DB_COLUMNS:
            if name not in have:
                conn.execute(f"ALTER TABLE health ADD COLUMN {name} {typ}")
                log(f"historico: columna nueva {name}")
        conn.execute("CREATE INDEX IF NOT EXISTS health_dev_at "
                     "ON health(device, at)")
        conn.commit()
        DB = conn
        n = conn.execute("SELECT COUNT(*) FROM health").fetchone()[0]
        log(f"historico en {path} ({n} latidos ya guardados)")
    except Exception as e:
        log(f"[aviso] sin historico de latidos: {e}")
        DB = None


def db_record_beat(dev, beat):
    """Guarda un latido. Nunca levanta: perder una fila es mejor que perder
    el device — el latido tiene que contestarse 200 igual."""
    if DB is None:
        return
    names = ["at", "device", "is_test"] + [n for n, _ in DB_COLUMNS] + ["raw"]
    vals = [beat.get("_seen"), dev, int(bool(beat.get("_test")))]
    for n, typ in DB_COLUMNS:
        v = beat.get(n)
        vals.append(_as_int(v) if typ == "INTEGER" else v)
    vals.append(json.dumps(beat, separators=(",", ":")))
    try:
        with DB_LOCK:
            DB.execute(
                f"INSERT INTO health ({','.join(names)}) "
                f"VALUES ({','.join('?' * len(names))})", vals)
            DB.commit()
    except Exception as e:
        log(f"[aviso] latido no guardado en el historico: {e}")

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

# Descifrado del audio en reposo (BPE2, Doc 09). Opcional: sin el modulo o
# sin el paquete `cryptography`, el ciphertext se guarda tal cual.
try:
    import importlib.util as _ilu2
    _spec2 = _ilu2.spec_from_file_location(
        "bpin_decrypt", Path(__file__).resolve().parent / "bpin_decrypt.py")
    bpin_decrypt = _ilu2.module_from_spec(_spec2)
    _spec2.loader.exec_module(bpin_decrypt)
except Exception as _e:
    print(f"[aviso] sin descifrado BPE2: {_e}", flush=True)
    bpin_decrypt = None


def log(msg):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


# Claves de audio-en-reposo por chip_id (Doc 09): --keys keys.json con
# {"62518A2B2063EAE0": "<32 hex>", ...}. Sin clave, el ciphertext se guarda
# tal cual y se descifra despues con tools/bpin_decrypt.py.
AUDIO_KEYS = {}


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

    # Audio cifrado en reposo (BPE2): descifrar aqui, con la clave del device.
    # Si no hay clave o falla, se guarda el ciphertext tal cual — recuperable
    # despues con bpin_decrypt.py; perder audio por no poder descifrarlo al
    # vuelo seria absurdo.
    if bpin_decrypt is not None and bpin_decrypt.es_bpe2(data):
        key_hex = AUDIO_KEYS.get(device_id or "")
        if key_hex:
            try:
                plaintext, avisos = bpin_decrypt.descifrar(data, bytes.fromhex(key_hex))
                for a in avisos:
                    log(f"{device_id}: {a}")
                path.with_suffix(".opus.enc").write_bytes(data)
                data = plaintext
            except ValueError as e:
                log(f"{device_id}: descifrado fallido ({e}); se guarda cifrado")
        else:
            log(f"{device_id}: audio cifrado y sin clave en --keys; se guarda cifrado")

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
<nav style="margin-bottom:1rem"><a href="/">Devices</a> &middot; <a href="/config">Configurar por cable</a></nav>
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
            if t.get("playable") and t.get("saved"):
                src = f'/audio/{t["device"]}/{t["session"]}/{t["saved"]}'
                res += (f' <audio controls preload="none" src="{src}" '
                        'style="height:22px;vertical-align:middle"></audio>')
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
           "<th>bater\u00eda</th><th>SD libre</th><th>SD usada</th><th>red</th><th>MAC</th>"
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
            f"<td>{b.get('ip') or (b.get('wifi_err') or DASH)}</td>"
            f'<td class="id">{b.get("mac") or DASH}</td>'
            f"<td class=\"id\">{b.get('mac') or DASH}</td>"
            f"<td>{b.get('state', '?')}</td>"
            f"<td>{up_txt}</td>"
            f"</tr>")

    out.append("</table>")
    return _render(len(rows), now, "".join(out))



ADMIN_HTML = """<!doctype html>
<meta charset="utf-8"><title>B\u00b7Pin \u2014 configurar</title>
<style>
 body{font:14px/1.6 -apple-system,system-ui,sans-serif;margin:2rem;max-width:820px;color:#1a1a1a;background:#fafafa}
 h1{font-size:1.2rem;margin:1.5rem 0 .3rem} h1:first-of-type{margin-top:0}
 .card{background:#fff;padding:1.2rem;box-shadow:0 1px 3px rgba(0,0,0,.1);margin-bottom:1.5rem}
 label{display:block;margin:.6rem 0 .15rem;font-size:.8rem;color:#555;text-transform:uppercase;letter-spacing:.03em}
 input,select{width:100%;padding:.45rem;border:1px solid #ccc;border-radius:3px;font:inherit;background:#fff;color:inherit}
 button{margin-top:1rem;padding:.5rem 1.1rem;border:0;border-radius:3px;background:#1a1a1a;color:#fff;font:inherit;cursor:pointer}
 button.warn{background:#b45309}
 table{border-collapse:collapse;width:100%;font-size:.85rem}
 td,th{text-align:left;padding:.35rem .5rem;border-bottom:1px solid #eee}
 .tag{font-family:ui-monospace,monospace;font-size:.8rem;background:#e0e7ff;color:#3730a3;padding:.05rem .35rem;border-radius:3px}
 .none{color:#999}
 pre{background:#f4f4f4;padding:.7rem;overflow-x:auto;font-size:.8rem;white-space:pre-wrap}
 .sub{color:#666;font-size:.85rem;margin-bottom:.8rem}
 @media (prefers-color-scheme:dark){
  body{background:#16181c;color:#e6e6e6}.card{background:#1e2126;box-shadow:none}
  input,select{background:#24272d;border-color:#3a3f47;color:#e6e6e6}
  button{background:#e6e6e6;color:#16181c}pre{background:#24272d}
  td,th{border-bottom:1px solid #2c3037}.tag{background:#312e81;color:#c7d2fe}}
</style>
<nav style="margin-bottom:1rem"><a href="/">Devices</a> &middot; <a href="/config">Configurar por cable</a></nav>

<h1>Device conectado</h1>
<div class="card"><div id="dev" class="sub">buscando\u2026</div></div>

<h1>Configuraci\u00f3n de entrega</h1>
<div class="card">
 <div class="sub">Lo que se hace una vez, con el device en la mano. En tienda no
 se toca nada m\u00e1s: s\u00f3lo mantenerlo cargado y encendido.</div>
 <label>Puerto</label><select id="port"></select>
 <label>Red WiFi (SSID)</label><input id="ssid" placeholder="Fenix">
 <label>Contrase\u00f1a</label><input id="psk" type="password">
 <label>Servicio \u2014 IP</label><input id="host" placeholder="192.168.2.18">
 <label>Servicio \u2014 puerto</label><input id="uport" value="8080">
 <button onclick="cfg()">Configurar</button>
 <pre id="cfgout" hidden></pre>
</div>

<h1>Logs del device</h1>
<div class="card">
 <div class="sub">Lo unico que dice por que se cayo. Se copian de la microSD a
 una carpeta con fecha, para poder comparar dos arranques.</div>
 <button onclick="getlogs()">Traer logs de la tarjeta</button>
 <label style="display:inline;margin-left:1rem;font-size:.8rem;text-transform:none">
  <input type="checkbox" id="soloerr" style="width:auto" checked> solo errores y avisos</label>
 <div id="logsout"></div>
 <pre id="logview" hidden style="max-height:26rem;overflow:auto"></pre>
</div>

<h1>Firmware</h1>
<div class="card">
 <div class="sub">El device debe estar <b>en modo recovery</b>. Se intenta
 <code>AT+DFU</code> primero; si ya est\u00e1 en recovery, sigue igual.</div>
 <table id="builds"><tr><th>etiqueta</th><th>fichero</th><th>tama\u00f1o</th><th>fecha</th><th></th></tr></table>
 <pre id="flashout" hidden></pre>
</div>

<script>
const $=i=>document.getElementById(i);
const show=(el,t)=>{el.hidden=false;el.textContent=t};

async function getlogs(){
  show($("logsout"),"copiando\u2026");
  const r=await (await fetch("/api/logs/fetch",{method:"POST",body:"{}"})).json();
  if(!r.ok){ $("logsout").innerHTML='<span class="none">'+r.error+'</span>'; return; }
  $("logsout").innerHTML="";
  listlogs();
}

async function listlogs(){
  const r=await (await fetch("/api/logs")).json();
  $("logsout").innerHTML = (r.descargas||[]).length
    ? '<table><tr><th>cuando</th><th>fichero</th><th>tama\u00f1o</th><th></th></tr>'
      + r.descargas.flatMap(d=>d.ficheros.map(f=>
          `<tr><td>${d.cuando}</td><td>${f.nombre}</td>`
          +`<td>${(f.bytes/1024).toFixed(0)} KB</td>`
          +`<td><button onclick="viewlog('${f.ruta}')">ver</button></td></tr>`)).join("")
      + "</table>"
    : '<span class="none">Ninguna descarga todav\u00eda.</span>';
}

async function viewlog(ruta){
  const only=$("soloerr").checked;
  const r=await (await fetch("/api/logs/read",{method:"POST",
    body:JSON.stringify({ruta:ruta,solo_problemas:only})})).json();
  if(!r.ok){ show($("logview"),r.error); return; }
  show($("logview"), r.lineas.length
      ? r.lineas.join("\n")
      : "(sin errores ni avisos en este log)");
}

async function load(){
  const d=await (await fetch("/api/device")).json();
  $("port").innerHTML=(d.ports||[]).map(p=>`<option>${p}</option>`).join("")
    ||"<option>(ningun puerto)</option>";
  $("dev").innerHTML = d.info && d.info.firmware
    ? `<b>firmware ${d.info.firmware}</b> en ${d.info.port}`
      + (d.info.sta&&d.info.sta.ip?` \u00b7 ${d.info.sta.ip} (${d.info.sta.ssid||""})`:" \u00b7 sin red")
    : `<span class="none">${(d.info&&d.info.error)||"sin device por cable"}</span>`;

  const b=await (await fetch("/api/builds")).json();
  $("builds").innerHTML='<tr><th>etiqueta</th><th>fichero</th><th>tama\u00f1o</th><th>fecha</th><th></th></tr>'
    + b.builds.map(x=>`<tr><td><span class="tag">${x.tag}</span></td>`
      +`<td>${x.nombre}</td><td>${(x.bytes/1024).toFixed(0)} KB</td>`
      +`<td>${x.modificado}</td>`
      +`<td><button class="warn" onclick="flash('${x.ruta}')">instalar</button></td></tr>`).join("");
}

async function cfg(){
  show($("cfgout"),"configurando\u2026");
  const r=await fetch("/api/wifi",{method:"POST",body:JSON.stringify({
    port:$("port").value,ssid:$("ssid").value,psk:$("psk").value,
    host:$("host").value,upload_port:$("uport").value||8080})});
  show($("cfgout"),JSON.stringify(await r.json(),null,1));
  $("psk").value="";
  load();
}

async function flash(ruta){
  if(!confirm("Instalar "+ruta+"?\\n\\nEl device debe estar en recovery."))return;
  show($("flashout"),"instalando "+ruta+"\u2026 (puede tardar un par de minutos)");
  const r=await fetch("/api/flash",{method:"POST",
    body:JSON.stringify({ruta:ruta,port:$("port").value})});
  show($("flashout"),JSON.stringify(await r.json(),null,1));
  setTimeout(load,8000);
}
load();
listlogs();
</script>
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # Acota el handshake TLS diferido y cualquier read: sin esto, una conexion
    # muda mantiene su hilo vivo para siempre (StreamRequestHandler.setup
    # aplica este valor con settimeout).
    timeout = 20

    def log_message(self, fmt, *args):
        pass  # el log propio basta

    def _html(self, text):
        body = text.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        body = json.dumps(obj, indent=1).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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

        if parts and parts[0] == "api":
            # Reescribir el firmware o las credenciales solo desde esta maquina.
            # El receptor esta expuesto a la red para recibir audio; esto no.
            if self.client_address[0] not in ("127.0.0.1", "::1"):
                self._json({"ok": False, "error": "solo desde localhost"}, 403)
                return
            if not panel_admin:
                self._json({"ok": False, "error": "modulo no disponible"}, 503)
                return

            body, err = self._read_body(MAX_JSON)
            if err:
                self._json({"ok": False, "error": err}, 400)
                return
            try:
                req = json.loads(body)
            except json.JSONDecodeError:
                self._json({"ok": False, "error": "json invalido"}, 400)
                return

            if parts == ["api", "wifi"]:
                # La contrasena no pasa por el log en ningun momento.
                log(f"configurando {req.get('port')} -> red {req.get('ssid')!r}")
                try:
                    pasos = panel_admin.configure(
                        req["port"], req.get("ssid", ""), req.get("psk", ""),
                        req.get("host", ""), req.get("upload_port", 8080))
                    self._json({"ok": all(p["ok"] for p in pasos), "pasos": pasos})
                except Exception as e:
                    self._json({"ok": False, "error": f"{type(e).__name__}: {e}"}, 500)
                return

            if parts == ["api", "logs", "fetch"]:
                log("copiando logs de la microSD")
                self._json(panel_admin.fetch_logs())
                return

            if parts == ["api", "logs", "read"]:
                self._json(panel_admin.read_log(
                    req["ruta"], only_problems=req.get("solo_problemas", False)))
                return

            if parts == ["api", "flash"]:
                log(f"instalando {req.get('ruta')} en {req.get('port')}")
                try:
                    self._json(panel_admin.flash(req["ruta"], req["port"]))
                except Exception as e:
                    self._json({"ok": False, "error": f"{type(e).__name__}: {e}"}, 500)
                return

            self._json({"ok": False, "error": "ruta desconocida"}, 404)
            return

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
            # El dict guarda el ahora; la tabla, la serie. Se marcan tambien
            # los latidos de prueba para poder excluirlos de una grafica: un
            # dato inventado que no se distingue del medido ya confundio una
            # vez a dos unidades desplegadas.
            db_record_beat(dev, beat)
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
                # El nombre REAL en disco (store() renombra si ya existia):
                # es lo que el boton de reproducir del panel necesita.
                "saved": path.with_suffix(".ogg").name,
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

        if self.path.startswith("/audio/"):
            # Sirve los .ogg recibidos para el boton de reproducir del panel.
            # La ruta la construye el navegador con datos que en origen puso
            # el device: resolver y comprobar que sigue DENTRO de OUT_DIR.
            rel = self.path[len("/audio/"):]
            try:
                target = (OUT_DIR / rel).resolve()
                ok = (target.suffix == ".ogg" and target.is_file() and
                      target.is_relative_to(OUT_DIR.resolve()))
            except (ValueError, OSError):
                ok = False
            if not ok:
                self._reply(404, b"no\n")
                return
            body = target.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "audio/ogg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/config":
            self._html(ADMIN_HTML)
            return

        if self.path == "/api/device":
            if not panel_admin:
                self._json({"ports": [], "info": {"error": "modulo no disponible"}})
                return
            ports = panel_admin.serial_ports()
            info = panel_admin.device_info(ports[0]) if ports else {}
            self._json({"ports": ports, "info": info})
            return

        if self.path == "/api/logs":
            self._json(panel_admin.list_logs() if panel_admin else {"descargas": []})
            return

        if self.path == "/api/builds":
            self._json(panel_admin.builds() if panel_admin
                       else {"builds": [], "tags": []})
            return

        if self.path == "/purge-test":
            with DEVICES_LOCK:
                gone = [k for k, v in DEVICES.items() if v.get("_test")]
                for k in gone:
                    del DEVICES[k]
            log(f"borrados {len(gone)} latidos de prueba")
            self._reply(200, f"borrados {len(gone)}\n".encode())
            return

        # Sacar la serie para graficarla. Sin esto el historico se acumula y no
        # sirve de nada: el valor esta en poder comparar dos versiones de
        # firmware, y para eso hay que poder exportarlo.
        #   /health.csv                      todo
        #   /health.csv?device=62518A...     un device
        #   /health.csv?hours=24             ultimas 24 h
        #   /health.csv?test=1               incluir latidos de prueba
        if self.path.split("?")[0] == "/health.csv":
            if DB is None:
                self._reply(503, b"historico desactivado (--no-db)\n")
                return
            q = parse_qs(urlparse(self.path).query)
            where, params = [], []
            if not q.get("test"):
                where.append("is_test = 0")
            if q.get("device"):
                where.append("device = ?")
                params.append(q["device"][0])
            if q.get("hours"):
                try:
                    since = (datetime.datetime.now() - datetime.timedelta(
                        hours=float(q["hours"][0]))).isoformat(timespec="seconds")
                    where.append("at >= ?")
                    params.append(since)
                except ValueError:
                    self._reply(400, b"hours no es un numero\n")
                    return
            sql = "SELECT at, device, " + \
                  ", ".join(n for n, _ in DB_COLUMNS) + " FROM health"
            if where:
                sql += " WHERE " + " AND ".join(where)
            sql += " ORDER BY at"
            try:
                with DB_LOCK:
                    cur = DB.execute(sql, params)
                    cols = [d[0] for d in cur.description]
                    rows = cur.fetchall()
            except Exception as e:
                log(f"[aviso] export fallido: {e}")
                self._reply(500, b"export fallido\n")
                return
            import csv
            import io
            buf = io.StringIO()
            w = csv.writer(buf)
            w.writerow(cols)
            w.writerows(rows)
            body = buf.getvalue().encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
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
    ap.add_argument("--cert", type=Path,
                    help="certificado del servicio (PEM). Con el, se sirve TLS.")
    ap.add_argument("--key", type=Path, help="clave privada del certificado")
    ap.add_argument("--keys", type=Path,
                    help="JSON chip_id -> clave AES-128 hex, para descifrar "
                         "el audio en reposo (BPE2) al recibirlo")
    ap.add_argument("--db", type=Path,
                    help="fichero SQLite del historico de latidos "
                         "(por defecto <out>/health.db)")
    ap.add_argument("--no-db", action="store_true",
                    help="no guardar historico; solo el ultimo latido en memoria")
    args = ap.parse_args()

    OUT_DIR = args.out
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # Encendido por defecto a proposito. Un historico que hay que acordarse de
    # pedir no existe el dia que hace falta, y el coste es un fichero.
    if not args.no_db:
        db_init(args.db or (OUT_DIR / "health.db"))

    if args.keys:
        AUDIO_KEYS.update(json.loads(args.keys.read_text()))
        log(f"claves de audio cargadas: {len(AUDIO_KEYS)} device(s)")

    server = ThreadingHTTPServer((args.bind, args.port), Handler)

    # TLS si se dan certificado y clave. El device negocia ECDHE-ECDSA y nada
    # mas — el certificado tiene que ser de curva eliptica (P-256); con RSA el
    # handshake falla sin decir por que.
    if args.cert and args.key:
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(str(args.cert), str(args.key))
        # El device recorta mbedTLS a TLS 1.2: sin esto el servidor ofreceria
        # 1.3 y no habria terreno comun.
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        # do_handshake_on_connect=False, y no es un detalle: con el valor por
        # defecto el handshake corre DENTRO de accept(), en el hilo principal
        # y sin timeout. Un cliente que conecta y se calla (un device que se
        # apaga a mitad de saludo) congela el servidor entero — panel incluido.
        # Diferido, el handshake ocurre en el primer read del hilo de cada
        # conexion, acotado por Handler.timeout.
        server.socket = ctx.wrap_socket(server.socket, server_side=True,
                                        do_handshake_on_connect=False)
        log(f"TLS activo con {args.cert.name}")
    esquema = "https" if (args.cert and args.key) else "http"
    log(f"Escuchando en {esquema}://{args.bind}:{args.port} -> {OUT_DIR.resolve()}")
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
