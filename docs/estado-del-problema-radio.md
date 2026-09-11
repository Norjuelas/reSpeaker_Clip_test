# La radio se queda sin asociar — estado del problema

Documento de traspaso. Todo lo que hace falta para retomar este problema sin repetir
camino. Última actualización: **2026-09-11**.

Complementa, no sustituye:
- `docs/cuaderno-de-laboratorio.md` — un apunte por cambio, con resultado real (L-001…L-015).
- `CLAUDE.md` — invariantes y escollos del proyecto.
- `25-backlog-bateria-y-transmision.md`, fuera de este repo, en `docs-respeaker/`.

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
2. **Un reinicio del SoC sí lo limpia**, en segundos. Observado tres veces.
3. **wpa_supplicant deja de contestar a su propio socket de control.** `DISCONNECT`,
   `REMOVE_NETWORK` y hasta `SET country` expiran a los 15-20 s. `SET country` no toca el
   aire, así que **no es cobertura**.
4. **Ya está roto antes de que la ventana empiece.** El `remove_network all` fallido cae
   20 s *antes* de que la ventana pida la radio. Cada ciclo hereda el destrozo del anterior.
5. **`no result event`**: el driver nunca entrega el resultado de la asociación.
6. **Matábamos el reintento del driver.** Él se programaba 138 s; nosotros esperábamos 45
   y soltábamos, y la radio se apagaba 45 s después: 90 s. **Su recuperación no llegó a
   intentarse ni una vez en toda la noche.** Esto es lo único demostrablemente mal que
   hacíamos nosotros, y es lo que ataca L-015.
7. **Hay al menos dos firmas distintas**, y puede que sean dos fallos:
   - **A**: tormenta de timeouts de wpa_supplicant, **sin** `0xAAAAAAAA` (noche 3)
   - **B**: tormenta de `0xAAAAAAAA` (398-400 por tanda), **sin** un solo timeout
   `0xAAAAAAAA` es patrón de memoria sin inicializar: el driver calcula una dirección del
   RPU que es basura.

---

## 4. Lo que está DESCARTADO, y con qué evidencia

No volver a proponerlo sin evidencia nueva.

| Hipótesis | Por qué cae |
|---|---|
| **Ahorro de energía del RPU** | Tres brazos, 10 asociaciones en frío cada uno: off **10/10**, on **0/12**, on con `RPU_RECOVERY` forzado a `n` **0/10**. `NRF_WIFI_LOW_POWER=n` es decisión medida |
| **"Reencender la radio en vez de reiniciar"** | Ya pasa: el vencimiento del préstamo llama a `wifi_sta_off()` → `net_if_down()`. Ocurrió 29 veces y no arregló nada |
| **Fuga de heap** | `heap_free` plano toda la noche: 42.488 en cada latido de apertura, 34.480 en cada cierre, sin deriva en 2 h |
| **La carga degrada el wifi** | 7 días y 694 latidos: RSSI medio **−50,4 cargando** vs **−50,7 sin cargar**. 0,3 dB |
| **Sólo cobertura débil** | `SET country` no toca el aire y aun así expira |
| **Nuestro apagado corrompe al suplicante** | `STA disconnect failed` aparece **0 veces**; los comandos que expiran son del camino de reconexión del driver, 45 s antes de que corra nuestro apagado |
| **El barrido se colgaba** | Si se hubiera colgado, `wedge_note_window()` no habría corrido y no habría reinicio. Reinició: las ventanas seguían ejecutándose |
| **La carga la provoca (RSSI)** | Ver arriba; el −70 dBm de una tanda fue por moverlo, no por el cargador |

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

---

## 7. Lo que está en marcha y lo que queda

### En marcha

**L-015** (`9e21bfd`) — espera de enlace adaptativa: 45 s normal, **150 s** (> los 120 del
backoff del driver) durante las 3 primeras ventanas malas. Ataca el hecho 6. Flasheado el
2026-09-11, **sin verificar en campo**. Verifica el *mecanismo*, no la *cura*: que el
reintento del driver llegue a correr no garantiza que recupere un suplicante atascado.

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
