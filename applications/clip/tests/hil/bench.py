"""Utilidades comunes de los arneses de banco (hardware in the loop).

Todo lo que hay aqui salio de repetir a mano las mismas tres cosas en cada
prueba: encontrar el puerto, hablar AT y leer el JSON, y comprobar que el banco
no esta mintiendo. Lo tercero es lo importante y es lo que no existia.
"""
import glob
import json
import re
import subprocess
import sys
import time

import serial

BAUD = 115200


# --------------------------------------------------------------------------
# Puerto serie
# --------------------------------------------------------------------------
def open_port(timeout_s=90):
    """Devuelve un puerto que conteste AT+DEVICE, o None.

    El nodo NO es estable: tras un reinicio el aparato aparece como ttyACM0 o
    ttyACM1 segun cuanto tarde el anterior en liberarse. Fijarlo a mano hacia
    que las pruebas no reconectaran a mitad de una tanda.
    """
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        for port in sorted(glob.glob("/dev/ttyACM*")):
            try:
                s = serial.Serial(port, BAUD, timeout=0.3)
                s.write(b"\n")
                time.sleep(0.4)
                s.reset_input_buffer()
                s.write(b"AT+DEVICE\n")
                s.flush()
                time.sleep(1.2)
                if b'"ok"' in s.read(4096):
                    return s
                s.close()
            except Exception:
                pass
        time.sleep(2)
    return None


def q(s, cmd, w=1.5):
    """Manda un comando AT y devuelve la respuesta ya parseada, o None."""
    try:
        s.reset_input_buffer()
        s.write(cmd.encode() + b"\n")
        s.flush()
        time.sleep(w)
        out = s.read(16384).decode(errors="replace")
        m = re.search(r'\{"ok".*?\}\}', out) or re.search(r'\{"ok".*\}', out)
        return json.loads(m.group(0)) if m else None
    except Exception:
        return None


def data(s, cmd, w=1.5):
    """Como q(), pero devuelve directamente el objeto "data" (o {})."""
    return (q(s, cmd, w) or {}).get("data") or {}


# --------------------------------------------------------------------------
# El banco esta limpio?
# --------------------------------------------------------------------------
def sd_mounted_on_host():
    """Ruta de montaje de la tarjeta si el escritorio la ha cogido, o None.

    Montar la tarjeta en el host no solo bloquea las subidas: impide que la
    radio ASOCIE. Dos gates enteros se dieron por buenos antes de verlo.
    Se filtra por transporte USB para no confundirla con /boot/efi, que
    tambien es vfat pero vive en el nvme.
    """
    try:
        out = subprocess.run(
            ["lsblk", "-J", "-o", "NAME,TRAN,MOUNTPOINT"],
            capture_output=True, text=True, timeout=10,
        )
        tree = json.loads(out.stdout)
    except Exception:
        return None

    def walk(node, on_usb):
        on_usb = on_usb or node.get("tran") == "usb"
        if on_usb and node.get("mountpoint"):
            return node["mountpoint"]
        for child in node.get("children", []) or []:
            hit = walk(child, on_usb)
            if hit:
                return hit
        return None

    for dev in tree.get("blockdevices", []):
        hit = walk(dev, False)
        if hit:
            return hit
    return None


def require_clean_bench(s, settle_s=60, force=False):
    """Aborta si el banco no puede dar una medida honesta.

    Dos confusores han invalidado cuatro medidas reales en este proyecto, y
    los dos son ya observables. Esto convierte "acuerdate de mirarlo" en algo
    que la maquina no te deja saltarte.

      1. Una lease colgada de AT+STA=on no caduca NUNCA. Deja la radio
         encendida 24/7 (~41 mA en vez de ~21) y el panel sigue viendo un
         aparato perfectamente sano.
      2. La tarjeta montada en el host impide asociar.

    Con la radio abajo lo correcto es leases == 0. Puede valer 1 de forma
    legitima y pasajera si justo hay una ventana abierta, asi que se espera a
    que cierre (la gracia son 45 s) antes de darlo por colgado.

    Ojo, distinto en el latido: alli leases nunca puede ser 0 porque el latido
    se manda desde dentro de una ventana. Lo que informa es 1 frente a 2.
    """
    problems = []

    mnt = sd_mounted_on_host()
    if mnt:
        problems.append(
            f"la tarjeta esta montada en el host ({mnt}). "
            "Desmontala: udisksctl unmount -b <dev>, y desactiva el automount:\n"
            "    gsettings set org.gnome.desktop.media-handling automount false"
        )

    st = data(s, "AT+STA?", 2.0)
    if not st:
        problems.append("AT+STA? no contesta; el aparato no esta en condiciones de medir")
    else:
        leases = st.get("leases", 0)
        if leases > 0:
            t0 = time.time()
            while time.time() - t0 < settle_s:
                time.sleep(5)
                st = data(s, "AT+STA?", 1.5)
                leases = st.get("leases", 0)
                if leases == 0:
                    break
            if leases > 0:
                problems.append(
                    f"hay {leases} lease(s) de radio sin soltar tras {settle_s} s. "
                    "Casi siempre es un AT+STA=on que nadie cerro: manda AT+STA=off"
                )

        rep = st.get("repowers", 0)
        if rep:
            print(f"AVISO: repowers ya vale {rep} al empezar; cuenta desde ahi", file=sys.stderr)

    if not problems:
        return

    print("\n=== BANCO SUCIO, la medida no valdria ===", file=sys.stderr)
    for p in problems:
        print(f"  - {p}", file=sys.stderr)
    if force:
        print("  (--force: se sigue de todos modos)\n", file=sys.stderr)
        return
    print("\nUsa --force si de verdad quieres medir asi.\n", file=sys.stderr)
    sys.exit(2)
