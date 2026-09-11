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
**`4b58d38` · 2026-09-09 · 🟢 verificado 2026-09-10**

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

**Resultado. 🟢 Cerrado el 2026-09-10.** Guardia probada 4/4 con el puerto falso,
incluida la distinción que importa: ventana pasajera (1→0) pasa, lease colgada
(1 persistente) aborta.

Y **contra hardware real**, con una lease de `AT+STA=on` de verdad colgada:

```
sucio  : "hay 1 lease(s) de radio sin soltar tras 60 s"  -> salida 2, NO escribe fichero
limpio : 2/2, 10,3 s por asociación, -44/-48 dBm         -> salida 0
```

Las dos mitades importan. Que aborte prueba que el confusor se detecta; que **no cree el
fichero** prueba que no queda una medida a medias que alguien pueda leer como buena. Y
que en limpio no dé falso positivo es lo que evita que la guardia acabe esquivándose con
`--force`, que sería como no tenerla.

Los 10,3 s coinciden exactamente con el gate de 20/20 del arreglo de H2: mismo guion,
mismos plazos, así que las cifras siguen siendo comparables entre sí. Que era justo el
motivo de sacar los arneses de `/tmp`.

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
**`4556fb1` · 2026-09-10 · 🟢 verificado en banco**

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

## L-006 · Log en la tarjeta cuando hay problema
**`71d8a68` · 2026-09-10 · 🟢 verificado en banco**

**Para qué.** L-005 vive en RAM y un reinicio se lo lleva — y reiniciar es justo lo que
hace el detector de radio colgada al llegar a su umbral. El log de la tarjeta, que sería
lo único que sobrevive, se retira a los 120 s de cada arranque
(`CLIP_LOG_FS_BOOT_WINDOW_S`). Resultado: la noche del 2026-09-09 al 10 el aparato estuvo
5 h 37 min mudo y **no quedó una sola línea** de por qué.

**Qué cambia.** `clip_log_fs_trouble(bool)` en `at_commands.c`, llamada desde
`win_note()`: se enciende el backend de log a la tarjeta en cuanto una ventana no logra
nada, y se retira al recuperarse o al agotar `CLIP_LOG_FS_TROUBLE_WINDOWS` (5 por
defecto). Se enciende **antes** de escribir la línea de la ventana mala, o la primera
—la que dice cómo empezó todo— no quedaría registrada; y se retira **después** de la
línea de recuperación, para que el fichero acabe diciendo cómo terminó.

**Implicación.** Mientras el log está activo, `clip_log_fs_active()` hace cierto a
`clip_sd_busy()` y **la tarjeta no duerme**. Por eso el cupo: interesa el comienzo del
problema, no repetir el mismo fallo veinte veces con el rail encendido en un aparato a
batería. Con 5 se cubren con margen las 3 ventanas que disparan el reinicio.
Una petición explícita de `AT+LOG` manda en los dos sentidos: si alguien pidió logs no
se los quitamos, y si los apagó no se los devolvemos.

**Coste medido.** 925.716 → 926.100 B, **+384 bytes**, 99,18% → 99,22%. Quedan 7.276
libres. Cero avisos del compilador.

**Cómo se verifica.** Provocar una racha mala (endpoint muerto) y comprobar que el log
se reactiva solo después de haberse retirado; después restaurar el endpoint y comprobar
que se retira otra vez.

**Resultado. 🟢 Primera mitad verificada en banco el 2026-09-10** (`71d8a68`):

```
up= 98s  miss=0            log=info   <- el por defecto de arranque
up=139s  miss=0            log=off    <- retirado a los 120 s
up=160s  miss=0            log=off
up=180s  miss=1 stage=2    log=info   <- vuelve SOLO con la ventana mala
```

Segunda mitad (que se retire al recuperarse) en curso. Es la que importa para la
seguridad del cambio: si no se retirara, la tarjeta no volvería a dormir nunca y el
ahorro que da el apagado por inactividad se perdería en silencio.

**Sin comprobar todavía:** que las líneas lleguen físicamente a `/SD:/LOG` con marcas
posteriores a los 120 s. `AT+LOG?` dice que el backend está activo, que es el mismo
camino que usa `AT+LOG=info` — pero verlo en la tarjeta exige montarla, y montarla por
USB obliga a `AT+USB=on`, que se lleva el canal AT. Pendiente para la próxima vez que
haya que leer la tarjeta de todos modos.

