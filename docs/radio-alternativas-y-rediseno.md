# Radio: alternativas de diseño, y cómo se implementaría cada una

Documento de análisis, **2026-09-12**. No propone tocar nada hoy: recoge las ideas que
salieron al pensar el subsistema de radio desde cero, otras formas de conectar y de
entregar el audio, y cambios de método para que el fallo se vea y el código se pueda
gobernar. Cada idea lleva qué cambia, cómo se haría, qué cuesta, qué revela del diseño
actual y cómo se daría por buena.

Complementa `docs/estado-del-problema-radio.md` (el diagnóstico y el plan de pruebas) y
`docs/cuaderno-de-laboratorio.md`. Los hechos en que se apoya están en el diagnóstico:
el chip deja de servir el bus en momentos de RF activa, el fallo persiste porque el
apagado en tiempo de ejecución no es un apagado real, y todas las muertes registradas
ocurrieron con VBUS puesto y el PMIC sin ver la celda.

**Presupuestos que condicionan todo:** 5,9 KB de flash libres en la imagen de producto,
unos 22 KB de RAM estática, sin sonda SWD, una sola unidad de pruebas. Por eso varias
ideas se plantean como imagen de banco o como muestra aparte, no como cambio en el
producto.

---

## 1. El subsistema de radio diseñado desde cero

Un periférico que falla y que no debe molestar a la grabación se diseña con cinco piezas.
Ponerlas junto a lo que hace hoy el código muestra los defectos sin discutirlos.

| pieza | qué posee | qué hay hoy | qué cuesta hoy |
|---|---|---|---|
| Gestor de radio: un hilo, una cola de mensajes, una máquina de estados explícita | cada transición, con su plazo y su evidencia | `wifi_sta_on()` en `sta_work_q`, `wifi_sta_off()` en la cola del sistema, la cadena de reconexión por un tercer camino, semáforos entre medias | 150 encendidos por noche, 35 s de apagado fallido por intento, la cadena que sobrevive al préstamo |
| Secuenciador de alimentación | los pines, el tiempo mínimo apagado, la marca de tiempo de cada flanco | el driver desconecta los pines tras apagar y nadie los posee | un chip colgado nunca se apaga de verdad en tiempo de ejecución |
| Supervisor de enlace | una sonda que llega al chip en cada etapa, y la duración de cada llamada bloqueante | `rpu_answers()` pregunta al suplicante, que sin asociación no toca el chip; ninguna duración medida | `repowers` vale 0 por construcción; los encendidos de 15 s se encontraron a mano |
| Escalera de recuperación con presupuesto | desconectar, bajar y subir la interfaz, ciclo de alimentación con pines a bajo, reinicio del SoC; cada peldaño contado; cortacircuitos tras N muertes | un solo peldaño, el reinicio, prohibido grabando, disparado por "nada salió bien", que un servidor caído también produce | la noche muda entera, 35 mAh en intentos vanos, aparatos sanos reiniciados |
| Telemetría con relleno | un registro compacto por ventana, guardado sin red y enviado al volver; un registro de incidente en flash en la primera muerte | contadores en vivo y una nota de arranque | las horas mudas no se reconstruyen nunca desde el servidor |

### 1.1 Gestor de radio

**Qué cambia.** Un único hilo dueño de la radio. Todo lo demás le manda mensajes:
`RADIO_REQ_LINK(quien, segundos)`, `RADIO_REL_LINK(quien)`, `RADIO_REQ_CYCLE(hard)`,
`RADIO_REQ_OFF`. La cola del sistema, el despachador de eventos y el hilo de subida no
vuelven a llamar a `net_mgmt()` ni a `net_if_up()`.

**Estados y plazos.** Cada transición tiene un plazo y una evidencia. El plazo es lo que
hoy existe; la evidencia es lo que falta.

