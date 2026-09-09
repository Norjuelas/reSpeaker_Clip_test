"""Prueba de require_clean_bench() SIN hardware, con un puerto de mentira.

Lo que importa distinguir es una ventana pasajera (leases baja a 0 sola) de una
lease colgada de AT+STA=on (no baja nunca). Si esa diferencia se rompe, la
guardia o no sirve o salta siempre, y en los dos casos se acaba usando --force,
que es como no tenerla.

    python3 test_bench_guard.py
"""
import sys
import time

import bench


class FakeSerial:
    """Contesta AT+STA? con la secuencia de leases que se le pase."""

    def __init__(self, lease_seq):
        self.seq = list(lease_seq)
        self.last = 0

    def reset_input_buffer(self):
        pass

    def flush(self):
        pass

    def write(self, _b):
        if self.seq:
            self.last = self.seq.pop(0)

    def read(self, _n):
        return (b'{"ok":true,"data":{"state":"off","ssid":"X",'
                b'"leases":%d,"repowers":0}}' % self.last)


def run(name, seq, expect_exit):
    print(f"--- {name} ---")
    try:
        bench.require_clean_bench(FakeSerial(seq), settle_s=12)
        got = 0
    except SystemExit as e:
        got = e.code
    verdict = "OK" if got == expect_exit else f"MAL (esperaba {expect_exit})"
    print(f"    salida={got}  {verdict}\n")
    return got == expect_exit


def main():
    bench.sd_mounted_on_host = lambda: None      # aislar del montaje
    real_sleep = time.sleep
    time.sleep = lambda _s: None                 # sin esperas reales
    try:
        results = [
            run("banco limpio (leases 0)", [0, 0], 0),
            run("lease colgada persistente (siempre 1)", [1] * 20, 2),
            run("ventana pasajera (1 y luego 0)", [1, 0, 0], 0),
            run("dos leases (manual + ventana)", [2] * 20, 2),
        ]
    finally:
        time.sleep = real_sleep

    print("TODAS BIEN" if all(results) else "HAY FALLOS")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