---

## L-007 · Dos hilos mandaban el latido a la vez, y la ventana se contaba fallida
**`71d8a68` · 2026-09-10 · 🟢 verificado en banco**

**Para qué.** Lo encontró L-005 a los veinte minutos de existir. Con el endpoint bueno y
el AP sano, el banco daba `miss=2 stage=2` mientras `beat_err` valía 0 y `conn_errno`
116: un latido había salido bien **y** un connect había expirado, en la misma ventana.

La causa: cuando la asociación termina pasan dos cosas a la vez. `wifi.c:380` llama a
`health_beat_now()` desde el evento STA-up, y la ventana —que sondea el enlace cada
segundo— llega justo después a mandar el suyo con `post_health_inline()`. **Dos
handshakes TLS simultáneos al mismo endpoint, en hilos distintos, sin nada que los
serialice y con ~44 KB de heap.** Uno gana, el otro muere con `ETIMEDOUT` o `-ENOMEM`.

Y no era solo un POST desperdiciado: cuando perdía el de la ventana, `any_success`
quedaba en false y **la ventana se contaba como fallida con la red perfectamente sana**.
Eso alimenta `wedge_bad_windows`, y tres de esas reinician un aparato que no tiene nada
roto.

El comentario de `wifi.c` dice *"esto no duplica latidos: adelanta el que tocaba"* — y es
cierto para el temporizador de `health.c`. No sabía de este otro camino, añadido después
con lo de "un despertar, los dos trabajos".

**Qué cambia.** Un solo camino de POST. `post_health_inline()` ya no manda nada por su
cuenta: si acaba de salir un latido (≤30 s) lo da por bueno, y si no se lo pide al hilo
del latido y espera el resultado hasta 30 s.

**Implicación.** La ventana ahora puede esperar hasta 30 s extra al hilo del latido; ya
esperaba 45 s por el enlace, así que no cambia el orden de magnitud. En el latido de
cierre eso se paga con la tarjeta encendida (`sweep_active` sigue en 1).
**Regresión que hubo que cerrar:** delegar dejaba a `AT+HEALTH=off` silenciando también
el latido de la ventana, con lo que **todas** las ventanas habrían contado como fallidas
y el detector habría reiniciado un aparato sano cada ~45 min. Con el latido apagado no
hay competidor posible, así que ese caso vuelve al camino directo de antes.

**Coste medido.** 925.636 → 925.716 B, **+80 bytes**.

**Cómo se verifica.** `miss` tiene que quedarse en 0 a lo largo de varias ventanas con
red sana, y `conn_errno` en 0.

**Resultado.** Primera ventana limpia el 2026-09-10:

```
up=122s  miss=0  ok_age=122  wifi=False   <- aun sin exito: ok_age = uptime
up=153s  miss=0  ok_age=12   wifi=True    <- ventana OK, beat_age=22
```

Las dos edades juntas cuentan la historia: el latido de `wifi.c` salió en el uptime ~131,
y la ventana del ~141 lo vio con 10 s, lo dio por bueno y **no mandó un segundo POST**.
`conn_errno` se quedó en 0 toda la tanda, donde antes daba 116. Faltan dos ventanas más
para darlo por bueno.

---

## L-008 · El log sale gratis cuando la tarjeta ya está encendida
**`2cbad8b` · 2026-09-10 · 🟢 verificado en banco**

**Para qué.** El retiro del log a los 120 s (`CLIP_LOG_FS_BOOT_WINDOW_S`) era un
**temporizador puro**: disparaba sin mirar si la tarjeta estaba encendida por otra razón.
Y grabando la tarjeta está encendida siempre — `audio_is_recording()` es una de las
condiciones de `clip_sd_busy()`, así que la puerta de inactividad no llega a ejecutarse.
Resultado: la noche del 2026-09-09 al 10 se grabó 7 h 21 min con el raíl alimentado todo
el rato y el log apagándose solo a los dos minutos. Se tiró registro gratis.