| estado | cómo se entra | plazo | evidencia de éxito | evidencia de "chip" |
|---|---|---|---|---|
| POWERING | `net_if_up()` | 10 s | `SUPPLICANT_READY` | `net_if_up` devuelve error |
| BOOTED | canario (§1.3) | 2 a 10 s según sonda | respuesta del chip | sin respuesta |
| SCANNING/ASSOC | `NET_REQUEST_WIFI_CONNECT` | 20 s | `CONNECT_RESULT` con estado 0 | duración anómala del canario posterior |
| IP | `net_dhcpv4_start` | 15 s | `IPV4_ADDR_ADD` | no aplica |
| SERVING | préstamos vivos | hasta el último `REL_LINK` más la gracia | | |
| TEARDOWN | `DISCONNECT`, 500 ms, `net_if_down()` | 5 s más 12 s | `net_if_down` en menos de 2 s | `net_if_down` de 10 s o más |
| RECOVERING | fallo con evidencia "chip" | según peldaño | canario responde | agota presupuesto |

**Cómo se implementaría.** Un fichero nuevo `radio_mgr.c` con `K_MSGQ_DEFINE` de 8
mensajes, un hilo de 6 KB que sustituye a `sta_work_q` (misma pila, mismo prioridad
mínima), y `k_work_delayable` internos para la gracia y el reintento. `wifi.c` queda
como capa fina de llamadas a `net_mgmt` sin estado propio. `http_upload.c` pide
`RADIO_REQ_LINK("upload", 180)` y espera un `k_sem` con el resultado tipado (§1.6). El
vencimiento del préstamo deja de ser un trabajo en la cola del sistema: es un mensaje al
gestor. El reintento tras fallo es política del gestor, acotada por la petición viva: sin
préstamo no hay reintento, que es lo que hoy no se cumple.

**Coste.** Sobre todo mover código: unas 600 líneas de `wifi.c` cambian de sitio. Neto
estimado de 1 a 2 KB de flash por la máquina de estados y los contadores. Se puede meter
tras `CONFIG_CLIP_RADIO_MGR` para comparar imágenes con el mismo guion de banco.

**Qué revela.** Que hoy hay tres hilos con derecho a tocar la radio y ninguno con
obligación de saber qué están haciendo los otros dos.

**Aceptación.** En una ventana que falla, exactamente una secuencia de intentos y menos
de diez líneas de log. `assert` de estado en cada entrada: nunca dos transiciones en
vuelo. El gate de asociación de `tests/hil/assoc_gate.py` da lo mismo que hoy, 20 de 20.

### 1.2 Secuenciador de alimentación

**Qué cambia.** Tras cada `net_if_down()` los dos pines de alimentación del nRF7002 se
conducen a bajo y se mantienen así hasta el siguiente encendido. Hoy el driver los deja
como `GPIO_DISCONNECTED`, y el DTS no les pone pull.

**Cómo se implementaría.** Dos `gpio_dt_spec` tomados del mismo nodo que usa el driver:

```c
static const struct gpio_dt_spec bucken = GPIO_DT_SPEC_GET(DT_NODELABEL(nrf70), bucken_gpios);
static const struct gpio_dt_spec iovdd  = GPIO_DT_SPEC_GET(DT_NODELABEL(nrf70), iovdd_ctrl_gpios);
/* justo despues de que net_if_down() devuelva */
gpio_pin_configure_dt(&iovdd,  GPIO_OUTPUT_INACTIVE);
gpio_pin_configure_dt(&bucken, GPIO_OUTPUT_INACTIVE);
```

El driver los reconfigura como salidas en `rpu_init()` al siguiente `net_if_up()`, así
que no hace falta devolverlos. En recuperación (§1.4) se espera un mínimo, del orden de
1 s, con los pines a bajo antes de volver a subir. Cada flanco se marca con
`k_uptime_get()` y las marcas van a la telemetría. Alternativa equivalente: parche en
`rpu_gpio_remove()` del árbol de NCS para que deje `GPIO_OUTPUT_INACTIVE`, con el mismo
flujo que `patches/mcuboot/`.

**Coste.** Decenas de líneas, flash despreciable. Conducir a bajo no consume.

**Qué revela.** Que la diferencia entre "ciclo de radio" y "reinicio del SoC" era que el
arranque sí conduce los pines a bajo, durante dos minutos.

