"""Gate de asociacion: N ciclos de radio en frio, contando exitos.

AT+STA=off hace net_if_down() y pone wifi_ready=false, asi que cada ciclo es un
arranque en frio del RPU con recarga del parche de 87 KB desde la flash externa.
Es el camino real desde que la radio va por prestamo.

    python3 assoc_gate.py salida.log 20

Numeros que ha dado (ver README): power-save on 0/12 · con RPU_RECOVERY
forzado a off 0/10 · antes del arreglo de H2 1/10 · despues 9/10 y 20/20.
No toques los tiempos de abajo sin anotarlo: son lo que hace comparables
esas cifras entre si.
"""
import sys
import time

import bench

SETTLE_S = 6      # que el RPU quede realmente abajo entre ciclos
LINK_TIMEOUT_S = 60

OUT = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 10
FORCE = "--force" in sys.argv

s = bench.open_port()
if not s:
    sys.exit("sin puerto")
bench.require_clean_bench(s, force=FORCE)

with open(OUT, "w", buffering=1) as f:
    f.write(f"{'n':>3} {'ok':>4} {'seg':>6} {'ip':>15} {'rssi':>5}  motivo\n")
    ok = 0
    for i in range(1, N + 1):
        bench.q(s, "AT+STA=off", 3.0)
        time.sleep(SETTLE_S)
        bench.q(s, "AT+STA=on", 2.0)

        t0 = time.time()
        got = None
        why = ""
        while time.time() - t0 < LINK_TIMEOUT_S:
            hd = bench.data(s, "AT+HEALTH?", 1.8)
            if hd.get("wifi") and hd.get("ip"):
                got = (hd["ip"], hd.get("rssi"))
                break
            why = str(hd.get("wifi_err", "")) or why
            time.sleep(2)
        dt = time.time() - t0

        if got:
            ok += 1
            f.write(f"{i:>3} {'SI':>4} {dt:>6.1f} {got[0]:>15} {got[1]:>5}\n")
        else:
            f.write(f"{i:>3} {'NO':>4} {dt:>6.1f} {'-':>15} {'-':>5}  {why}\n")

    f.write(f"\nRESULTADO: {ok}/{N}\n")
    st = bench.data(s, "AT+STA?", 2.0)
    f.write(f"repowers al terminar: {st.get('repowers')}   leases: {st.get('leases')}\n")