**Qué cambia.** `clip_sd_busy_other_than_log()` separa "la tarjeta está ocupada por algo
que no es el log" de `clip_sd_busy()` — hacía falta porque el log es una de las
condiciones de esta última, así que preguntarle con el log encendido siempre dice que sí.
`log_fs_apply()` resuelve dos peticiones independientes: `log_req_trouble` (L-006, donde
el log **sí** cuesta porque despierta la tarjeta, de ahí el cupo) y `log_req_free` (la
tarjeta ya está encendida, no cuesta corriente). El tick de inactividad reevalúa antes de
intentar apagar, en ese orden, para que el mismo tick pueda apagar la tarjeta cuando deja
de hacer falta.

**Implicación.** Grabando, barriendo o con USB puesto ahora hay log continuo sin coste de
energía — sólo las escrituras. En reposo con la radio sana no hay log, como antes. Y el
retiro por ventana de arranque ya no apaga el log de una grabación en curso.

**Cómo se verifica.** Que el fichero de la tarjeta crezca con marcas muy posteriores a
los 120 s de arranque.

**Resultado. 🟢 Verificado el 2026-09-10.** `log.0038` llegó a `[00:22:52]` — once veces
la ventana de arranque. Y es lo que capturó L-009.

---

## L-009 · `net_if_down()` puede fallar, y descartábamos el aviso
**`2cbad8b` · 2026-09-10 · 🟡 sin verificar**

**Para qué — y esto es el hallazgo del día.** Con L-006 y L-008 registrando, el banco
capturó por primera vez el fallo de radio con detalle del driver:

```
[00:19:15] <err> wifi_nrf: hal_rpu_mem_write: Invalid memory address 0xAAAAAAAA
[00:19:15] <err> wifi_nrf: nrf_wifi_wpa_set_supp_port: nrf_wifi_sys_fmac_chg_sta failed
[00:19:23] <wrn> wifi: radio: sin prestamos, se apaga
[00:19:34] <err> wifi_nrf: nrf_wifi_sys_fmac_chg_vif_state: RPU is unresponsive for 10 sec
[00:19:34] <err> wifi_nrf: nrf_wifi_if_stop_zep: nrf_wifi_sys_fmac_chg_vif_state failed
```

`0xAAAAAAAA` es patrón de memoria sin inicializar: el driver está calculando una
dirección del RPU que es basura. Y la misma firma aparece en los logs **viejos** justo
antes de `radio colgada: 3 ventanas seguidas sin exito. Reiniciando en frio.` — o sea que
es la huella del H2 residual, no algo de hoy.

La línea que importa es la última: `nrf_wifi_if_stop_zep` **es** la implementación de
bajar la interfaz, lo que llama `net_if_down()`. Y en `wifi_sta_off()` el retorno se
descartaba.

**Corrige una conclusión mía de esta misma mañana.** Dije que la radio se apaga entera
entre cada dos ventanas y que por tanto un chip colgado no podía ser la causa — y con eso
descarté reencenderla como arreglo. Es cierto **sólo cuando el apagado funciona**. Cuando
el RPU no responde, lo que falla es justo el apagado: `rpu_pwroff()` no corre, el chip se
queda como estaba, y la ventana siguiente levanta una interfaz que nunca bajó.

**Qué cambia.** Se comprueba el retorno, se cuenta (`teardown_fails`), se registra con la
explicación, y se publica en el latido como `tdfail`.

**Implicación.** Sólo detección: no cambia ningún comportamiento de recuperación todavía.
A propósito — diseñar el remedio antes de saber qué está roto es como salieron cinco
teorías equivocadas sobre H2, y hoy van cuatro más mías. Con `tdfail` y `miss_stage`
juntos el diagnóstico son dos números: `tdfail` subiendo con `stage=1` es "el apagado
falló y ahora no levanta".

**Lo que NO se afirma.** Que esto explique las 5 h 37 min de anoche. Aquí se recuperó
solo: errores a las 00:19:34 y un `AT+STA=on` manual asoció normalmente a las 00:22:47,
tres minutos después, sin reiniciar. Misma firma, persistencia muy distinta.

**Coste medido.** 926.396 → 926.612 B, **+216 bytes**, 99,25% → 99,28%. Quedan 6.764
libres. Cero avisos del compilador.

**Cómo se verifica.** Que `tdfail` suba la próxima vez que aparezca la firma
`RPU is unresponsive` en el log de la tarjeta.

**Resultado.** Pendiente. Recién instalado el 2026-09-10; `tdfail=0` al arrancar.

---

## L-010 · Que un aparato de tienda pueda contar lo que le pasó ANTES de reiniciarse
**`ad59518` · 2026-09-10 · 🟢 verificado en banco (WEDGE y CLEAN)**