**Aceptación.** Con el multímetro: 0 V en BUCKEN, IOVDD_CTRL y en el raíl IOVDD entre
ventanas. En banco, con el cable puesto y el chip muerto en una ventana, la ventana
siguiente asocia.

### 1.3 Supervisor de enlace y canario

**Qué cambia.** Una pregunta que tiene que ir y volver del chip, en cada etapa, con plazo
propio. Hoy la primera señal de chip muerto es un plazo de asociación de 20 s, y la sonda
existente no llega al chip.

**Cómo se implementaría.** Hay tres sondas posibles con el API que ya está compilado:

- `NET_REQUEST_WIFI_REG_DOMAIN` con `oper = WIFI_MGMT_GET`. Llega a
  `nrf_wifi_fmac_get_reg()`, que manda un mandato al chip y espera su evento hasta 10 s.
  Devuelve error si no contesta. Coste cero de flash; plazo interno largo, pero es
  evidencia directa.
- `NET_REQUEST_STATS_GET_WIFI`. Llega a `nrf_wifi_sys_fmac_stats_get()`, con espera
  acotada por `NRF_WIFI_FMAC_STATS_RECV_TIMEOUT`. Requiere `CONFIG_NET_STATISTICS_WIFI`,
  que cuesta flash; medir con `rom_report` antes.
- La duración de `net_if_down()`. Diez segundos o más significa que `chg_vif_state`
  agotó su plazo: el chip no contestó. Coste cero y sirve como evidencia en el apagado.

La sonda corre tras `SUPPLICANT_READY` y justo antes de `TEARDOWN`. Su resultado y las
duraciones de encendido, país, conexión, DHCP y apagado se guardan por ventana.

**Qué revela.** Que `CLIP_WIFI_RPU_PROBE` nunca ha preguntado nada al chip, porque
`supplicant_status()` sólo lo consulta cuando hay asociación.

**Aceptación.** En el log del banco, una muerte de chip se clasifica en menos de 12 s y
con la etiqueta correcta, no tras 20 s con "no result event".

### 1.4 Escalera de recuperación con presupuesto

**Qué cambia.** Cuatro peldaños en vez de uno, y un cortacircuitos.

| peldaño | acción | presupuesto | cuándo se sube al siguiente |
|---|---|---|---|
| 1 | `DISCONNECT`, `net_if_down`, `net_if_up` | 1 por ventana | canario sin respuesta |
| 2 | ciclo de alimentación con pines a bajo 1 s (§1.2) | 2 por ventana | canario sin respuesta tras el ciclo |
| 3 | cortacircuitos: no se enciende la radio durante K ventanas | K configurable, 2 por defecto | sólo se baja de peldaño si el canario responde |
| 4 | reinicio del SoC | sólo sin grabar y con el cortacircuitos abierto M veces | último recurso |

**Cómo se implementaría.** Un estado RECOVERING del gestor con un contador por peldaño,
publicado en el latido como `rec_soft`, `rec_hard`, `rec_breaker`, `rec_reboot`. La
condición de entrada es evidencia de chip (§1.3), nunca "la ventana no logró nada": un
endpoint inalcanzable produce etapa 2 y no toca la escalera.

**Un coste que hay que medir.** Cada encendido cuesta unos 400 ms de audio, dos timeouts
del DMIC, en 36 de las 39 ventanas del arranque limpio. Una escalera que cicla la radio
grabando multiplica esos huecos. La aceptación debe contar `DMIC timeout` por ventana
además de asociaciones.

**Qué revela.** Que el reinicio funciona por accidente: es la única ruta que mantiene los
pines a bajo, y el detector lo dispara con la señal equivocada.

### 1.5 Telemetría con relleno y registro de incidente

**Qué cambia.** Un registro por ventana que sobrevive a la falta de red y un registro de
la primera muerte que sobrevive al reinicio.

