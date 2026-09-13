# La radio se queda sin asociar — estado del problema

Documento de traspaso. Todo lo que hace falta para retomar este problema sin repetir
camino. Última actualización: **2026-09-12**.

Complementa, no sustituye:
- `docs/cuaderno-de-laboratorio.md` — un apunte por cambio, con resultado real (L-001…L-015).
- `CLAUDE.md` — invariantes y escollos del proyecto.
- `25-backlog-bateria-y-transmision.md`, fuera de este repo, en `docs-respeaker/`.
- `docs/radio-alternativas-y-rediseno.md` — ideas de rediseño, otras formas de conectar y de
  entregar, y método para ver el fallo; cada una con cómo se implementaría (2026-09-12).

**Cómo leer este documento.** Las secciones 1 a 9 son el traspaso del 2026-09-11, escrito
con una ventana de un log. La sección 10 es el diagnóstico del 2026-09-12 sobre los cuatro
juegos de logs completos y sobre todo el código que toca el chip; **corrige varias
afirmaciones de las secciones 3, 4 y 7**, que quedan marcadas en su sitio con `⟵` en vez
de borrarse. Las secciones 11 y 12 son qué hacer ahora: pruebas y paliativos.

---

## 1. El síntoma

Un aparato que está grabando perfectamente **deja de entregar nada** —ni latido ni
subidas— durante horas, y en el panel simplemente desaparece. El audio **nunca se pierde**:
queda en la tarjeta. Lo que se pierde es la entrega a tiempo y toda visibilidad.

Sólo un **reinicio del SoC** lo recupera, y lo hace en segundos.

---

## 2. Antecedentes — tres cosas distintas que se llamaron "el fallo del wifi"

Separarlas importa, porque se confundieron durante días.

| # | Qué era | Estado |
|---|---|---|
| 1 | **H2 original.** El enlace decía "arriba" con IP y nada se movía | ✅ **resuelto** `688496b` |
| 2 | **H2 residual.** La radio deja de asociar, causa raíz abierta | 🔴 **abierto — es este documento** |
| 3 | **Ficheros que no llegaban.** Parecía la radio; era la tarjeta | ✅ **resuelto y verificado** `1780026` |

**(1) era nuestro, no de Nordic.** `wifi_sta_off()` tenía `if (!sta_associated) return 0;`
— "nada conectado, nada que desconectar". Pero *"interfaz arriba, sin asociar"* es
justamente el estado en que hay que reencender la radio, y `net_if_down()` es la única
ruta del firmware que llega a `rpu_pwroff()`. Tras una asociación fallida nada reencendía
el chip. Costó semanas, atribuido al driver de Nordic. La puerta de asociación pasó de
**1/10 a 20/20** al quitarlo.

**(3) nunca fue la radio.** El barrido de subida leía la tarjeta sin montarla; dormida,
`storage_list_sessions()` devolvía `-EINVAL`, los dos bucles daban cero vueltas y el
aparato publicaba `pending_files: 0` con audio sin enviar. Costó un día perseguirlo como
si fuera red. Verificado arreglado el 2026-09-10 con la tarjeta apagada al abrir la
ventana y el fichero subiendo igualmente.

---

## 3. Lo que sabemos de (2), con evidencia

### La firma, en una fila del latido

```
prev_why=2   prev_miss=29   prev_stage=1   tdfail=0
```

*Se rindió tras 29 ventanas seguidas que no llegaron a asociar, sin un solo fallo de
apagado.*

### Y en el log de la tarjeta, una ventana cualquiera de esas 29

```
[04:51:59] wpa_supp: Failed to execute wpa_cli command: remove_network all   <- ya roto ANTES
[04:52:19] wifi: STA association failed (reason: timeout, no result event)
[04:52:20] wifi: radio: 'upload' pide prestamo, se enciende
[04:52:34] wpa_supp: 'DISCONNECT' command timed out.
[04:52:54] wpa_supp: 'REMOVE_NETWORK all' command timed out.
[04:52:54] wpa_supp: Failed to remove all networks
[04:52:54] wifi: STA reconnect in 138343 ms                 <- el driver se da 138 s
[04:53:05] http_upload: ventana: sin enlace tras esperar    <- nosotros esperabamos 45
[04:53:05] wifi: radio: 'upload' suelta, se apaga en 45s
[04:53:05] ventana sin exito (12 seguidas), etapa 1
[04:53:05] radio: colgada, pero se esta grabando; no se reinicia
```

### Hechos establecidos

1. **El estado es del lado del host, no del chip.** `tdfail=0` en las 29 ventanas
   significa que `net_if_down()` → `rpu_pwroff()` cortó BUCKEN e IOVDD **29 veces** y la
   asociación falló después de cada una. **Un ciclo de alimentación real del chip no
   limpia este estado.**
   ⟵ **Corregido §10.2.** `tdfail=0` no prueba nada de eso: `nrf_wifi_if_stop_zep()`
   devuelve 0 aunque registre `RPU is unresponsive for 10 sec`, y 5 de esos apagados de la
   noche 3 cayeron sobre un chip que ya no contestaba. Y el apagado en tiempo de ejecución
   deja los pines de alimentación flotando (§10.8), así que ni siquiera es seguro que fuera
   un ciclo de alimentación real.
2. **Un reinicio del SoC sí lo limpia**, en segundos. Observado tres veces.
3. **wpa_supplicant deja de contestar a su propio socket de control.** `DISCONNECT`,
   `REMOVE_NETWORK` y hasta `SET country` expiran a los 15-20 s. `SET country` no toca el
   aire, así que **no es cobertura**.
   ⟵ **Matizado §10.2.** Cierto que no es cobertura, pero el suplicante no está atascado
   por su cuenta: está esperando al chip (10 s en `GET_REG`, hasta 35 s en resultados de
   escaneo) y el chip no contesta. Los timeouts son consecuencia, no causa.
4. **Ya está roto antes de que la ventana empiece.** El `remove_network all` fallido cae
   20 s *antes* de que la ventana pida la radio. Cada ciclo hereda el destrozo del anterior.
   ⟵ **Corregido §10.2.** Ese `remove_network all` es el primer mandato del CONNECT de las
   04:51:59, lanzado por **nuestra** cadena de reconexión fuera de cualquier ventana. Falla,
   hostap devuelve éxito igual, y por eso no hay resultado que esperar.