**Para qué.** Auditoría de lo que el latido podía contestar en remoto, que es lo único
que llega cuando no estás delante — el log de la tarjeta exige tenerla en la mano. El
agujero: **`miss`, `miss_stage` y `tdfail` viven en RAM, y el reinicio del detector de
radio colgada se los lleva.** Ese detector existe justamente para reiniciar tras tres
ventanas malas, así que en el fallo típico el aparato llega al servidor con todo a cero y
`reset: software`: dice que se rindió, no por qué.

La noche del 2026-09-09 al 10 sólo se salvó de eso por accidente — la grabación en curso
difirió el reinicio cinco horas y con ello conservó la evidencia. Un aparato de tienda
que no esté grabando reinicia a los ~45 minutos y no se aprende nada.

Y dos ambigüedades menores del mismo tipo: `reset_cause: unknown` no distingue *lo
apagaron* de *se quedó sin batería*, y no había forma de saber cuántas veces ha
reiniciado una unidad desde que se instaló.

**Qué cambia.** Una nota persistida de 16 bytes, `struct clip_boot_note`, en su propia
clave de settings (`clip/boot_note`) — **fuera del `config_table`** a propósito: esa tabla
mapea claves a campos de `struct clip_config` por `offsetof`, y añadir ahí cambiaría el
formato persistido de la configuración entera. Esto es diagnóstico, no configuración.

El mecanismo, y la parte que importa es la tercera:

1. Al arrancar se lee la nota anterior y se guarda en RAM para publicarla como `prev_*`.
2. `boots` se incrementa y persiste.
3. **Acto seguido se escribe una nota nueva con motivo `UNKNOWN`.** Así, si el aparato
   muere sin avisar — corte, batería, cuelgue — el arranque siguiente encuentra `UNKNOWN`,
   y eso *ya es información*. Sólo un apagado o un reinicio deliberado la sobrescriben
   con su motivo real antes de irse.

Motivos: `0` no se sabe · `1` apagado deliberado (ship mode) · `2` el detector de radio
colgada reinició en frío.

Campos nuevos en el latido: `boots`, `prev_why`, `prev_miss`, `prev_stage`, `prev_td`.

**Implicación.** Un reinicio del detector deja de ser opaco: llega
`prev_why=2 prev_miss=3 prev_stage=1 prev_td=2` y eso se lee como *"se rindió tras tres
ventanas sin asociar, con dos apagados de interfaz fallidos"* — sin tener el aparato
delante. No se intenta migrar notas de versión distinta: perder una nota vieja no cuesta
nada, malinterpretarla sí.

**Coste medido.** 926.612 → 927.324 B, **+712 bytes**, 99,28% → 99,35%. Quedan **6.052
libres**, y el latido pasa a 660 bytes de los 960 del buffer. Cero avisos del compilador.

**Cómo se verifica.** Tres caminos, uno por motivo.

**Resultado.**

- 🟢 **Persistencia y contador**, 2026-09-10: `boots` fue 1 → 2 a través de un
  `AT+REBOOT`, con `prev_why=0` — correcto, porque `AT+REBOOT` no se marca y "no se sabe"
  es la respuesta honesta.
- 🟢 **Motivo `CLEAN` VERIFICADO el 2026-09-10.** Apagado con el botón, dos minutos
  fuera, y encendido de nuevo:

  ```
  reset_cause : unknown     <- arranque en frio de verdad (RESETREAS = 0)
  prev_why    : 1           <- apagado deliberado
  boots       : 6
  ```

  Ese par es lo que se buscaba. Hasta hoy `unknown` significaba tres cosas
  indistinguibles: lo apagaron, se quedó sin batería, o se colgó. La tabla queda:

  | `reset_cause` | `prev_why` | qué pasó |
  |---|---|---|
  | `unknown` | 1 | lo apagaron a propósito |
  | `unknown` | 0 | batería agotada, celda retirada, o fallo duro |
  | `software` | 2 | el detector de radio colgada se rindió |
  | `software` | 0 | `AT+REBOOT`, o un reinicio del firmware sin marcar |