**Cómo se implementaría.** Un `struct` de 16 bytes: uptime en segundos (4), clase (1),
etapa (1), intentos (1), banderas VBUS y celda detectada (1), milivoltios (2), duración
del encendido en centésimas (2), duración del apagado en centésimas (2), reservado (2). Un
anillo de 32 en RAM, 512 B. El latido añade `"wins":"<hex>"` con los no enviados, ocho
como máximo por latido (256 caracteres); se marcan enviados al recibir 200. El buffer del
latido va hoy en 660 de 960 B: o se sube a 1.280 o se limita a cuatro registros por
latido. En `bpin-fleet-service`, una tabla `windows` decodificada desde `raw`.

El registro de incidente reutiliza el mecanismo de `clip_boot_note` en `config.c`: en la
primera muerte de chip del arranque se escribe una vez en settings uptime, VBUS, celda
detectada, milivoltios, índice de ventana, clase y las cuatro duraciones. Una escritura
por arranque, sin desgaste.

**Qué revela.** Que hoy la noche muda es un hueco en la base de datos, y que la única
memoria persistente es la nota de arranque.

**Aceptación.** Tras una noche mala, la tabla `windows` del servidor reproduce la tabla
que hoy se saca a mano del log de la tarjeta.

### 1.6 Planificador y gestor separados, resultado tipado

Hoy `periodic_work_fn` pide el préstamo, espera, sube, juzga y castiga. Separado, el
planificador pide un enlace y recibe un resultado con tipo:

```c
enum radio_result { RADIO_OK, RADIO_CHIP_DEAD, RADIO_NO_AP, RADIO_NO_IP,
                    RADIO_NO_ROUTE, RADIO_ENDPOINT_FAIL };
```

El detector razona sobre el tipo, no sobre un booleano. `RADIO_CHIP_DEAD` sube por la
escalera; `RADIO_NO_AP` sólo se cuenta y se publica; `RADIO_ENDPOINT_FAIL` no hace nada
con la radio. Es lo que habría evitado el reinicio de un aparato sano al final de T-02.

### 1.7 El suplicante como caja negra

Cuando un mandato de control expira, el canal queda desincronizado para la vida de esa
interfaz, porque el protocolo no empareja peticiones con respuestas. La reacción correcta
no es mandar el siguiente mandato, es recrear la interfaz: `net_if_down()` y `net_if_up()`
hacen que hostap la quite y la vuelva a añadir. Implementación: el gestor cuenta
`ctrl_timeouts` por interfaz y ante el primero pasa a peldaño 1 de la escalera. Hoy se
encadenan tres mandatos de 15 s cada uno sobre el mismo canal muerto.

---

## 2. Otras formas de conectar y de entregar

### 2.1 La base de carga como cosechadora USB

**Qué cambia.** El camino nocturno de entrega deja de usar la radio. Un aparato con VBUS
no graba, por veto propio del firmware, así que en la base está ocioso y su tarjeta está
libre. Un host pequeño en la base monta las tarjetas por USB MSC, copia las sesiones, las
sube con una pila TLS de verdad y marca lo subido. La radio queda para latidos de día y
subidas pequeñas.

**Cómo se implementaría.**

- *Hardware:* una Raspberry Pi 4 o 5 y hubs USB alimentados para 20 puertos. 20 aparatos
  por 200 MB por noche caben en USB 2.0 con mucho margen.
- *Host:* un guion en `applications/clip/tests/tools/`, hermano de `panel_admin.py`. Por
  cada disco nuevo: montar **sólo lectura** con `udisksctl mount -o ro`, leer
  `/REC/YYYYMMDD/HH/MM/SS/` y `session.json`, subir con el mismo cliente que ya habla con
  `/upload`, identificar el aparato por `AT+DEVICE` en su puerto CDC.
- *Marcar lo subido sin dos escritores en la FAT.* El host no debe escribir en la tarjeta:
  el log de la tarjeta sigue escribiendo en `LOG/` mientras hay USB. Dos opciones: un
  mandato AT nuevo, `AT+UPLOADED=<sesion>,<indice>`, que llame a `upload_registry_mark()`
  y escriba la línea `sesion:indice` en `UPLOADED.TXT` desde el aparato; o el
  `AT+DELETE=<sesion>` existente tras confirmar la subida, que es destructivo pero
  coherente con que el libro de subidas falle abierto.