5. **`no result event`**: el driver nunca entrega el resultado de la asociación.
6. **Matábamos el reintento del driver.** ⟵ **Corregido §10.2: no existe tal reintento.
   Los 138 s son `sta_schedule_reconnect()` en `wifi.c`, backoff propio de 5 a 120 s más
   jitter.** Él se programaba 138 s; nosotros esperábamos 45
   y soltábamos, y la radio se apagaba 45 s después: 90 s. **Su recuperación no llegó a
   intentarse ni una vez en toda la noche.** Esto es lo único demostrablemente mal que
   hacíamos nosotros, y es lo que ataca L-015.
7. **Hay al menos dos firmas distintas**, y puede que sean dos fallos:
   - **A**: tormenta de timeouts de wpa_supplicant, **sin** `0xAAAAAAAA` (noche 3)
   - **B**: tormenta de `0xAAAAAAAA` (398-400 por tanda), **sin** un solo timeout
   `0xAAAAAAAA` es patrón de memoria sin inicializar: el driver calcula una dirección del
   RPU que es basura.
   ⟵ **Corregido §10.4.** El valor sale de leer la cola de mandatos del chip cuando el chip
   no la sirve, y el shim de Nordic ignora el error de lectura. Y no son dos fallos: **B es
   el chip callado; A es lo que hace nuestra capa encima de un chip callado.**

---

## 4. Lo que está DESCARTADO, y con qué evidencia

No volver a proponerlo sin evidencia nueva.

| Hipótesis | Por qué cae |
|---|---|
| **Ahorro de energía del RPU** | Tres brazos, 10 asociaciones en frío cada uno: off **10/10**, on **0/12**, on con `RPU_RECOVERY` forzado a `n` **0/10**. `NRF_WIFI_LOW_POWER=n` es decisión medida |
| **"Reencender la radio en vez de reiniciar"** | Ya pasa: el vencimiento del préstamo llama a `wifi_sta_off()` → `net_if_down()`. Ocurrió 29 veces y no arregló nada. ⟵ **§10.8: ese apagado deja BUCKEN e IOVDD_CTRL flotando (`GPIO_DISCONNECTED`); el arranque los conduce a bajo durante dos minutos. No es el mismo apagado, y es la única asimetría entre "ciclo de radio" y "reinicio del SoC" que llega al chip.** Vuelve a la mesa como paliativo A de §12 |
| **Fuga de heap** | `heap_free` plano toda la noche: 42.488 en cada latido de apertura, 34.480 en cada cierre, sin deriva en 2 h |
| **La carga degrada el wifi** | 7 días y 694 latidos: RSSI medio **−50,4 cargando** vs **−50,7 sin cargar**. 0,3 dB |
| **Sólo cobertura débil** | `SET country` no toca el aire y aun así expira |
| **Nuestro apagado corrompe al suplicante** | `STA disconnect failed` aparece **0 veces**; los comandos que expiran son del camino de reconexión del driver, 45 s antes de que corra nuestro apagado. ⟵ **Razonamiento inválido (§10.2):** sin asociación `wifi_sta_off()` no envía DISCONNECT, así que esa ausencia no dice nada, y los comandos que expiran son nuestros. **La conclusión se sostiene por otra vía:** la primera muerte del chip ocurre dentro de una ventana normal, antes de que corra ningún apagado |
| **El barrido se colgaba** | Si se hubiera colgado, `wedge_note_window()` no habría corrido y no habría reinicio. Reinició: las ventanas seguían ejecutándose |
| **La carga la provoca (RSSI)** | Ver arriba; el −70 dBm de una tanda fue por moverlo, no por el cargador. ⟵ **Descartado el mecanismo RSSI, no el cable:** todas las primeras muertes del chip del 10 y del 11 ocurrieron con VBUS puesto, y el único arranque limpio corrió desenchufado (§10.8) |

---

## 5. Métricas de las pruebas

### Las tres noches

| noche | imagen | grabación | audio | entrega | qué pasó |
|---|---|---|---|---|---|
| 09-09→10 | `1780026` | 7 h 21 min | ✅ | ❌ parcial | radio muerta a las 04:50, **5 h 37 min mudo** |
| 09-10→11 | `ad59518` | 9 h 44 min | ✅ | ✅ completa | **39/39 asociaciones, 0 timeouts** |
| 09-11 | `ad59518` | 9 h 15 min | ✅ | ❌ parcial | **29 ventanas fallando, 7 h 33 min mudo, 72 ficheros parados** |

**Las noches 2 y 3 corrieron el MISMO binario, sin reflashear entre medias.** Una limpia y
una rota. **El fallo no es determinista: una noche buena no prueba nada.**

Y la grabación no ha fallado nunca: **3 de 3 noches capturaron audio**. Falla la entrega.

### Otras cifras medidas

| | |
|---|---|
| Asociación en frío, sana | **10,3 s**, repetible (mismo guion que el gate 20/20 de H2) |
| Caudal de subida | ~52 KB/s, ~3,1 ficheros/min |
| Drenaje de atraso | 88 ficheros / 176 MB en ~23 min, **0 fallos** |
| Autonomía grabando + subiendo | **9 h 15 min** de 4213 mV a 3598 mV |
| Radio asociada, en reposo | ~41,4 mA de 42,5 mA totales |
| Ciclo de trabajo de la radio | ~6,3% |
| Heap | 42.488 B libres en reposo, 34.480 durante una subida |
| Latido | 660 B de un buffer de 960 |
| FLASH | **99,37%** — 5.908 B libres de 933.376 |
| RAM | ~95,2% |

### Un fallo aparte, sin resolver

**El medidor de combustible miente.** `battery_pct 83` con `battery_mv 3598` — una celda
esencialmente vacía. Reconvergió a 2% sólo tras reiniciar. Coherente con el defecto
documentado de `wifi_load_estimate_a()`, que lee `wifi_ap_is_running()` (compilado fuera),
así que la compensación por wifi **no se aplica nunca** y el SoC lee alto con la radio
encendida. **Usar voltaje, no porcentaje.**

---

## 6. Instrumentación disponible (nueva, 2026-09-10/11)

Sin esto no se puede diagnosticar en remoto. El log de la tarjeta **no sirve en campo**:
exige tener el aparato en la mano.

### En el latido