- 🟢 **Motivo `WEDGE` VERIFICADO el 2026-09-10.** Imagen de prueba con
  `CLIP_UPLOAD_INTERVAL_MIN=2` (2 y no 1: una ventana fallida tarda ~50 s y la gracia del
  préstamo otros 45, con 60 s se solaparían) y el endpoint en un puerto muerto:

  ```
  up=172s  miss=1 stage=2         <- ventana 1 falla
  up=352s  miss=2 stage=2         <- ventana 2 falla
           (sin puerto)           <- ventana 3: el detector reinicia
  up= 15s  boots=4  prev_why=2  prev_miss=3  prev_stage=2
  ```

  El aparato se reinició solo y volvió **diciendo por qué**. Cuatro cosas de una vez:
  L-005 contando bien (y `stage=2`, no 1 — asoció y falló el latido, correcto contra un
  puerto muerto), L-006 registrando durante toda la racha, L-010 sobreviviendo al
  reinicio en frío, y el propio detector siguiendo vivo después de que L-007 tocara
  `post_health_inline()` — que era un riesgo real.

  `prev_td=0` es correcto: en esta prueba la radio estaba sana y el apagado nunca falló.

  Después se reflasheó la imagen de producción y se verificó `intervalo=15` y
  `endpoint=:443`. El primer intento de reflasheo falló porque el aparato había vuelto
  como **ttyACM1** y el guion tenía ACM0 fijo — el escollo de siempre, y van dos veces.

---

## L-011 · Limpieza de `prj.conf` demostrablemente inerte
**pendiente de commit · 2026-09-10 · 🟢 verificado por construcción**

**Para qué.** `prj.conf` tenía 8 símbolos asignados dos veces y 22 líneas `CONFIG_BT_*`
de un Bluetooth que se compila fuera. Lo peligroso no es el desorden: es que en un
duplicado **gana la última asignación en silencio**, y tres fallos de este proyecto
entraron exactamente así.

**Lo que la limpieza NO podía hacer.** Cinco de los ocho pares tienen valores
**distintos**:

```
MBEDTLS_HEAP_SIZE               1024  ->  16384
MCUMGR_GRP_IMG_STATUS_HOOKS        n  ->  y
MCUMGR_GRP_IMG_UPLOAD_CHECK_HOOK   n  ->  y
MCUMGR_MGMT_NOTIFICATION_HOOKS     n  ->  y
NCS_SAMPLE_MCUMGR_BT_OTA_DFU       n  ->  y
```

Los cuatro `MCUMGR` están en `n` con un comentario que dice *"BLE OTA va con BLE"*, y un
bloque posterior los vuelve a poner en `y`. **El estado real es `y`.** Ordenar hacia la
intención declarada —quedarse con el `n`— habría cambiado la imagen y podría haber roto
los ganchos de progreso de DFU, que es el único camino para reflashear una unidad
sellada. Por eso se borra **la primera de cada par**, nunca la última.

**Qué cambia.** 30 líneas fuera: las 8 primeras de cada duplicado y las 22 `CONFIG_BT_*`
(que ni siquiera llegan al `.config` generado — Kconfig las descarta por dependencias no
satisfechas).

**Implicación.** Ninguna funcional, y eso está *probado*, no supuesto. Lo que sí cambia
es que ahora se puede activar una puerta de cero avisos en CI: los duplicados casi
seguro emitían "assigned more than once", y una puerta que pinta el repo en rojo el
primer día se desactiva y no vuelve.

**Cómo se verifica.** El gate para cualquier cambio que se afirme inerte:

```sh
diff <(grep ^CONFIG_ build-A/clip/zephyr/.config | sort) \
     <(grep ^CONFIG_ build-B/clip/zephyr/.config | sort)
cmp -l build-A/clip/zephyr/zephyr.bin build-B/clip/zephyr/zephyr.bin | wc -l
```

**Resultado. 🟢 Inerte, byte a byte.**

```
.config generado : IDENTICO
FLASH / RAM      : 927.324 B / 428.848 B en los dos
zephyr.bin       : 0 bytes distintos
```

Los `.signed.bin` sí difieren en hash, y eso es esperado: imgtool estampa su propia
cabecera. La imagen es la misma.