- *Cierre de sesiones:* garantizado por el veto de grabar con VBUS. Leer `REC/` en sólo
  lectura mientras el aparato escribe `LOG/` es seguro.

**Coste.** Un guion de unos cientos de líneas en el host, un mandato AT de veinte líneas
en el aparato, y hardware de base que de todas formas se iba a construir.

**Qué revela.** Que el camino nocturno nunca necesitó la radio, y que la única condición
que correlaciona con todas las muertes del chip, VBUS presente, es justo la condición
en que la base podría no usarla.

**Aceptación.** Una noche con dos aparatos en una base de prueba: 100 % de los ficheros
en el servidor por la mañana, `UPLOADED.TXT` coherente, cero ventanas de radio abiertas
con VBUS.

### 2.2 Política de radio por estado de alimentación

**Qué cambia.** La ventana decide cuánto hace según el cable, hasta que el cable quede
explicado. En batería, como hoy. Con VBUS y la celda sin detectar, sólo latido, sin
drenar. Con VBUS y la celda detectada, lo que diga la prueba 2 del diagnóstico.

**Cómo se implementaría.** En `periodic_work_fn`, antes de pedir el préstamo, leer
`battery_vbus_present()` y el estado del cargador que ya cachea `battery.c`, y elegir
modo. Un símbolo `CLIP_RADIO_ON_VBUS` con tres valores. Unas 30 líneas.

**Qué revela.** Que hoy la ventana no sabe si hay cable, y que la base de 20 aparatos
sería, sin esta política, el peor caso posible por construcción.

### 2.3 Siempre asociado con ahorro de energía, re-probado en batería

**Qué cambia.** Nada del producto todavía: se reabre una decisión. `NRF_WIFI_LOW_POWER=n`
salió de un A/B en banco, sobre USB, que hoy es la condición sospechosa. Si el ahorro de
energía funciona en batería, un enlace permanente en bajo consumo elimina de la noche
todos los encendidos, todos los escaneos y todos los ciclos de alimentación, y deja
disponible la recuperación propia del driver, `NRF_WIFI_RPU_RECOVERY`.

**Cómo se implementaría.** Imagen de banco: `CONFIG_NRF_WIFI_LOW_POWER=y` (arrastra
`RPU_RECOVERY`), `CONFIG_CLIP_WIFI_ON_DEMAND=n` o una gracia muy larga. Una noche en
batería, desenchufado desde antes de arrancar. El consumo se mide por la pendiente del
SoC entre latidos, nunca con `battery_ua`, que no ve la radio.

**Aceptación.** 20 de 20 asociaciones y un latido cada 5 minutos toda la noche. Después,
comparar mAh por noche con los 21 mA medios de la radio por ventana.

### 2.4 Conexión dirigida

**Qué cambia.** Se conecta al BSSID y canal de la última asociación buena y sólo se
escanean las dos bandas si eso falla. Menos segundos de RF por ventana, y un dato de
diagnóstico: si el chip muere igual en la primera trama, muere al arrancar la RF, no por
el escaneo.

**Cómo se implementaría.** Tras `STA up`, leer `NET_REQUEST_WIFI_IFACE_STATUS`, que
devuelve `bssid` y `channel`, y guardarlos en settings. En `wifi_sta_on()` rellenar
`req.bssid`, `req.channel` y `req.band = WIFI_FREQ_BAND_2_4_GHZ` cuando hay caché;
borrar la caché tras un fallo. Unas 60 líneas.

### 2.5 Escaneo pasivo y potencia mínima como experimentos

**Qué cambia.** Dos imágenes de banco para preguntar si el disparador es la corriente de
transmisión al arrancar la RF, sin osciloscopio.