| campo | qué dice |
|---|---|
| `miss` | ventanas seguidas sin un solo éxito; si la racha terminó, la última que hubo |
| `miss_stage` | **dónde** se rindió: `1` no asoció · `2` asoció y falló el latido · `3` fallaron las subidas · `5` tarjeta no montó · `7` no se pudo listar |
| `ok_age_s` | segundos desde la última ventana que logró algo (uptime si nunca) |
| `tdfail` | veces que `net_if_down()` falló — si sube, el RPU **no** se apagó |
| `boots` | arranques desde el aprovisionamiento, persistente |
| `prev_why` | **por qué murió el arranque anterior**: `0` no se sabe · `1` apagado deliberado · `2` detector de radio colgada |
| `prev_miss`, `prev_stage`, `prev_td` | los contadores **congelados antes de reiniciar** |

`prev_*` es lo único que sobrevive al reinicio, y el reinicio es justo el caso que más
interesa explicar.

### En la tarjeta

El log ahora se mantiene encendido mientras la tarjeta esté encendida por otra razón
(grabando, barriendo, USB puesto) — antes se retiraba a los 120 s del arranque pasara lo
que pasara. Resultado: **9 h 44 min de log continuo** donde el techo era `00:01:59`.
Y se reactiva solo durante las primeras ventanas malas aunque la tarjeta estuviera dormida.

### Cómo leerlo

Idioma de consulta y credenciales: ver `CLAUDE.md`, "Where field data actually lives".
El diagnóstico en dos números: **`tdfail` subiendo con `stage=1`** = "el apagado falló y
ahora no levanta". **`stage=1` con `tdfail=0`** = el fallo de este documento.

⟵ **Aviso 2026-09-12:** `tdfail` es ciego al caso que importa. `nrf_wifi_if_stop_zep()`
registra `RPU is unresponsive` y devuelve 0, así que un chip muerto en el apagado cuenta
como apagado limpio. Hasta que el driver lo reporte (§12, parche E), `tdfail=0` sólo dice
que `net_if_down()` no falló, no que el chip contestara.

---

## 7. Lo que está en marcha y lo que queda

### En marcha

**L-015** (`9e21bfd`) — espera de enlace adaptativa: 45 s normal, **150 s** (> los 120 del
backoff del driver) durante las 3 primeras ventanas malas. Ataca el hecho 6. Flasheado el
2026-09-11, **sin verificar en campo**. Verifica el *mecanismo*, no la *cura*: que el
reintento del driver llegue a correr no garantiza que recupere un suplicante atascado.

⟵ **Nota 2026-09-12:** el backoff de 120 s es nuestro (`STA_RECONNECT_MAX_MS` en
`wifi.c`), así que L-015 le da tiempo a nuestro propio `wifi_sta_on()` contra el mismo chip
callado. Cambia el tiempo de la carrera entre nuestros dos hilos (§10.6), no el mecanismo.
No hace daño y no cura.

### Decisión pendiente, del producto

**Recuperación mientras se graba.** Hoy el detector cuenta 3 ventanas malas y reinicia —
**pero nunca grabando**, así que difiere indefinidamente. Dos de tres noches acabaron así.
La única recuperación conocida es el reinicio.

- Coste de no hacerlo: **7 h 33 min mudo**, 72 ficheros parados (medido).
- Coste de hacerlo: se parte la sesión en un límite de trozo.
- Mitigación: persistir "estaba grabando" en la nota de arranque y **reanudar solo** al
  arrancar. La pérdida baja a los ~15-20 s del arranque.

El usuario objetó, con razón, que la aplicación exige grabación continua y que si el fallo
se repite mucho se pierde mucho. Por eso primero L-015, que no cuesta audio.

### Sin empezar

- **Causa raíz del suplicante atascado.** Probablemente necesita la Raytac DK: SWD,
  parada en el fallo, osciloscopio. Es la conclusión del propio backlog.
- **Conflicto de temporizadores**, más allá de L-015: la gracia del préstamo (45 s) y el
  backoff del driver (hasta 120 s) siguen sin estar coordinados.
- **El medidor de combustible** (sección 5).
- **Quitar el soporte de AP del suplicante**: `CONFIG_WIFI_NM_WPA_SUPPLICANT_AP=y` en un
  aparato que sólo es estación. Medido: **104 KB de FLASH** (99,35% → 88,21%). Pero cuesta
  **11,8 KB de heap** y pierde 11ac/11ax, y ya vimos latidos con `-ENOMEM` a 44 KB libres.
  Necesita prueba, no un interruptor.

---

## 8. Protocolo de prueba

**Una noche buena no prueba nada** (noches 2 y 3, mismo binario). Hacen falta varias.

Antes de empezar:
- `leases: 0` — un préstamo colgado de `AT+STA=on` **no vence nunca**, deja la radio
  siempre encendida y el panel lo ve sano. Ha invalidado dos medidas.
- Cable **fuera**: grabar está vetado con VBUS presente, y con cable la tarjeta no duerme.
- Tarjeta no montada en el host: **impide asociar**, no sólo subir.
- Batería al 100%.
- Endpoint e intervalo de producción (`:443`, 15 min). Dejar un valor de prueba dentro ya
  costó una mañana.

Después: **dejarlo quieto, en el mismo sitio, sin enchufar, al menos una hora.** La noche 2
se estropeó como experimento porque al parar se movió y se enchufó a la vez.

Al recoger: **primero** el latido del servidor, **después** enchufar y leer la tarjeta.
Los contadores viven en RAM.

---

## 9. Escollos que ya han costado tiempo

- Un cable **esconde** toda la clase de fallos de la tarjeta dormida: `clip_sd_busy()` es
  cierto mientras `usb_cdc_is_enabled()`, hasta 10 min después de quitar VBUS.
- `AT+USB=off` **se lleva el CDC**: hace falta replug físico.
- El aparato reaparece como **ttyACM0 o ttyACM1**. Fijarlo a mano falló dos veces; usar
  `applications/clip/tests/hil/bench.py`.
- El CDC se colgó una vez con el aparato vivo y enumerado: `open()` bloqueado. Replug.
- El reloj del aparato está **mal**: los identificadores de sesión dicen 2026-08-16. Las
  marcas del log son **uptime**, no hora.
- Un fichero de log contiene **varios arranques**; agregar por hora mezcla la primera hora
  de todos. **Segmentar por `Boot: last reset`.** Este error me hizo describir la misma
  tanda como "impecable" y luego como "rota constantemente" en dos mensajes seguidos.
- `pending_files` es el número de la ventana **anterior** en el latido de apertura.
- **Copiar los logs a mitad de prueba contamina la prueba.** La primera muerte de la noche 3
  cae en el mismo minuto en que se copió la tarjeta por USB (23:18, uptime 00:04). Con cable
  puesto no se graba, la tarjeta no duerme y el chip tiene VBUS: tres variables cambiadas a
  la vez. Copiar sólo al terminar.