**Nota para el resto de la limpieza.** Los 9 símbolos `CLIP_*` muertos
(`CLIP_AGC_*` ×4, `CLIP_STORAGE_ENABLED`, `CLIP_TRANSFER_ENABLED`, `CLIP_UDP_*`)
**no** se han tocado todavía: quitarlos sí cambia el `.config` (desaparecen), aunque no
debería cambiar la imagen. Hay que pasarlos por el mismo gate antes de afirmarlo.
Y `CLIP_TRANSFER_ENABLED` no gobierna nada: `transfer.c` se compila incondicionalmente
en `CMakeLists.txt:21`, así que sacar el fichero es un cambio aparte que sí vale 2.864 B.

---

## L-012 · Fuera 1.014 ficheros que no entran en ninguna imagen
**pendiente de commit · 2026-09-10 · 🟢 verificado por construcción**

**Para qué.** El repositorio arrastraba 2.339 ficheros rastreados, de los cuales casi la
mitad pertenecían al aparato retirado. No es cuestión de estética: `mobile/` son 930
ficheros de SDKs BLE para un producto que ya no existe, y cada búsqueda en el árbol los
atraviesa.

**Qué cambia.**

| | ficheros | por qué |
|---|---|---|
| `mobile/` + sus 2 workflows | 930 | SDKs Flutter/Android/iOS por BLE; BLE se quitó por seguridad |
| `lib/lua/` + `samples/lua_repl/` | ~80 | `CONFIG_LUA` no lo activa ninguna imagen |
| `docs/requirements.md`, `custom_app_guide.md`, `development.md` | 3 | era Seeed, describen otro aparato |

Rastreados: **2.339 → 1.326**.

**Implicación.** Ninguna sobre la imagen, y está probado abajo. Lo que sí hubo que
arreglar son las referencias vivas que quedaban colgando: `skills/clip-dev/references/
mcuboot.md`, dos README de `samples/` y `applications/clip/tests/docs/testing.md`
apuntaban a `custom_app_guide.md`; ahora apuntan a la sección "Board and sysbuild" de
`CLAUDE.md`. Las notas de versión v0.0.6 y v0.0.9 **se dejan intactas**: son registro
histórico y describen con exactitud lo que existía entonces.

**Lo que casi se rompe.** Quitar `lib/lua` exigía tocar DOS sitios, y sólo encontré uno a
ojo: `lib/CMakeLists.txt` tiene `add_subdirectory_ifdef(CONFIG_LUA lua)` y `lib/Kconfig`
tiene `rsource "lua/Kconfig"`. El primero es condicional y habría sobrevivido; el segundo
**no**, y el árbol no configuraba:

```
lib/Kconfig:7: '.../lib/lua/Kconfig' not found (in 'rsource "lua/Kconfig"')
```

Lo cazó la construcción, no la lectura. Es el argumento entero a favor de pasar el gate
en vez de confiar en que un borrado "obviamente" no afecta.

**Resultado. 🟢 Inerte.**

```
.config generado : 0 lineas distintas
FLASH            : 927.324 B en los dos
zephyr.bin       : 9 bytes distintos -- y son los 9 del hash de commit embebido
                   v0.2.0-439235cbf0e6  vs  v0.2.0-c6d2a59ba640
```

Esos 9 bytes son el banner de versión siguiendo a HEAD, que es justo lo que `CLAUDE.md`
registra como la explicación de las "compilaciones no deterministas" de antes.

---

## L-013 · Nueve símbolos de Kconfig que no gobernaban nada
**pendiente de commit · 2026-09-10 · 🟢 verificado por construcción**

**Para qué.** Nueve `config CLIP_*` aparecían en el `.config` generado y ningún fuente los
leía. Un símbolo así no es inofensivo: parece una palanca, invita a girarla, y no está
conectada a nada. Cierra Tier 2, que es lo que permite activar una puerta de cero avisos
en CI.

**La decisión que había que tomar, y por qué se borra en vez de conectarse.** El backlog
dejaba abierta la opción de **cablear** el grupo `CLIP_AGC_*` a `audio.c` en vez de
borrarlo. Mirando el código, no procede:

```c
audio.c:1241   int32_t target_level = 6000;    /* AGC entero, hecho a mano */
Kconfig        CLIP_AGC_TARGET default 30000   /* AGC de SpeexDSP */
```

Cinco veces de diferencia. No son valores por defecto sin cablear esperando conexión: son
ajustes de un **AGC distinto** —el de SpeexDSP, que no está disponible en modo
`FIXED_POINT` y se sustituyó por un AGC entero propio con su propio estado
(`agc_hp_x1`, `agc_envelope`, `agc_gain_q8`). Cablearlos metería un objetivo 5× erróneo.
Decisión: se borran, y queda anotado para no volver a plantearlo.