**Cómo se implementaría.** Potencia: un overlay de DTS que baje
`wifi-max-tx-pwr-2g-dsss`, `-mcs0` y `-mcs7` de 9 a 0. Escaneo pasivo: hostap fija
`scan_ssid 1` en el pegamento, así que la conexión siempre sondea; la aproximación es un
escaneo explícito previo con `NET_REQUEST_WIFI_SCAN` y `scan_type = WIFI_SCAN_TYPE_PASSIVE`
seguido de conexión dirigida (§2.4), que reduce las tramas a un canal.

**Qué se aprende.** Si a 0 dBm no hay muertes en las condiciones de la prueba del cable,
el margen de alimentación es la pista principal.

### 2.6 Quitar trabajo del momento de conectar

Dos cargas que hoy caen justo cuando arranca la RF:

- **El dominio regulatorio.** `SET country` cuesta 2 s de espera al chip y un modo de
  fallo de 15 s en cada encendido. El driver tiene `CONFIG_NRF70_REG_DOMAIN`, hoy en
  `"00"`: fijarlo a `"CO"` y llamar a `wifi_set_reg_domain()` una vez por arranque, o
  nunca. Coste cero.
- **PBKDF2 en cada conexión.** La frase de paso se convierte en PMK cada vez que se
  configura la red, unos segundos de CPU al 100 % a 64 MHz. Con la clave ya derivada, el
  pegamento de hostap la rechaza: exige de 8 a 63 caracteres y la manda entre comillas.
  Hacen falta tres líneas de parche en `wpas_add_and_config_network()` para emitir un PSK
  de 64 hexadecimales sin comillas cuando `psk_length == 64`, y derivar la clave en el
  ordenador al aprovisionar (`wpa_passphrase`) o una vez en el aparato si se compila
  `MBEDTLS_PKCS5_C`, que hoy no está.

### 2.7 Una sesión TLS por ventana

**Qué cambia.** Hoy `http_upload.c` abre un socket por fichero: 116 apretones de manos
en las sesiones que precedieron a las muertes. Una conexión persistente por ventana los
reduce a uno.

**Cómo se implementaría.** Abrir el socket al empezar la ventana y reutilizarlo con
`http_client_req()` fichero a fichero, con `Connection: keep-alive` y reconexión si el
servidor cierra. El servicio tiene que mantener la conexión; comprobarlo en la
configuración del receptor de `bpin-fleet-service`. Los tamaños de mbedTLS ya
configurados, 4 KB de entrada y salida, bastan.

**Coste y ganancia.** Pocas líneas; ahorra 1,1 s y un apretón de manos por fichero, y
quita presión de heap justo cuando el latido compite por él.

### 2.8 Coordinar el PMIC con la radio

El paliativo de G7 en `battery.c` apaga y encienda el cargador cada 5 minutos mientras el
PMIC no ve la celda. Sea o no el disparador, una ventana de radio y un ciclo del cargador
no deberían solaparse. Implementación: `battery.c` consulta `wifi_lease_count()` y aplaza
el ciclo si es mayor que cero. Diez líneas.

---

## 3. Ver el problema y gobernar el código

### 3.1 Un tercer testigo en el aire

T-02 añadió el registro del AP como segundo testigo. El tercero es el aire. En el
portátil, con una tarjeta que soporte modo monitor:

```sh
sudo iw dev wlan0 interface add mon0 type monitor
sudo ip link set mon0 up
sudo iw dev mon0 set channel <canal del AP>
sudo tcpdump -i mon0 -w clip-aire.pcap 'wlan addr2 b2:2b:20:63:ea:e0 or wlan addr1 b2:2b:20:63:ea:e0'
```

Lectura: una ventana muerta sin *probe requests* en el aire es el chip; sondas sin
respuesta del AP es el AP; tramas de datos fluyendo mientras el aparato registra
bloqueos es el host. Guardar el `.pcap` junto a los logs de la tarjeta de esa noche.

### 3.2 Medir cada llamada bloqueante

Encendido, país, conexión, DHCP, desconexión, bajada de interfaz. Máximos por ventana al
latido como `t_up`, `t_cty`, `t_assoc`, `t_dhcp`, `t_down`. Umbrales que ya se conocen:
`t_down` de 10 000 ms o más es chip sin respuesta; `t_cty` de 15 000 ms es encendido
muerto. Implementación: `k_uptime_get()` antes y después de cada llamada en el gestor,
seis enteros por ventana.