- **`grep` en la máquina de desarrollo es `ugrep`** y con varios ficheros devuelve las
  coincidencias desordenadas. Fichero a fichero, o `awk` sobre los ficheros en orden.
- **`repowers` vale 0 por construcción**, no porque H2 no aparezca: la sonda usa
  `NET_REQUEST_WIFI_IFACE_STATUS`, y `supplicant_status()` no toca el chip si no hay
  asociación. Nunca ha preguntado nada al RPU.
- **Un fichero de log cabe en 128 KB**; una noche mala son cuatro. Los ficheros que faltan
  (la tarjeta guarda 20) hay que copiarlos antes de que roten.


---

## 10. Diagnóstico del 2026-09-12: los cuatro juegos de logs y todo el código que toca el chip

### 10.1 Qué se miró

Copias de `/SD:/LOG` en la máquina de desarrollo, fuera del repo:

| carpeta | qué contiene |
|---|---|
| `~/clip-log-20260909/LOG/` | banco y noche 1, imágenes anteriores a `ad59518`, unos 30 arranques |
| `~/clip-log-20260910/` | copia parcial de la mañana del 10 |
| `~/clip-log-noche2/` (`log.0036` a `0039`) | tarde y noche del 10, copiada el 10 a las 23:18 |
| `~/clip-log-noche3/` (`log.0040` a `0043`) | noche del 10 al 11, copiada el 11 a las 09:55 |

El arranque de la noche 3 empieza al final de `noche2/log.0039`. Sus primeros 15 minutos, uptime
00:04 a 00:18, **no están en ninguna copia**: la tarjeta todavía tiene ese fichero (§11.1).

Código leído completo y contrastado con los logs: `wifi.c`, `http_upload.c`, `clip_event.c`,
`health.c`, `battery.c`, `usb_cdc.c`; en NCS v3.3.0 el driver nRF70 (`net_if.c`,
`wpa_supp_if.c`, `fmac_main.c`), su HAL (`hal_api_common.c`, `hpqm.c`, `hal_mem.c`,
`hal_interrupt.c`), el shim de bus (`qspi_if.c`, `rpu_hw_if.c`, `shim.c`), `nrfx_qspi.c` con
sus erratas, y el pegamento de hostap (`supp_main.c`, `supp_api.c`, `ctrl_iface_zephyr.c`,
`wpa_ctrl.c`, `driver_zephyr.c`).

### 10.2 Correcciones a lo escrito el día 11

- **"STA reconnect in 138343 ms" es nuestro.** `sta_schedule_reconnect()` en `wifi.c`: 5 s
  doblando hasta 120 s más un 50 % de jitter. El driver de Nordic no tiene ningún reintento.
- **Los timeouts de `DISCONNECT` y `REMOVE_NETWORK all` son nuestros.** Son los dos mandatos de
  `supplicant_disconnect()`, que lanza la rama de fallo de `wifi_sta_on()` justo después de
  "STA association failed". La cuenta es exacta: 15 s de timeout del socket de control, 5 s de
  espera a `WPA_DISCONNECTED`, 15 s otra vez. De 04:52:19 a 04:52:54.
- **El `remove_network all` de las 04:51:59 es el primer mandato de un CONNECT** lanzado por
  nuestra cadena de reconexión, fuera de ventana. En hostap, `wpas_add_and_config_network()`
  salta a `out` con `ret` todavía en 0, así que `net_mgmt(CONNECT)` devuelve éxito sin haber
  configurado nada. "No result event" es el resultado natural, no un evento perdido.
- **`tdfail=0` no demuestra 29 apagados limpios.** Ver la marca en §3.
- **La "noche 2 limpia" es un arranque, no una imagen.** Esa misma tarde, con el mismo binario,
  hubo tres arranques malos con el guion idéntico al de la noche 3, todos con USB puesto.
- **Las tormentas de `0xAAAAAAAA` no son de la noche 3.** Están en las cuatro carpetas: 971,
  731, 371 y 6685 líneas. Y en tres arranques malos el chip callado no produjo ninguna: sólo
  timeouts. La firma común es **el chip que no contesta**, no el valor.
- **H2 original no está resuelto.** "Enlace arriba con IP y nada se mueve" es exactamente la
  muerte a mitad de sesión de §10.3. `688496b` arregló que el firmware reintente, no que el
  chip no muera.
- **`repowers = 0` por construcción.** Ver §9.

### 10.3 El guion, arranque por arranque

Arranques de la imagen actual (`ad59518` y `9e21bfd`), segmentados por `Boot: last reset`:

| arranque (fichero:línea) | reset | VBUS en la primera muerte | grabando | ventanas OK / total | `RPU is unresponsive` | líneas `0xAAAAAAAA` |
|---|---|---|---|---|---|---|
| noche2 `log.0038:190` | encendido | fuera desde 00:10 | sí, desde 00:10 | **39 / 39** | 0 | 0 |
| noche2 `log.0038:1283` | encendido | puesto | no | 1 / 4 | 1 | 0 |
| noche2 `log.0039:47` | reinicio por cuelgue | puesto | no | 1 / 4 | 1 | 0 |
| noche2 `log.0039:268` | reinicio por cuelgue | puesto | no | 1 / 2 | 1 | 0 |
| noche 3 (`0039:350` a `0043:1225`) | encendido | puesto a las 00:04 | desde ~00:20 | 3 / 35 | 5 | 971 |
| banco 09-09, unos 30 arranques | mixto | banco, USB | no | muchas primeras ventanas muertas | 4 | 6685 |

**El guion de cada arranque malo es el mismo.** La primera ventana asocia en 10 s y drena un
atraso de 114 a 116 ficheros pequeños, una conexión TLS por fichero. Durante ese drenaje, o justo
después, el chip deja de contestar: las subidas fallan con `-116`, `signal_poll` y
`get_conn_info` expiran a los 5 s, el latido falla. En el apagado de los 45 s,
`chg_vif_state` registra `RPU is unresponsive for 10 sec`. A partir de ahí, en cada ventana el
chip se enciende, carga el firmware, contesta INIT, VIF UP, MAC y `SET country`, y unos 3 s
después, en el primer escaneo, toda lectura del bus devuelve `0xAAAAAAAA`. Dos ventanas de la
noche 3 asociaron a pesar de todo, subieron 17 ficheros y murieron igual a mitad de sesión.