**Qué cambia.** Fuera `CLIP_AGC_{TARGET,MAX_GAIN,INCREMENT,DECREMENT}`,
`CLIP_STORAGE_ENABLED`, `CLIP_STORAGE_SESSIONS_PER_PAGE`, `CLIP_TRANSFER_ENABLED`,
`CLIP_UDP_MAX_DATA_SIZE`, `CLIP_UDP_HEARTBEAT_INTERVAL_MS`. 27 líneas.

**Implicación.** `CLIP_TRANSFER_ENABLED` merece una nota aparte: **no gobernaba
`transfer.c`**, que se compila incondicionalmente en `CMakeLists.txt:21`. Quitar el
símbolo no saca el fichero. Sacar `transfer.c` del build es un cambio distinto y sí vale
2.864 B — queda pendiente.

**Resultado. 🟢 Inerte.**

```
.config : solo desaparecen los 9 simbolos, nada mas
FLASH   : 927.324 B en los dos
zephyr.bin : 11 bytes distintos, TODOS del banner de version
             v0.2.0-c6d2a59ba640 -> v0.2.0-98fb52da1f42
             bytes distintos fuera de esa zona: 0
```

---

## L-014 · Línea base antes de tocar la radio: qué hace la versión actual
**código de `ad59518`, imagen construida en `439235c` · 🟡 caracterizada, no arreglada**

**Para qué.** Antes de cambiar nada en `wifi.c` hay que dejar escrito qué hace la versión
que ya está, o el cambio siguiente no se podrá juzgar. Esta es la imagen que corrió las
noches 2 y 3 **sin reflashear entre medias** — mismo binario, dos noches, resultados
opuestos.

**El historial, con la versión de cada noche.**

| noche | imagen | grabación | audio capturado | entrega | qué pasó |
|---|---|---|---|---|---|
| 09-09→10 | `1780026` (sólo L-002) | 7 h 21 min | ✅ | ❌ parcial | radio muerta a las 04:50, **5 h 37 min mudo** |
| 09-10→11 | `ad59518` | 9 h 44 min | ✅ | ✅ completa | **39/39 asociaciones, 0 timeouts** |
| 09-11 | `ad59518` | 9 h 15 min | ✅ | ❌ parcial | **29 ventanas seguidas fallando, 7 h 33 min mudo, 72 ficheros parados** |

La lectura que importa: **la grabación no ha fallado nunca — 3 de 3 noches capturaron
audio.** Lo que falla es la entrega, en 2 de las 3. El audio está siempre en la tarjeta;
lo que se pierde es la visibilidad y el que llegue a tiempo.

Y la misma imagen da una noche limpia y una rota, así que **no es determinista**. Cualquier
arreglo tiene que evaluarse sobre varias noches, no sobre una.

**Lo que se sabe del fallo, con evidencia.**

- `prev_why=2 prev_miss=29 prev_stage=1 tdfail=0` — 29 ventanas seguidas sin asociar.
- **`tdfail=0` en las 29.** El apagado funcionó cada vez, así que `rpu_pwroff()` cortó
  BUCKEN e IOVDD **29 veces** y la asociación falló después de cada una. Un ciclo de
  alimentación real del chip **no limpia este estado**.
- Un reinicio del SoC sí lo limpia, en segundos. Dos veces observado.
- El estado es **del lado del host**: wpa_supplicant deja de contestar a su propio socket
  de control (`DISCONNECT`, `REMOVE_NETWORK`, `SET country` expiran a los 15-20 s), y ya
  está roto **antes** de que la ventana siguiente empiece.
- `no result event` — el driver nunca entrega el resultado de la asociación.
- Hay una **segunda firma distinta** en otras tandas: tormentas de `0xAAAAAAAA` (398-400)
  **sin** un solo timeout. Puede que sean dos fallos, no uno.

**Dos cosas nuestras que pueden estar causándolo, no sólo sufriéndolo.**

1. `wifi_sta_off()` manda `DISCONNECT` y **no espera**: sólo registra un aviso y duerme
   500 ms antes de `net_if_down()`. Pero en el fallo ese DISCONNECT tarda 15-20 s en
   expirar, así que tiramos la interfaz encima de un comando que el suplicante todavía
   está procesando. El comentario de esa misma función ya avisa de que eso *"took the
   whole device down every time"*, y 500 ms fue la mitigación para el caso rápido.
