"""Registra la bateria una vez por minuto. Para cargas y descargas largas.

No hace falta banco limpio para una CARGA, pero para una DESCARGA si: una
lease colgada convierte ~8 h de autonomia en ~4 y el aparato parece sano.
Por eso se comprueba salvo que pases --carga.

    python3 watch_charge.py salida.log 40          # descarga, con guardia
    python3 watch_charge.py salida.log 40 --carga  # carga, sin guardia

Recuerda: current_ua viene del medidor de combustible, que NO ve el nRF7002
(la radio se cuelga de VBAT por delante de la resistencia de medida). Para el
consumo real usa la pendiente de la carga entre latidos, nunca este campo.
"""
import sys
import time

import bench

PERIOD_S = 58

OUT = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 40
CHARGING = "--carga" in sys.argv
FORCE = "--force" in sys.argv

s = bench.open_port()
if not s:
    sys.exit("sin puerto")
if not CHARGING:
    bench.require_clean_bench(s, force=FORCE)

with open(OUT, "w", buffering=1) as f:
    f.write(f"{'t_min':>6}  {'pct':>4}  {'mV':>5}  {'mA':>7}  {'C':>3}  "
            f"{'chg':>6}  {'status':>7}  {'err':>5}  {'lease':>5}\n")
    t0 = time.monotonic()
    for _ in range(N):
        b = bench.data(s, "AT+BATT?", 2.0)
        st = bench.data(s, "AT+STA?", 1.2)
        el = (time.monotonic() - t0) / 60
        if b:
            f.write(f"{el:>6.1f}  {b['battery']:>4}  {b['voltage']:>5}  "
                    f"{b['current_ua']/1000:>7.1f}  {b['temp']:>3}  "
                    f"{str(b['charging']):>6}  {b['chg_status']:#07x}  "
                    f"{b['chg_error']:#05x}  {st.get('leases', '?'):>5}\n")
        else:
            f.write(f"{el:>6.1f}  (sin respuesta)\n")
        time.sleep(PERIOD_S)