### 3.3 Autoclasificación de cada ventana mala

La clase se decide por evidencia, no por "nada salió bien":

| evidencia | clase |
|---|---|
| canario sin respuesta, `t_down` de 10 s o más, `Scan trigger failed` | `CHIP_DEAD` |
| conexión sin resultado con canario respondiendo y apagado rápido | `NO_AP` |
| asociado sin IP | `NO_IP` |
| IP sin ruta o `connect` con -116 | `NO_ROUTE` o `ENDPOINT_FAIL` |

La clase va al latido como `win_class` y al registro de ventana (§1.5). Los datos de campo
se etiquetan solos, y el detector actúa por clase.

### 3.4 Banco de caos

Reproducir el disparador a voluntad y después usarlo como prueba de regresión:

- **VBUS programado:** un hub controlable con `uhubctl -l <hub> -p <puerto> -a off`, o un
  relé USB, para conectar y quitar el cable en horario, con el aparato ocioso y ventanas
  cada pocos minutos.
- **Barrido de VBAT:** una fuente programable en lugar de la celda, de 4,2 V a 3,0 V en
  pasos de 0,2 V, N ventanas por paso. Cuantifica el margen en vez de intuirlo.
- **El último mandato antes del silencio:** una imagen de banco con
  `CONFIG_NRF_WIFI_CMD_EVENT_LOG=y` y `WIFI_NRF70_LOG_LEVEL_INF`, que registra cada
  mandato y cada evento del chip. No cabe en la imagen de producto; sí en la muestra de
  §3.6.

### 3.5 Dos aparatos, la misma noche

El experimento controlado más barato que existe y el único que separa unidad mala de
diseño malo: mismo binario, mismo sitio, una unidad con cable y otra sin él, y a la
mañana siguiente las dos tarjetas por la herramienta de §3.8.

### 3.6 Una muestra "laboratorio de radio"

La imagen de producto tiene 6 KB libres; los experimentos no caben en ella. El repo ya
dice que los prototipos van a `samples/`. Una muestra `samples/radio_lab/` con las mismas
placa y particiones, sin audio ni almacenamiento: WiFi, suplicante, shell por CDC y TLS
opcional. Mandatos de shell: `radio on`, `radio off [hard]`, `radio connect [bssid canal]`,
`radio window <n>` (asocia, empuja N bytes, apaga, imprime las seis duraciones),
`radio band 2g|any`, `radio hold <ms>`. La potencia TX y el escaneo pasivo entran por
overlays de DTS y `prj.conf` de la muestra. Un ajuste se prueba en una tarde y sólo el
ganador se porta al producto.

### 3.7 Ajustes en tiempo de ejecución en vez de reconstrucciones

Cada experimento ha costado un directorio de compilación; hay dieciocho. Con el
subsistema `settings` ya presente: `radio/interval_min`, `radio/link_wait_s`,
`radio/grace_s`, `radio/hold_low_ms`, `radio/band`, `radio/wedge_windows`, leídos al
arrancar y ajustables con `AT+RADIOCFG=<clave>,<valor>` y consultables con `AT+RADIOCFG?`.
Unas 150 líneas y cerca de 1 KB de flash; si no cabe, sólo los tres primeros.

### 3.8 La herramienta de análisis de logs

La tabla por arranque de este diagnóstico se hizo con un `awk` en un directorio temporal.
Como `applications/clip/tests/tools/radio_boots.py`: segmenta por `Boot: last reset`,
cuenta ventanas, `STA up`, fallos, `RPU is unresponsive`, tormentas, timeouts de control,
SSID en uso, estado de VBUS y celda, duraciones de encendido y asociación, y clasifica
cada ventana con las reglas de §3.3. Salida en tabla Markdown para pegar en el cuaderno.
Cada noche produce la misma tabla, y las comparaciones dejan de depender de quién lee.

### 3.9 Carriles de imagen