2. La ventana espera enlace **45 s**, el backoff del driver llega a **120 s**
   (`STA_RECONNECT_MAX_MS`) y se vio en **138 s**. Soltamos el préstamo a los 45 y la
   radio se apaga 45 s después: **90 s**. El reintento que el driver tenía programado no
   llega a ejecutarse **nunca**, ni una vez en toda la noche.

**Qué se probó a continuación:** L-015. De los tres candidatos que se plantearon, **dos
cayeron al mirar el log** — ver ahí.

---

## L-015 · Dejar que el reintento del driver llegue a ejecutarse
**pendiente de flashear · 2026-09-11 · 🔴 sin verificar, ni siquiera en banco**

**Para qué.** De las tres ideas "sin coste de audio" de L-014, **dos no sobrevivieron al
log**, y conviene dejar escrito por qué antes de que alguien las vuelva a proponer:

1. *"Esperar a que el DISCONNECT termine antes de `net_if_down()`."* **Descartada.**
   `STA disconnect failed` aparece **0 veces** en toda la noche: nuestro
   `net_mgmt(DISCONNECT)` nunca devolvió error. Y los timeouts de `DISCONNECT` y
   `REMOVE_NETWORK` caen a las 04:52:34 y 04:52:54, **mientras la ventana todavía
   espera** (pidió la radio a las 04:52:20, se rindió a las 04:53:05) — nuestro apagado
   no corre hasta 45 s después. Esos comandos son del **camino de reconexión del driver**,
   no nuestros. No estamos corrompiendo al suplicante: lo estamos viendo fallar.

2. *"No tirar la interfaz incondicionalmente en este estado."* **Descartada por riesgo.**
   Esa incondicionalidad **es** el arreglo de H2 (`688496b`), que llevó la puerta de
   asociación de 1/10 a 20/20. Quitarla sin evidencia de que estorbe aquí cambia un fallo
   conocido-arreglado por uno desconocido.

**Lo que sí quedó en pie.** El ciclo completo, de una de las 29 ventanas:

```
04:52:20  pedimos la radio
04:52:19  asociacion falla: "no result event"
04:52:54  el driver se programa un reintento en 138 s  -> vencia 04:55:12
04:53:05  nos rendimos a los 45 s y soltamos el prestamo
04:53:50  la radio se apaga                            <- mata el reintento
```

**El reintento del driver muere 82 segundos antes de vencer. Las 29 veces.** 45 s de
espera más 45 de gracia son 90; `STA_RECONNECT_MAX_MS` son 120 y se vio 138. Su
recuperación no llegó a intentarse **ni una sola vez en toda la noche**.

**Qué cambia.** La espera de enlace pasa a ser adaptativa:
`CLIP_WIFI_LINK_WAIT_S` (45, igual que antes) en condiciones normales, y
`CLIP_WIFI_LINK_WAIT_BAD_S` (150 > 120 del backoff) durante las primeras
`CLIP_WIFI_LINK_WAIT_RETRY_WINDOWS` (3) ventanas malas seguidas.

**Implicación.** Una ventana sana no cambia en nada. Una ventana mala mantiene la radio
~150 s en vez de 45. El tope de 3 no es cosmético: 29 ventanas a 150 s son **~35 mAh, la
quinta parte de la celda** — al driver se le da su oportunidad al principio, no toda la
noche.

**Coste medido.** 927.324 → 927.468 B, **+144 bytes**, 99,35% → 99,37%. Quedan 5.908
libres. Cero avisos.

**Cómo se verifica.** Que aparezca `ventana: espera larga (150 s)` en el log tras una
ventana mala, y sobre varias noches — L-014 deja claro que la misma imagen da una noche
limpia y una rota, así que **una noche buena no prueba nada**.

**Resultado.** Nada todavía: **no se ha llegado a flashear.** Al ir a instalarlo, el CDC
del aparato dejó de responder — el nodo `/dev/ttyACM0` existe y `nrfutil device list` lo
enumera, pero `open()` se queda bloqueado incluso con `exclusive=False`. Hizo falta
replug físico. Anotado por si vuelve a pasar: es la primera vez que se ve el CDC colgado
con el aparato aparentemente vivo.

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
