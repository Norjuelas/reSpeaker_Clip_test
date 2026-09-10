# Cuaderno de laboratorio

Un apunte por cambio. Se escribe **cuando se hace el cambio**, no después, y se
vuelve para rellenar el resultado cuando hay una medida.

Existe porque este proyecto tiene un historial largo de cambios que se dieron por
buenos y no lo eran: cinco teorías sobre H2 tumbadas por la evidencia, un
diagnóstico de la MAC desmentido en cinco minutos, y un arreglo de la tarjeta que
pasó dos noches sin llegar a ejercitarse nunca. Lo que evita repetir terreno no es
la lista de commits, es saber **qué se esperaba, qué implicaba, y qué pasó de verdad**.

Estados: 🟢 verificado · 🟡 sin verificar · 🔴 refutado · ⚪ no aplica

---

## L-001 · Los arneses de banco salen de `/tmp` y se niegan a medir sucio
**`4b58d38` · 2026-09-09 · 🟡 parcialmente verificado**

**Para qué.** Los cuatro guiones que produjeron las cifras de H2 (`1/10` → `20/20`)
vivían en el directorio temporal de la sesión. Lo caro no eran las 242 líneas sino la
comparabilidad: dos números solo significan algo si los produjo el mismo guion con los
mismos plazos.

**Qué cambia.** `applications/clip/tests/hil/` con `assoc_gate.py`, `traffic_gate.py`,
`reboot_gate.py` (antes `assoc_test.py`), `watch_charge.py` y `bench.py`.
`require_clean_bench()` aborta ante los dos confusores conocidos: lease de radio sin
soltar y tarjeta montada en el host.

**Implicación.** Los tiempos se conservaron exactamente (margen 6 s, plazo 60 s, mismo
criterio de éxito) para que las cifras históricas sigan siendo comparables. Un cambio
futuro de esos plazos invalida la línea base y hay que anotarlo aquí.

**Cómo se verifica.** `test_bench_guard.py` con un puerto de mentira, y la rama de la
lease contra hardware real.

**Resultado.** Guardia probada 4/4 con el puerto falso, incluida la distinción que
importa: ventana pasajera (1→0) pasa, lease colgada (1 persistente) aborta.
**Sin probar contra hardware**: la rama de la lease nunca se ejercitó con un aparato
real. Pendiente.

---

## L-002 · La pasada de subida no montaba la tarjeta
**`1780026` · 2026-09-09 · 🟢 VERIFICADO 2026-09-10**

**Para qué.** El 2026-09-09 se quedaron 1,2 MB de audio sin enviar 50 minutos con la
red viva (los latidos salieron en las tres ventanas del hueco). Reproducido dos veces
el mismo día. La tarjeta se apaga sola a los 45 s y `http_upload.c` era el único
consumidor que no la volvía a montar: `storage_list_sessions()` devolvía `-EINVAL`,
los dos bucles daban cero vueltas, y el aparato publicaba `pending_files: 0`.

**Qué cambia.** Tres cosas en `periodic_work_fn()` y `clip_sd_busy()`:
`storage_ensure_mounted()` antes de listar; un atómico `sweep_active` que
`clip_sd_busy()` consulta para no cortar el rail a mitad de una lectura; y `found < 0`
tratado como error en vez de como "no hay nada".

**Implicación.** La tarjeta se enciende cada 15 minutos aunque no haya nada que subir
— coste de energía no medido, pequeño frente a los ~20 s de radio de la ventana, pero
real. El atómico tiene que limpiarse en **todas** las salidas o la tarjeta no vuelve a
dormir nunca; se limpia en la etiqueta `release:`, igual que el préstamo de radio.
No se usó `status.state` bajo `status_lock` a propósito: `clip_sd_busy()` corre con
`sd_lifecycle_mutex` cogido y sería una inversión de cerrojos.