**La noche 3 empeoró con las horas:**

| tramo | ventanas | dónde muere el chip |
|---|---|---|
| 00:33 a 01:59 | 6 | en el primer escaneo, tras `SET country` correcto; encendido de 0,6 s |
| 01:20 y 01:42 | 2 | asocia, sube 17 ficheros, muere a mitad de sesión |
| 02:14 a 09:12 | 28 | antes del CONNECT: `GET_REG` sin respuesta, `SET country` expira a los 15,000 s exactos, 140 veces |

Un encendido sano tarda 0,6 s desde `pide prestamo` hasta `STA connecting` en las dos noches. A
partir de las 02:14 tarda 15,000 s, que es el timeout del socket de control esperando a un chip
que ya no contesta al ajuste regulatorio; al final de la noche, hasta 80 s. La batería acabó la
noche a 3,3 V.

En los tres arranques malos del día 10 **el aparato no estaba grabando cuando el chip murió
por primera vez**: con VBUS puesto la grabación está vetada. En la noche 3 la ventana 1 corrió
con USB y sin grabar; si el chip murió en el encendido de las 00:18 y no en el apagado de las
00:04, no se sabe si ya grababa. Lo dicen los 15 minutos que faltan (§11.1).

### 10.4 Qué es el `0xAAAAAAAA`

`zep_shim_qspi_read_reg32()` en el shim de Nordic declara `val` sin inicializar, llama a la
lectura del bus, **ignora el código de retorno** y devuelve `val`. Con `CONFIG_INIT_STACKS=y`
una palabra de pila no escrita vale `0xAAAAAAAA`. La otra ruta al mismo valor es el patrón de
relleno que devuelve el esclavo QSPI del nRF7002 cuando no puede servir la palabra a tiempo:
el propio driver lo trata como "no hay más eventos" en `hal_rpu_event_get_all()`. Las dos
lecturas coinciden en lo que importa: **una lectura al chip que no devolvió nada válido.**
Los fallos vienen a 0,3 ms unos de otros, así que no son timeouts.

Encima, el HAL hace algo dañino con ese valor: `hal_rpu_hpq_dequeue()` lo escribe de vuelta en
el registro de cola del chip siempre que no sea cero. Una lectura mala puede corromper la cola
de mandatos del RPU.

La carga de firmware funciona en cada ventana, cientos de transferencias por el mismo bus, así
que el periférico QSPI del nRF5340 no está atascado. **El chip deja de servir el bus en el
momento en que empieza la actividad de RF**: escaneo o TX. Más tarde en la noche, ya en el
primer mandato tras VIF UP.

### 10.5 Código revisado como posible detonante

| Código que podría hacer fallar al chip | Qué dicen los logs | Veredicto |
|---|---|---|
| Carreras de encendido/apagado y cadena de reconexión en `wifi.c` | Cada primera muerte ocurre dentro de una ventana sin competencia: un préstamo, asociado en 10 s, subiendo, ningún segundo encendido ni apagado en curso | Amplificador después, no detonante |
| CPU boost 64/128 MHz y el guardián de la errata 159 del QSPI en nrfx | El arranque limpio grabó a 128 MHz 9,7 h con 39/39 ventanas; las muertes del día 10 fueron a 64 MHz sin grabar | No es el detonante. El guardián está inactivo en este silicio; depende de la revisión, un lote futuro podría activarlo y entonces el wifi no funcionaría grabando |
| Audio, PDM y escrituras a la SD grabando | En ninguna primera muerte del día 10 se estaba grabando | No es necesario para el fallo |
| El driver SPI de la SD peleando con el shim QSPI por el divisor de 192 MHz | El driver SPI sólo lee el divisor de la CPU; nunca toca HFCLK192M | No existe tal pelea |
| Reset y configuración de coexistencia | El reset corre tras DISCONNECT, es decir después de la muerte; el conmutador RF no lo controla el host | No es el detonante |
| Parche de firmware en la flash externa | El aparato reporta 1.2.14.8; la cabecera del blob de NCS 3.3.0 es 1.2.14.8; el driver valida la cabecera, y una carga mala aborta `net_if_up`, que no ocurrió en las noches 2 ni 3 | No es el detonante |
| Secuencia de encendido | En todas las ventanas malas pasan `rpu_validate_comms`, la carga del firmware, INIT y VIF UP. El chip muere en el primer mandato de RF posterior | Bus y chip sanos al encender |
| Heaps del driver recortados a 12 y 20 KB con asignación bloqueante | Aparcarían un hilo; INIT nunca expiró y las lecturas devuelven basura, no bloqueo | Riesgo aparte, no esta firma |
| Shim QSPI: init/uninit por transferencia y cambio de divisor sin espera | Idéntico en el arranque limpio; cientos de transferencias por encendido salen bien | No puede ser el detonante solo; contribuyente no excluible sin el código de error |
| Configuración del PMIC y del cargador | 110 mA de carga, 1 A de descarga, 500 mA de VBUS; el ciclo off/on de G7 precedió a una muerte una vez de tres, 7 s antes | Nada deja al chip sin corriente por configuración |
| Potencia TX | Los ficheros de placa la limitan a 9 dBm en 2,4 GHz y 6 dBm en 5 GHz | Picos de corriente muy por debajo del máximo del chip |

### 10.6 Lo que el código sí hace, verificado

Nada de esto corre antes de la primera muerte. Todo esto decide que la primera muerte dure la
noche entera y que el log parezca un problema del suplicante.

En el driver de Nordic:
- `rpu_gpio_remove()` deja BUCKEN e IOVDD_CTRL como `GPIO_DISCONNECTED` tras apagar; el DTS no
  les pone pull. Un chip colgado nunca se apaga de verdad en tiempo de ejecución. Sólo el
  arranque, `nrf_wifi_gpio_config_early()`, los conduce a bajo, y los mantiene así hasta la
  primera ventana, dos minutos.
- `zep_shim_qspi_read_reg32()` devuelve una palabra sin inicializar cuando la lectura falla.
- `hal_rpu_hpq_dequeue()` escribe ese valor de vuelta en la cola del chip.
- `nrf_wifi_if_stop_zep()` devuelve éxito tras registrar `RPU is unresponsive`.

