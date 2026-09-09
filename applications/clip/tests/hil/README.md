# Arneses de banco (hardware in the loop)

Guiones que hablan con un aparato real por el cable USB, le hacen la misma
pregunta N veces y escriben una tabla. No son pruebas unitarias: el sujeto es
el aparato, no una funcion.

Estaban en un directorio temporal y se perdian al cerrar la sesion. Eso no era
grave por las 242 lineas — se reescriben en veinte minutos — sino porque **un
numero solo significa algo comparado con otro numero obtenido igual**. `1/10` y
`20/20` prueban que el arreglo de H2 era el arreglo porque los produjo el mismo
guion, con el mismo margen de 6 s y el mismo criterio de exito. Reescribe el
guion de memoria con 3 s y el `18/20` de dentro de un mes no se puede comparar
con nada.

Si cambias un tiempo, anotalo aqui y vuelve a medir la linea base.

## Que hay

| Guion | Que pregunta | Ciclo |
|---|---|---|
| `assoc_gate.py` | ¿arranca la radio en frio de forma fiable? | `STA=off` → 6 s → `STA=on` → esperar IP |
| `traffic_gate.py` | ¿y si el apagado cae sobre un bus que acaba de trabajar? | igual, pero con un POST TLS a EC2 antes de apagar |
| `reboot_gate.py` | ¿y arrancando el aparato entero? | `AT+REBOOT` cada vuelta (antes `assoc_test.py`) |
| `watch_charge.py` | bateria minuto a minuto | muestreo, para cargas y descargas largas |
| `bench.py` | puerto, AT/JSON y la guardia de banco limpio | biblioteca, no se ejecuta |

```sh
cd applications/clip/tests/hil
python3 assoc_gate.py   /tmp/assoc.log   20
python3 traffic_gate.py /tmp/traffic.log 20
python3 reboot_gate.py  /tmp/reboot.log  10
python3 watch_charge.py /tmp/batt.log    40 --carga
```

Necesita `pyserial` y `lsblk`. Sin argumentos de mas, todos abortan si el banco
esta sucio; `--force` lo salta.

## La guardia de banco limpio

Un **confusor** es algo que mueve la medida sin ser lo que tu cambiaste. Este
proyecto tiene dos, y entre los dos han invalidado cuatro medidas reales. Los
dos estaban documentados en `CLAUDE.md` antes de volver a picar, porque una nota
exige que una persona se acuerde de leerla justo cuando esta pensando en otra
cosa. `require_clean_bench()` lo convierte en algo que la maquina no te deja
saltarte.

**1. La lease de radio que no caduca.** La radio se reparte por conteo de
referencias; cuando se suelta la ultima, se apaga 45 s despues. `AT+STA=on` coge
una lease `manual` que **no vence nunca**: se queda hasta `AT+STA=off`. Un
aparato asi pasa de tener la radio al 6% de ciclo a tenerla encendida siempre —
~41 mA en vez de ~21, unas 4 h de autonomia en vez de 8 — y en el panel se ve
perfectamente sano. Se comio una descarga de 3,6 h y una hora entera de
grabacion.

Con la radio abajo lo correcto es `leases == 0`. Puede valer 1 de forma legitima
y pasajera si hay una ventana abierta, asi que la guardia espera hasta 60 s a
que cierre antes de darlo por colgado. **Dentro de un latido es distinto:** alli
`leases` no puede ser 0 nunca, porque el latido se manda desde dentro de una
ventana. Lo que informa es 1 frente a 2.

**2. La tarjeta montada en el host.** El escritorio la coge en cada
reenumeracion USB, y el efecto no es el obvio: no bloquea solo las subidas,
**impide que la radio asocie**. Dos gates enteros se dieron por buenos antes de
verlo.

```sh
gsettings set org.gnome.desktop.media-handling automount false
```

La guardia filtra por transporte USB, asi que no confunde la tarjeta con
`/boot/efi`, que tambien es vfat pero vive en el nvme.

## Linea base — cifras que han dado estos guiones

`assoc_gate.py`, 2026-09:

| Configuracion | Resultado | Que probo |
|---|---|---|
| `NRF_WIFI_LOW_POWER=y` | **0/12** | el ahorro de energia rompe la asociacion en esta placa |
| idem, con `NRF_WIFI_RPU_RECOVERY` forzado a `n` | **0/10** | no es la capa de recuperacion |
| antes del arreglo de H2 | **1/10** | el fallo era real y reproducible |
| despues (`688496b`) | **9/10**, luego **20/20** | el arreglo era el arreglo |

Dos de los primeros gates de esa tanda salieron 1/10 y 0/10 **por la tarjeta
montada**, no por lo que se estaba probando. De ahi viene la guardia.

`traffic_gate.py`: 20/20 asociaciones con trafico TLS de por medio. Los
`TIMEOUT` de la columna del latido son del criterio de exito del propio guion,
no fallos de entrega.

## Lo que estos guiones NO pueden contestar

Distingue dos tipos de pregunta:

- **De cuenta** — "¿arranca la radio siempre?". 20 ciclos son la prueba; el
  reloj de pared es incidental. Se puede comprimir.
- **De integral** — "¿cuanto dura la bateria?". Un mA es un mA. **No** se
  comprime acortando la prueba, solo cambiando el instrumento: medir la
  corriente de cada estado y multiplicar por su ciclo de trabajo, en vez de
  descargar una celda y mirar.

Hoy no hay instrumento para lo segundo. El medidor de combustible **no ve el
nRF7002** (se cuelga de VBAT por delante de la resistencia de medida), asi que
`current_ua` excluye la radio: en una descarga en reposo marcaba 1,1 mA
mientras el consumo real era 42,5 mA. Los ~18 mA de grabacion que siguen sin
atribuirse estan sin atribuir por esto. Un PPK2 con `ppk2-api` y unas 40 lineas
de Python lo cierra.

Y los temporizadores (`CLIP_UPLOAD_INTERVAL_MIN`, `CLIP_HEALTH_INTERVAL_S`) son
constantes de compilacion, no ajustes en caliente como el SSID o el reloj. Por
eso probar una ventana de radio distinta obliga a recompilar y reinstalar.