**Coste medido.** Mismo árbol con y sin el cambio: 924.996 → 925.252 B. **+256 bytes**,
99,10% → 99,13% de los 933.376 que ofrece el enlazador. Cero avisos del compilador.

**Cómo se verifica.** Una ventana cuyo latido de **apertura** lleve `sd_mounted: 0` y en
la que el fichero suba igualmente. Requiere el aparato **con batería y en reposo**: con
cable la tarjeta no se apaga nunca, y grabando tampoco.

**Resultado. 🟢 Verificado en hardware el 2026-09-10 11:22 UTC**, al tercer intento
(los dos primeros no ejercitaron nada: uno con cable, otro grabando toda la noche).

```
11:22:19  uptime 3270  sd_mounted 0  pending 1  up_ok 71   <- apertura, tarjeta APAGADA
11:22:32                0001.opus  196.282 B  ok=1         <- sube el fichero
11:22:33  uptime 3285  sd_mounted 1  pending 1  up_ok 72   <- cierre, tarjeta MONTADA
```

`recording: 0` en todas las filas, así que la prueba fue válida. La tarjeta pasó de 0
a 1 **dentro de la misma ventana**, que es exactamente lo que hace el arreglo.

**Cabo suelto.** El latido de las 11:07:39 (`sd_mounted: 1`, `pending: 1`, `up_ok: 71`,
**sin latido de cierre**): esa ventana tenía la tarjeta arriba y un fichero pendiente y
no subió nada. No afecta al veredicto, pero es la misma *forma* que el fallo arreglado.
Vigilar.

---

## L-003 · Documentación: lo aprendido persiguiendo L-002
**`5fb0375` · 2026-09-10 · ⚪**

**Para qué.** El README describía el aparato de v0.0.8 (BLE, `ClipAP_XXXX`, UDP, SDKs
móviles) y enlazaba a tres ficheros que no existen. Y ocho hallazgos de la caza de
L-002 no estaban en ningún sitio donde se fueran a leer.

**Qué cambia.** README reescrito. En `CLAUDE.md` y `skills/clip-dev/`: el log de la
tarjeta solo cubre los primeros 120 s de cada arranque; solo `storage_ensure_mounted()`
rearma el temporizador de inactividad; un cable esconde toda esta clase de fallo;
grabar está vetado con VBUS presente; `AT+USB=off` se lleva el CDC; `reset: unknown` es
un arranque en frío de verdad y `software` es un `sys_reboot()` deliberado
(`CONFIG_RESET_ON_FATAL_ERROR` no está puesto, así que un cuelgue **para** el micro);
`pending_files` es el número de la ventana anterior; y dónde viven los datos de campo.

**Implicación.** `CLAUDE.md` se carga en cada sesión, así que un dato equivocado ahí
cuesta tiempo repetidamente — la afirmación de que el log de la tarjeta era "la única
ventana a un aparato que ha dejado de responder" mandó a buscar en un log que se corta
a los 120 s.

---

## L-004 · La recuperación de radio no puede actuar mientras se graba
**pendiente · 2026-09-10 · 🟡 sin verificar**

**Para qué.** La noche del 2026-09-09 al 10 el aparato grabó 7 h 21 min y estuvo
**5 h 37 min sin dar señal**: último latido a las 04:50:09 con la radio sana
(asociada, IP, RSSI −52, `wifi_err` vacío, `repowers 0`), y nada hasta que se paró la
grabación. Entonces reinició en el mismo minuto (`reset: software`, arranque 10:27:50).

Eso confirma que `wedge_note_window()` estuvo contando toda la noche y difiriendo el
reinicio por esta guarda:

```c
if (audio_is_recording()) {
    return;               /* el contador se deja como esta */
}
```

El razonamiento de la guarda es correcto —el audio vale más que la conectividad— pero
el remedio no: trata el reinicio como la única recuperación, así que cuando reiniciar
no vale **no hace nada**. Un aparato de tienda grabando una jornada puede perder la
radio en la primera hora y seguir mudo hasta que alguien lo toca.