En hostap: un CONNECT cuyo primer mandato falla devuelve éxito; la retirada de la interfaz
(`del_interface`) da al bucle de eventos 1 s cuando ese bucle bloquea legítimamente 35 s; el
canal de control no empareja peticiones con respuestas, así que un timeout desincroniza el
canal para el resto de la vida de esa interfaz.

En nuestro firmware:
- `wifi_sta_on()` y `wifi_sta_off()` corren en hilos distintos sin exclusión mutua, y
  `k_work_cancel_delayable` no detiene un reintento ya en marcha. La cadena de reconexión sigue
  viva con el préstamo a cero. Eso son los 138 s, los timeouts de 35 s por intento y los 560
  timeouts de la noche 3: 150 encendidos en una noche.
- `wifi_sta_off()` no envía DISCONNECT si no hay asociación y tira la interfaz debajo de un
  suplicante a media búsqueda.
- La sonda de `CLIP_WIFI_RPU_PROBE` nunca llega al chip sin asociación.
- El detector de cuelgue sólo sabe reiniciar, y el reinicio funciona porque es la única ruta
  que mantiene los pines de alimentación a bajo.

### 10.7 La teoría del AP frente a los logs

- Los 262 fallos de asociación de las cuatro carpetas dicen `timeout, no result event`. No hay
  ni un motivo del suplicante: ningún rechazo de autenticación ni de asociación, en ningún
  fichero.
- Todos los `STA disconnected` de las cuatro carpetas siguen a nuestro propio apagado (57 en
  las dos noches). El AP nunca tiró el enlace mientras lo teníamos.
- El chip dejó de contestar mandatos que no salen de la placa: `SET_IFFLAGS` en el apagado, y
  desde las 02:14 de la noche 3 `GET_REG` en el encendido, 140 veces, antes de pedir ningún
  CONNECT.
- Las tormentas aparecen bajo tres SSID distintos (`APTO11-2G`, `ACE_IOT`, `Apto 11`): dos
  puntos de acceso de casa y el del banco.
- El punto de muerte se adelantó con las horas dentro de la misma noche. Eso es un aparato que
  se degrada, no una red.

La teoría del AP sí tiene casa en los datos, pero no en las noches malas. En `20260909/LOG/log.0023`,
arranque de la línea 293, hay **12 fallos seguidos de `no result event` en diez minutos con el
chip perfectamente vivo**: apagados limpios, sin tormentas, sin `unresponsive`, y se recuperó
solo. Las asociaciones sanas tardan de 6,2 a 7,9 s, lejos del plazo de 20 s, así que esa clase
no es un problema de plazo: es lo que parece un AP caído o cobertura perdida. **Es otro fallo,
más leve, y no es el que deja al aparato mudo una noche.**

### 10.8 Lo que queda: el chip y su alimentación

Ni código ni AP deja al nRF7002 y a cómo lo alimenta esta placa. Los logs apuntan ahí por su
cuenta:

- Muere sólo en momentos de RF activa: a mitad de subida o en el primer escaneo; al final de la
  noche, en el primer mandato tras VIF UP.
- El inicio coincide con VBUS puesto en todos los arranques malos del 10 y del 11 y en el
  banco; el único arranque limpio corrió desenchufado desde el minuto 10.
- Se degrada a lo largo de la noche 3 mientras la celda se vacía.
- `usb_cdc.c` ya documenta que el controlador USB ve caer VBUS cada vez que se enciende la
  radio. Es una perturbación de alimentación que alguien ya observó.
- El chip cuelga de VBAT directamente, aguas arriba de la resistencia de medida del PMIC
  (`CLAUDE.md`).
- El 9 de septiembre, cuatro veces, el chip ni siquiera despertó al encender: `RPU enable
  failed`.

"Problema de hardware" aquí no significa necesariamente una unidad rota. Puede ser margen de
diseño en la alimentación del nRF7002, común a todas las unidades, o el propio firmware del
chip en esas condiciones. Las tres cosas que lo separan sin tocar el firmware están en §11.

Y un caso de uso que ahora pasa a ser el peor: **la base de carga de 20 aparatos**. Carga por
USB y sube por la noche. Si la correlación con VBUS es causal, la base es exactamente la
condición mala, y hay que meterla en la matriz de pruebas antes de que exista.

### 10.9 Añadido el 12 por la tarde: T-02 y los tres arranques que la rodean

Logs en `~/clip-log-dia4/` y `~/clip-log-dia5/` (esta última es la tarjeta completa, 20 ficheros).
Imagen: `fc56bfe` (L-015 revertido, con `bfe05a5`). Tres arranques nuevos, uno por condición:

| arranque | condiciones | ventanas | muertes del chip | tormentas |
|---|---|---|---|---|
| dia5 `log.0044:121`, día 11 diurno | batería, grabando, `APTO11-2G`, subidas en cada ventana | 22 de 26 asocian; 4 ventanas seguidas sin asociar de 01:08 a 02:02 (18 intentos), después 16 de 16 | 0 | 0 |
| dia5 `log.0044:1173`, tarde-noche del 11 | sin grabar, sin nada que subir, `APTO11-2G`; **batería hasta 02:09, USB desde entonces** con el PMIC sin ver la celda | **8 de 8 en batería; 2 de 7 con USB** | 2 `unresponsive` | 630 líneas |
| dia5 `log.0046:837`, T-02, noche 11 a 12 | batería desde 00:09, grabando, `CLIP_TEST` del portátil, sin internet | **31 de 31 asocian**; 30 de 30 ventanas en etapa 2 porque el portátil no tenía salida | 0 | 0 |

Lo que sale de los tres, en orden de peso:

1. **La muerte del chip no necesita tráfico ni grabación, y aparece al enchufar.** El arranque de
   la tarde del 11 es un A/B dentro del mismo arranque: mismo AP, mismo firmware, mismo sitio,
   ventanas de solo latido de 70 s. Ocho de ocho en batería; a las 02:09 entra VBUS (`cargador:
   error 0x40`, el PMIC sin ver la celda, ciclo off/on cada 5 min) y la ventana de las 02:38 muere
   en el escaneo 4 s después de encender, con tormenta, sin haber movido un byte. Corrige §10.8:
   el tráfico TLS no es condición necesaria. El instante del ciclo del cargador tampoco es el
   disparador: la primera muerte precede al ciclo #6 en 9 s.