Dos nombres y una regla: `build-lab-*` para imágenes de banco, que pueden llevar
símbolos de depuración, intervalos cortos y potencias distintas; `build-field` para lo
que sale a tienda. Una imagen de campo nunca lleva un símbolo de banco, y el cuaderno
anota siempre de qué carril salió el binario de cada noche.

---

## 4. Decisiones cerradas que conviene reabrir

| decisión | por qué se cerró | por qué reabrirla |
|---|---|---|
| `NRF_WIFI_LOW_POWER=n` | 0 de 12 asociaciones con ahorro activo | el A/B corrió sobre USB, la condición sospechosa; una noche en batería (§2.3) |
| `NRF70_QSPI_LOW_POWER=y` | el gate con `n` dio 1 de 10 | ese gate corrió con la tarjeta montada en el portátil; nunca se repitió limpio |
| gracia de 45 s e intervalo de 15 min | dimensionados para batería | interactúan con la escalera de recuperación y con el coste de audio por encendido |
| `CLIP_WIFI_RPU_PROBE` | `repowers` siempre 0 | no porque H2 no aparezca, sino porque la sonda no llega al chip; sustituir por el canario de §1.3 antes de borrarla |

---

## 5. Para la siguiente revisión de hardware

Preguntas y cambios para quien tenga el esquema:

- Pull-down en BUCKEN e IOVDD_CTRL, para que el chip no dependa de que el host los
  conduzca para estar apagado.
- Alimentación propia del nRF7002, un regulador desde VSYS con capacidad de reserva para
  los flancos de RF, o al menos medir VBAT en su pin durante una subida con la celda a
  3,4 V y con el cable puesto.
- Un punto de medida de corriente en su raíl. Hoy el medidor de combustible es ciego a la
  radio por diseño.
- Verificar el NTC: el nPM1300 decide si hay batería por esa vía, y en esta unidad VBUS
  siempre viene con "celda no detectada" (`err 0x11` o `0x40`, `status 0x00`).
- Puntos de prueba en VBAT del chip, IOVDD, BUCKEN e IOVDD_CTRL.
- Una pista ya anotada en `usb_cdc.c`: el controlador USB ve caer VBUS cada vez que se
  enciende la radio. Es una perturbación de alimentación observada; el hardware debería
  poder explicarla.

---

## 6. Priorización

| idea | valor diagnóstico | valor de producto | coste | riesgo | depende de |
|---|---|---|---|---|---|
| §3.8 herramienta de logs | alto | medio | bajo | ninguno | nada |
| §3.1 testigo en el aire | alto | bajo | bajo | ninguno | una tarjeta en modo monitor |
| §1.3 canario y §3.2 duraciones | alto | alto | bajo | bajo | flash medido |
| §1.2 secuenciador de alimentación | alto | alto | bajo | bajo | prueba del cable |
| §1.4 escalera con presupuesto | medio | alto | medio | medio | §1.2 y §1.3 |
| §1.5 telemetría con relleno | alto | alto | medio | bajo | cambio en el servidor |
| §2.1 base como cosechadora USB | bajo | muy alto | medio | bajo | decisión de producto |
| §2.2 política por alimentación | medio | alto | bajo | bajo | prueba del cable |
| §1.1 gestor de radio | medio | alto | alto | medio | tiempo de banco |
| §2.3 ahorro de energía en batería | alto | alto si funciona | bajo | medio | una noche |
| §2.6 y §2.7 menos trabajo al conectar | medio | medio | bajo | bajo | parche pequeño en hostap |
| §3.6 muestra de laboratorio | alto | medio | medio | ninguno | nada |
| §3.4 banco de caos | alto | medio | medio | ninguno | hub controlable o relé |

El orden razonable: primero lo que mide sin cambiar el producto (§3.8, §3.1, §3.5, §3.4),
después lo que convierte una noche muda en una ventana mala (§1.2, §1.3, §1.4), después lo
que reduce la exposición (§2.2, §2.6, §2.7), y en paralelo la decisión de producto que
puede sacar la radio del camino nocturno (§2.1).