Esto **no es un hallazgo nuevo**: el backlog ya dice desde el 2026-09-07 que la causa
raíz del cuelgue del RPU sigue abierta y que la mitigación excluye la grabación a
propósito. Lo nuevo es el precio medido de esa exclusión.

**Corrección del 2026-09-10 — dos hipótesis mías, las dos refutadas.**

1. *"Que la recuperación reencienda la radio en vez de reiniciar."* **No sirve.**
   `wifi_lease_expire_handler()` (`wifi.c:743`) ya llama a `wifi_sta_off()` al llegar a
   cero préstamos, y desde `688496b` esa función llega a `net_if_down()`
   incondicionalmente — lo único que corta BUCKEN e IOVDD. **La radio ya se apaga
   entera entre cada dos ventanas.** Anoche arrancó en frío unas 22 veces y falló las
   22. Propuse ese arreglo dos veces razonando desde el comentario del detector en vez
   de mirar qué hace el vencimiento del préstamo.
2. *"Fuga de heap."* **Refutada por los datos.** `heap_free` fue plano toda la noche:
   42.488 en cada latido de apertura y 34.480 en cada uno de cierre, sin deriva en 2 h.

**Lo que sí dicen los datos.** Todas las ventanas venían en pareja (apertura + cierre)
hasta la de las 04:50:09, que abrió y **nunca cerró**. Y como el reinicio de las
10:27:50 solo puede venir de `wedge_note_window()` —que corre al final de una ventana
completa— las ventanas siguieron ejecutándose: no se colgó el barrido, **la radio dejó
de asociar**. Eso es la causa raíz de H2, que el backlog lleva abierta desde el
2026-09-07: algo del lado del host sobrevive a `rpu_pwroff()` y solo lo limpia un
reinicio del SoC.

**Qué cambia.** Nada todavía en la recuperación: diseñar el remedio antes de saber qué
está roto es exactamente cómo salieron cinco teorías equivocadas sobre H2. Primero
L-005 (visibilidad). La opción honesta que queda para grabando es **cerrar el trozo en
curso y reiniciar** (`POWER_OFF_EXEC` ya hace justo eso), a cambio de partir la sesión
en dos — decisión de producto, no técnica.

**Resultado.** Abierto. Depende de L-005.

---

## L-005 · El latido cuenta lo que pasó mientras no podía contar nada
**pendiente de commit · 2026-09-10 · 🟢 verificado en banco, 🟡 sin verificar en campo**

**Para qué.** Cuando la radio se cae, el único canal que informa de la salud es el que
se cae. Para entender las 5 h 37 min mudas de L-004 hicieron falta cuatro consultas al
servidor y dos hipótesis equivocadas — y aun así no se supo **en qué** se rendían las
ventanas, que es justo el dato que decide el arreglo.

**Qué cambia.** Tres campos nuevos en el latido, acumulados en RAM y publicados en el
primer latido que consigue salir:

| campo | qué dice |
|---|---|
| `miss` | ventanas seguidas sin un solo éxito. Si la racha ya terminó, la última que hubo — el latido de la recuperación es el que tiene que contar el tamaño del agujero |
| `miss_stage` | dónde se rindió la última ventana mala (`enum clip_win_stage`) |
| `ok_age_s` | segundos desde la última ventana que logró algo |

Etapas: 0 ok · 1 no asoció · 2 asoció y falló el latido · 3 fallaron las subidas ·
4 subida ya en curso · 5 la tarjeta no montó · 6 sin heap · 7 no se pudo listar.
**Son contrato con el panel: añadir al final, nunca reordenar.**

Lo de anoche habría sido, de un vistazo: `miss=22 stage=1 ok_age_s=20200`.

