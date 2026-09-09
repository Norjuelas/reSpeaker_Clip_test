"""Gate CON TRAFICO: reproduce en el banco la condicion de campo de H2.

Ciclo: STA=on -> esperar enlace -> AT+HEALTH=now (POST TLS a EC2) -> esperar a
que el latido termine -> pausa corta (fase variable) -> STA=off (apagado real
del RPU, BUCKEN=0) -> 6 s -> siguiente. El gate de asociacion normal apaga la
radio sin haber movido un byte y da 10/10; la sospecha era que el apagado que
cae sobre un bus que acaba de trabajar es lo que deja el estado del host roto.
Dos fallos de asociacion seguidos = aparato colgado: se para y se anota el ciclo.

    python3 traffic_gate.py salida.log 20
"""
import sys
import time

import bench

SETTLE_S = 6
LINK_TIMEOUT_S = 60
BEAT_TIMEOUT_S = 25
DWELLS = [0.0, 0.5, 1.5, 3.0]          # fase del apagado tras el trafico

OUT = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 10
FORCE = "--force" in sys.argv

s = bench.open_port()
if not s:
    sys.exit("sin puerto")
bench.require_clean_bench(s, force=FORCE)

with open(OUT, "w", buffering=1) as f:
    f.write(f"{'n':>3} {'asoc':>5} {'seg':>5} {'latido':>7} {'b_err':>5} {'pausa':>5}  nota\n")
    ok = 0
    fails_in_row = 0
    i = 0
    for i in range(1, N + 1):
        bench.q(s, "AT+STA=off", 3.0)
        time.sleep(SETTLE_S)
        bench.q(s, "AT+STA=on", 2.0)

        t0 = time.time()
        linked = False
        while time.time() - t0 < LINK_TIMEOUT_S:
            d = bench.data(s, "AT+STA?", 1.2)
            if d.get("state") == "connected" and d.get("ip"):
                linked = True
                break
            time.sleep(1.5)
        dt = time.time() - t0

        if not linked:
            fails_in_row += 1
            f.write(f"{i:>3} {'NO':>5} {dt:>5.1f} {'-':>7} {'-':>5} {'-':>5}  sin enlace\n")
            if fails_in_row >= 2:
                f.write(f"\nCOLGADO en el ciclo {i-1}: dos fallos seguidos, se para.\n")
                break
            continue

        fails_in_row = 0
        ok += 1

        # trafico TLS: un latido forzado, y esperar a que termine
        age0 = bench.data(s, "AT+HTTPUP?", 1.5).get("beat_age_s", -1)
        bench.q(s, "AT+HEALTH=now", 1.0)
        t1 = time.time()
        beat, berr = "?", "?"
        h = {}
        while time.time() - t1 < BEAT_TIMEOUT_S:
            h = bench.data(s, "AT+HTTPUP?", 1.2)
            age, err = h.get("beat_age_s", -1), h.get("beat_err", "?")
            if err not in (0, "?") and err != -11:
                beat, berr = "FALLO", err
                break
            if age != -1 and (age0 == -1 or age < age0) and age <= 6:
                beat, berr = "OK", err
                break
            time.sleep(1.0)
        else:
            beat, berr = "TIMEOUT", h.get("beat_err", "?")

        dwell = DWELLS[(i - 1) % len(DWELLS)]
        time.sleep(dwell)
        f.write(f"{i:>3} {'SI':>5} {dt:>5.1f} {beat:>7} {str(berr):>5} {dwell:>5.1f}\n")

    f.write(f"\nRESULTADO: {ok}/{i} asociaciones; ultimo ciclo {i}\n")
    st = bench.data(s, "AT+STA?", 2.0)
    f.write(f"repowers al terminar: {st.get('repowers')}   leases: {st.get('leases')}\n")
