"""Gate de asociacion TRAS REINICIO EN FRIO. Antes se llamaba assoc_test.py.

Se diferencia de assoc_gate.py en el ciclo: alli la radio se apaga y se
enciende con la aplicacion viva; aqui el aparato REINICIA entero cada vuelta
(AT+REBOOT), asi que se prueba tambien el arranque, el montaje de la tarjeta,
la lectura de la cabecera del parche del nRF7002 y la reconexion del puerto.
Es el ciclo mas lento y el mas parecido a "un aparato que enchufas".

    python3 reboot_gate.py salida.log 10
"""
import sys
import time

import bench

LINK_TIMEOUT_S = 75
BOOT_S = 18            # lo que tarda en volver a haber puerto tras el reinicio

OUT = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 10
FORCE = "--force" in sys.argv

s0 = bench.open_port()
if not s0:
    sys.exit("sin puerto")
bench.require_clean_bench(s0, force=FORCE)
try:
    s0.close()
except Exception:
    pass

with open(OUT, "w", buffering=1) as f:
    f.write(f"{'n':>3}  {'asociado':>9}  {'seg':>5}  {'ip':>15}  {'rssi':>5}  motivo\n")
    ok_count = 0
    for i in range(1, N + 1):
        s = bench.open_port()
        if not s:
            f.write(f"{i:>3}  SIN PUERTO tras el reinicio\n")
            continue

        bench.q(s, "AT+STA=on", 1.0)
        t0 = time.time()
        assoc = False
        ip, rssi, why = "", 0, ""
        while time.time() - t0 < LINK_TIMEOUT_S:
            st = bench.data(s, "AT+STA?", 1.5)
            if st.get("state") == "on" or st.get("ip"):
                hd = bench.data(s, "AT+HEALTH?", 2.0)
                if hd.get("wifi") and hd.get("ip"):
                    assoc = True
                    ip, rssi = hd.get("ip", ""), hd.get("rssi", 0)
                    break
            why = st.get("last_error", "") or why
            time.sleep(2)
        dt = time.time() - t0

        if assoc:
            ok_count += 1
            f.write(f"{i:>3}  {'SI':>9}  {dt:>5.1f}  {ip:>15}  {rssi:>5}\n")
        else:
            f.write(f"{i:>3}  {'NO':>9}  {dt:>5.1f}  {'-':>15}  {'-':>5}  {why}\n")

        bench.q(s, "AT+REBOOT", 1.0)
        try:
            s.close()
        except Exception:
            pass
        time.sleep(BOOT_S)

    f.write(f"\nRESULTADO: {ok_count}/{N} asociaciones correctas\n")