**Implicación.** Los contadores viven fuera de `#if CONFIG_CLIP_WIFI_WEDGE_RECOVERY`, así
que la contabilidad existe aunque la recuperación por reinicio se compile fuera.
`win_note()` corre en el mismo sitio que `wedge_note_window()` — solo si hubo préstamo,
o sea solo si hubo ventana de verdad; un aparato sin endpoint provisionado sale por
`reschedule` y no cuenta como ventana mala.
**Límite conocido:** un reinicio se lleva los contadores, así que un agujero que acaba
en reinicio del detector de cuelgue se pierde. Eso lo tiene que cubrir el log de la
tarjeta (L-006), no esto.

**Coste medido.** 925.252 → 925.652 B, **+400 bytes**, 99,13% → 99,17%. RAM +16 B.
Quedan 7.724 bytes libres. Cero avisos del compilador.

**Cómo se verifica.** Provocar una ventana mala con el endpoint apuntado a un puerto
muerto y comprobar que `miss` y `miss_stage` la reflejan con la etapa correcta; después
restaurar el endpoint y comprobar que el latido de la recuperación sigue contando el
tamaño del agujero.

**Resultado. 🟢 Primera mitad verificada en banco el 2026-09-10.** Con el endpoint en el
puerto 4443 y el AP bueno:

```
up= 137s  miss=0 stage=0  wifi=True     <- la radio asocia
up= 160s  miss=1 stage=2  wifi=True     <- ventana mala contabilizada
```

`stage=2` es lo importante: distingue correctamente "asoció y falló el latido" de "no
asoció". Esa distinción es exactamente la que no se pudo hacer con los datos de anoche.
Segunda mitad (que el latido de recuperación conserve `miss`) en curso.

---

## L-006 · Log en la tarjeta cuando hay problema *(propuesto, sin escribir)*

**Para qué.** L-005 se pierde en un reinicio, y el log de la tarjeta se retira a los
120 s de cada arranque (`CLIP_LOG_FS_BOOT_WINDOW_S`), así que un agujero que acaba en
reinicio no deja rastro en ningún sitio.

**Idea.** Reactivar el backend de log en tarjeta mientras `win_bad_run > 0`, y retirarlo
otra vez al recuperarse. El log aparece exactamente cuando hay algo que registrar, y de
paso caen en la tarjeta las líneas del detector de cuelgue, incluida la de
"reiniciando en frío" — que hoy no las ve nadie.

**Implicación a vigilar.** `clip_log_fs_active()` es una de las condiciones de
`clip_sd_busy()`, así que mientras el log esté activo **la tarjeta no duerme**. Es
aceptable justo en ese caso (si la radio está rota no se está ahorrando nada útil), pero
hay que asegurarse de que se retira al recuperarse o el ahorro se pierde para siempre.

---

## Observaciones sin cambio asociado

- **El latido falla con `-ENOMEM` mientras se drena un atraso grande** (2026-09-10).
  Durante 23 minutos drenando 88 ficheros, `beat_err` fue `-12` en todos los sondeos y
  el heap se quedó en 44.888; al terminar subió a 57.520 y el latido siguiente salió a
  la primera. O sea: un aparato drenando está **invisible en el panel** todo el rato que
  dure, trabajando perfectamente. Contención de heap entre la sesión TLS de la subida y
  la del latido. `last_fail_stage` y `last_fail_heap` ya existen para acotarlo.
- **Reinicios `software` sin explicar** alrededor de eventos de enchufar/desenchufar.
  Con `CONFIG_RESET_ON_FATAL_ERROR` sin poner, un cuelgue no puede producirlos: son
  `sys_reboot()` deliberados, y solo hay cuatro sitios que lo llaman.
- **`repowers` sigue en 0** tras toda esta actividad. Alimenta T2.7.4: si tras la prueba
  de campo sigue en 0, `CLIP_WIFI_RPU_PROBE` se quita (~90 líneas que nunca han hecho
  nada). Y explica por qué no ayudó anoche: la sonda solo corre al **arrancar** la
  radio, no cuando un enlace ya establecido se muere.