2. **El recuento con VBUS es 6 de 6 y en batería 0 en 97 ventanas.** Con VBUS: los tres arranques
   del 10 por la tarde, la noche 3 (VBUS a las 00:04), la tarde del 11 y el banco del 9. En batería:
   la noche 2 (39 ventanas), el día 11 diurno (26) y T-02 (32), ninguna muerte. En esta unidad
   VBUS implica siempre "PMIC sin ver la celda" (`err 0x11` o `0x40`, `status 0x00`), así que VBUS
   y G7 están confundidos: hace falta una unidad cuyo PMIC sí detecte la celda, o arreglar la
   detección, para separarlos.
3. **El AP de casa sí tiene una avería propia, y es otra clase de fallo.** El día 11 diurno pasó
   55 minutos sin asociar con el chip sano: encendidos de 0,6 s, `SET country` contestado, apagados
   limpios, ni una línea de `wifi_nrf`, y se recuperó solo. Es la clase de §10.7. El portátil no la
   mostró en 31 ventanas. Esa avería alimenta el detector de cuelgue y produce ventanas en etapa 1
   iguales que las de la radio muerta; hay que distinguirlas por la ausencia de `Invalid memory
   address` y `RPU is unresponsive`.
4. **Las balizas del AP de casa quedan exoneradas.** Durante T-02 el `APTO11-2G` siguió en el aire
   (la primera ventana del arranque asoció a él antes del cambio de SSID) y el chip escaneó 31
   veces sin morir.
5. **T-01 queda falsada, como dice el cuaderno**, y T-02 no prueba que el AP sea la causa de nada:
   corrió sin VBUS y sin tráfico, que son justo las condiciones de todos los arranques limpios.
6. **El detector reinició un aparato sano** al parar la grabación de T-02, porque un endpoint
   inalcanzable produce la misma señal que una radio muerta. Falso positivo confirmado.

Lo que cambia en §11: la prueba decisiva ya no es el AP ni el atraso, es el cable. Un aparato en
reposo, sin grabar, con USB puesto y el PMIC en G7, contra el portátil o contra el AP de casa:
si muere, el AP y el tráfico quedan fuera a la vez. Y la pregunta de la noche 1 se resuelve en la
base: `JSON_EXTRACT(raw, '$.charging')` de `health_beats` alrededor de las 04:4x UTC del 10.

---

## 11. Qué hacer ahora: pruebas, en orden de coste, sin tocar el firmware

### 11.1 Recuperar los 15 minutos que faltan de la noche 3

La tarjeta guarda 20 ficheros y `log.0039` sigue ahí, ahora completo. Contiene el apagado de la
ventana 1 (uptime 00:04) y el encendido de la ventana 2 (00:18): dice si el chip murió en el
apagado tras una sesión limpia o al encender 14 minutos después, y qué pasó con el USB y el
inicio de la grabación entre medias. Copiar `log.0039` y `log.0040` de la tarjeta **con el
aparato parado**, no durante una prueba. Buscar en el tramo 00:04 a 00:18: `RPU is
unresponsive`, `VBUS retirado`, `USB auto-disable`, `Recording`, `Invalid memory address`.

### 11.2 La prueba del AP

Vale hacerla, pero sabiendo qué puede decir. Mismo protocolo de §8, y además:

- Sin USB **desde antes de arrancar** y hasta recoger. Nada de copiar logs a mitad.
- Que haya atraso que drenar en la primera ventana, porque ahí murió el chip cuatro de cuatro
  veces: dejar ficheros sin subir de la sesión anterior, o encolar una con `AT+HTTPUP=<sesion>`
  antes de desenchufar.
- Apuntar hora de arranque, hora de desenchufar, hora de empezar a grabar y voltaje inicial.

Al leer la tarjeta, tres huellas y qué significa cada una:

| huella en el log | qué significa |
|---|---|
| `Invalid memory address 0xAAAAAAAA` o `RPU is unresponsive` | el chip; el AP no interviene |
| `no result event` con `sin prestamos, se apaga` limpio y sin errores `wifi_nrf` | compatible con AP o cobertura (la clase de §10.7) |
| `STA disconnected` con un préstamo vivo | el AP o la cobertura nos echaron |

Una sesión buena con el AP nuevo no prueba nada: el AP de casa dio 39/39 y luego 3/35 en dos
arranques seguidos. **Una sola tormenta con otro AP cierra la teoría del AP** para el fallo de
este documento.

⟵ **Hecha la noche del 11 al 12 (T-02 en el cuaderno).** 31 de 31 asociaciones en 8 h 29 min
contra el portátil, sin una tormenta ni un `unresponsive`. Lo que demuestra y lo que no, en
§10.9: corrió en batería y sin tráfico, que son las dos condiciones en que la radio nunca ha
muerto en ningún registro.

### 11.3 Reproducir el inicio en el banco

El inicio se ha visto cuatro veces con las mismas condiciones, así que es reproducible sin
esperar una noche. Matriz mínima, una fila por vez, varias repeticiones cada una:

| condición | cómo | qué mirar en el log de la tarjeta |
|---|---|---|
| A. USB puesto, sin grabar, atraso de más de 100 ficheros pequeños | encolar la sesión con `AT+HTTPUP=<sesion>` o esperar la ventana de los 2 minutos tras arrancar | `Failed to get station info`, `connect crudo: ret=-1 errno=116`, `RPU is unresponsive` durante o tras el drenaje |
| B. Batería, desenchufado, mismo atraso | encolar por cable, desenchufar antes de que abra la ventana | lo mismo; si A muere y B no, el cable es causal, no correlación |
| C. USB con la celda detectada frente a estado G7 | `AT+HEALTH?` o el latido: `batt_det`; provocar G7 si hace falta con el ciclo off/on | separa "USB" de "cargador que no ve la celda" |
| D. Batería, atraso de pocos ficheros grandes frente a muchos pequeños | dos sesiones preparadas | separa "muchas conexiones TLS" de "mucho tiempo de TX" |

Los arneses de `applications/clip/tests/hil/` hablan por cable, así que sirven para A y C
(`traffic_gate.py` es exactamente "apagar sobre un bus que acaba de trabajar") y no para B ni
D, que son pruebas de campo cortas: arrancar, encolar, desenchufar, esperar una ventana, leer.

### 11.4 Una segunda unidad

Misma imagen, misma matriz. Si la segunda unidad muere igual, es diseño o firmware del chip; si
no, es esta unidad. Es la única prueba que distingue las dos cosas, y no necesita más que otro
Clip.

### 11.5 Medidas eléctricas

Sin sonda de depuración, pero con multímetro u osciloscopio sobre la placa:

- **Entre ventanas** (14 minutos de "radio apagada"): tensión en las redes BUCKEN (P0.11) e
  IOVDD_CTRL (P0.26) y en el raíl IOVDD del nRF7002. Si no están a cero, el chip nunca se apaga
  en tiempo de ejecución, y eso explica por qué sólo cura el reinicio.
- **Al empezar un escaneo y durante una subida**: VBAT en el pin del nRF7002, con y sin USB, con
  la celda llena y a 3,4 V. Se busca un hundimiento por debajo del mínimo del chip en el flanco
  de RF.
- **Al encender la radio, con USB puesto**: VBUS en el conector. El controlador USB ya "ve"
  caer VBUS ahí; medirlo dice si es real o es el detector.

### 11.6 Cuando se pueda tocar el firmware: instrumentación, no arreglos

Lo único que un cambio de software aporta al diagnóstico es información que hoy se tira:

- El código de retorno de `rpu_read()` en `zep_shim_qspi_read_reg32()` y las palabras crudas de
  `qspi_hl_readw()`, una vez por tormenta. Separa "el nRF5340 rechazó la transferencia" de "el
  chip devolvió su patrón".
- Un contador de fallos de bus y de `RPU is unresponsive` en el latido, y que
  `nrf_wifi_if_stop_zep()` devuelva error cuando el chip no contesta, para que `tdfail` deje de
  ser ciego.
- Estadísticas de los dos heaps del driver (`wifi_drv_ctrl_mem_pool`, `wifi_drv_data_mem_pool`)
  en el latido; `heap_free` no los ve.
- `CONFIG_NET_MGMT_EVENT_LOG_LEVEL_WRN` para ver eventos de red descartados por cola llena.

---

## 12. Paliativos: que funcione aunque el hardware sea el sospechoso

Ordenados por lo que arreglan frente a lo que cuestan. Ninguno cura al chip; A y B convierten
"mudo toda la noche" en "una ventana mala", que es la diferencia entre perder 7 h de entrega y
perder 15 minutos.

| | paliativo | qué arregla | coste | riesgo |
|---|---|---|---|---|
| A | Apagado real del chip entre ventanas | la persistencia: el chip colgado vuelve a arrancar limpio | decenas de líneas, flash despreciable | bajo; hay que medir que los pines a bajo no consuman |
| B | Recuperación de radio sin reiniciar el SoC | recuperar **grabando**, que hoy está prohibido | el detector existente más A | bajo |
| C | Menos exposición: sólo 2,4 GHz, una conexión TLS para muchos ficheros | menos tiempo de RF por ventana; menos ocasiones de morir | mediano; ya está en el backlog (T2.3.6) | bajo |
| D | Serializar nuestra capa de radio | fin de la tormenta de reintentos, logs legibles, menos radio en ventanas malas | mediano | bajo |
| E | Parches al driver de Nordic | no corromper la cola del chip, `tdfail` honesto, código de error visible | pequeño, como los parches de MCUboot | medio: es árbol de NCS |
| F | Experimentos de margen de bus | saber si el bus contribuye | Kconfig y DTS | bajo, pero sólo bajo gate |

**A. Apagado real.** Tras `net_if_down()`, configurar P0.11 y P0.26 como salidas a bajo desde la
aplicación y mantenerlas así hasta el siguiente encendido; el driver las reconfigura como
salidas en `rpu_init()`, así que no hace falta tocarlo. Alternativa: parche a `rpu_gpio_remove()`
para que deje `GPIO_OUTPUT_INACTIVE` en vez de `GPIO_DISCONNECTED`. Verificación: el guion de
la noche mala, ventana 2 en adelante, debe volver a asociar. Si no lo hace, el estado no vive en
la alimentación del chip y A se retira.

**B. Radio en vez de SoC.** Con A funcionando, el detector de cuelgue puede hacer un ciclo de
alimentación completo del chip, con espera de un segundo con los pines a bajo, sin tocar el
audio, y dejar el reinicio como último recurso. Es la "recuperación mientras se graba" de §7,
sin partir la sesión.

**C. Exposición.** El AP es de 2,4 GHz y el aparato escanea las dos bandas
(`WIFI_FREQ_BAND_UNKNOWN`): fijar 2,4 GHz acorta el escaneo y quita los probes de 5 GHz. Las
sesiones que mataron al chip fueron 116 apretones de manos TLS en un minuto; una conexión
persistente para todos los ficheros de una ventana reduce eso a uno. Ambas cosas ahorran
batería aunque el chip fuera perfecto.

**D. Serializar.** Un solo hilo dueño de `wifi_sta_on()` y `wifi_sta_off()`; el vencimiento
del préstamo encola la orden de apagar en ese hilo en vez de ejecutarla en la cola del
sistema; ningún reintento con el préstamo a cero; DISCONNECT siempre antes de `net_if_down()`,
asociado o no, y esperar a que el driver no tenga escaneo en curso. Quita las 150 asociaciones
por noche y los 560 timeouts; deja el log con una línea por ventana.

**E. Driver.** Tres cambios pequeños en el árbol de NCS, con el mismo flujo que
`patches/mcuboot/`: no escribir `0xAAAAAAAA` de vuelta en `hal_rpu_hpq_dequeue()`; que
`nrf_wifi_if_stop_zep()` devuelva error cuando `chg_vif_state` falla; y registrar el código de
error de bus en el shim. El primero evita que una lectura mala corrompa la cola del chip; los
otros dos son instrumentación.

**F. Margen de bus.** El gate pendiente de `CONFIG_NRF70_QSPI_LOW_POWER=n` (el de `prj.conf`
que nunca se corrió con la tarjeta libre), y bajar `qspi-frequency` a 8 MHz en el DTS. Si las
tormentas desaparecen a 8 MHz, el bus contribuye y hay que mirar la latencia del esclavo. Sólo
bajo gate de asociación, nunca en una imagen de campo sin medir.

**Lo que no arregla esto.** L-015 cambia tiempos, no mecanismo. Quitar el AP del suplicante da
104 KB de flash pero no toca nada de lo de arriba. Reiniciar el SoC seguirá curando porque es A
hecho por accidente.

**Para el equipo de hardware**, si §11.4 y §11.5 apuntan a diseño: pull-down en BUCKEN e
IOVDD_CTRL para que el chip no dependa de que el host los conduzca; capacidad de reserva en
VBAT del nRF7002 para los flancos de RF; y una medida de VBAT en el pin del chip durante una
subida con la celda a 3,4 V.
