# Puesta en marcha — Clip / B·Pin

Todo lo necesario para pasar de un repositorio recién clonado a un device
grabando y enviando audio. Escrito para que alguien que no ha visto el proyecto
pueda repetirlo.

> ¿Términos que no reconoces? El [glosario](../docs-respeaker/17-glosario.md)
> explica los acrónimos de red, cifrado y hardware que aparecen aquí.

---

## 1. Herramientas

```sh
# nRF Connect SDK v3.3.0 — el compilador y las librerías
nrfutil install sdk-manager
nrfutil sdk-manager toolchain install --ncs-version v3.3.0

# Entorno de Python para las pruebas y el panel
cd applications/clip/tests
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
```

`nrfutil mcu-manager` es lo que instala firmware por cable. Viene con nrfutil.

---

## 2. Compilar

**La postura segura es la que sale sin argumentos extra.** No hay que pasar ningún
overlay para tener TLS: `CONFIG_CLIP_UPLOAD_TLS=y` vive en `prj.conf`, y Bluetooth
y el modo punto de acceso están apagados ahí mismo.

**La ruta del repo no puede llevar metacaracteres de shell** — ni `&`, ni espacios, ni
`(` `)` `;` en ningún directorio padre. CMake escribe las rutas sin comillas en las reglas
de ninja y `/bin/sh` las vuelve a partir. Con el repo en `.../mic&pose/code/...` el build
moría en `keygen.py` con código 127 y el mensaje útil quedaba escondido: `/bin/sh: 1:
pose/code/...: not found`. Un enlace simbólico **no** lo arregla — CMake resuelve algunos
objetivos a la ruta real y falla más tarde, en `app_version.h`. Hay que renombrar.

```sh
export ZEPHYR_EXTRA_MODULES=$(pwd)   # variable de entorno, NO de CMake: Kconfig
                                     # descubre los módulos antes de que CMake exista
export R=$(pwd)

# Producción — la que va a tienda. Consola apagada, ~170 µA en reposo
west build --build-dir build-prod --board clip/nrf5340/cpuapp applications/clip \
  -- -DSNIPPET_ROOT=$R/applications/clip -DSNIPPET=production

# Depuración — misma postura de seguridad, con consola y log a la microSD
west build --build-dir build-clip --board clip/nrf5340/cpuapp applications/clip
```

Las dos llevan TLS, mTLS y cifrado en reposo. La única diferencia es la consola:
para medir corriente hay que usar la de producción, porque la UART de depuración
se lleva ~570 µA en reposo y tapa todo lo demás.

Usa `--pristine` tras cualquier cambio de Kconfig, devicetree, sysbuild,
particiones o placa. Y si el directorio de build viene de otra máquina, **bórralo**:
`--pristine` no limpia las rutas absolutas muertas de `CMakeCache.txt`, y el
síntoma es el desconcertante `No board named 'clip' found. Did you mean: clip`.

### La imagen de banco, que NO es desplegable

```sh
# Devuelve Bluetooth y el modo AP, y APAGA TLS. Solo banco de pruebas
west build --build-dir build-dev --board clip/nrf5340/cpuapp applications/clip \
  -- -DEXTRA_CONF_FILE=$R/applications/clip/overlay-dev-radio.conf
```

TLS (~38 KB) y BLE (~15 KB) no caben juntos en el slot de 936 KB. Una imagen con
radio sube el audio **sin cifrar** y abre dos canales sin autenticar: marca el
device que la reciba y reflashéalo antes de devolverlo al inventario.

> **Corrección (2026-08-25).** Este apartado prescribía
> `-DEXTRA_CONF_FILE=…/overlay-tls.conf` para la imagen «con TLS» y omitirlo para
> la de «desarrollo sin TLS». Ese fichero **no contenía ni una línea de
> configuración** —sólo un comentario declarándose obsoleto desde el 2026-08-18—,
> así que las dos órdenes producían firmware idéntico mientras el texto afirmaba
> que se diferenciaban justo en TLS. El overlay se ha borrado.

---

## 3. Instalar en el device

El device tiene que estar **en modo recovery**. Se entra con `AT+DFU` si está
arrancado, o con el botón si no responde.

```sh
nrfutil mcu-manager serial image-upload \
  --firmware build-tls/clip/zephyr/zephyr.signed.bin \
  --serial-port /dev/tty.usbmodem1101
nrfutil mcu-manager serial reset --serial-port /dev/tty.usbmodem1101
```

> El número del puerto cambia según el USB que uses (`usbmodem101`,
> `usbmodem1101`…). `ls /dev/tty.usbmodem*` lo resuelve.

---

## 4. El servicio y el panel

```sh
cd applications/clip/tests/tools
../.venv/bin/python bpin_http_receiver.py --port 8080 --out ./recordings
```

| Ruta | Qué es |
|---|---|
| `http://localhost:8080/` | panel de devices y transferencias |
| `/config` | configuración por cable, firmware y logs |
| `/devices.json` | el estado en crudo |

Con TLS, añade certificado y clave:

```sh
../.venv/bin/python bpin_http_receiver.py --port 8443 --out ./recordings \
  --cert ../../../../utils-scripts/keys/bpin-fleet-ca-test/srv.pem \
  --key  ../../../../utils-scripts/keys/bpin-fleet-ca-test/srv.key
```

**El certificado va atado a la IP** por `subjectAltName`. Si el Mac cambia de
red, hay que reemitirlo o la validación falla — con un error que no menciona la
IP por ninguna parte:

```sh
cd utils-scripts/keys/bpin-fleet-ca-test
openssl req -new -key srv.key -out /tmp/s.csr -subj "/CN=<IP>"
printf "subjectAltName=IP:<IP>\nextendedKeyUsage=serverAuth\n" > /tmp/e.cnf
openssl x509 -req -in /tmp/s.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -out srv.pem -days 825 -sha256 -extfile /tmp/e.cnf
```

Y **tienen que ser de curva elíptica P-256**, no RSA: el device sólo negocia
ECDHE-ECDSA, y con RSA el handshake falla sin decir por qué.

---

## 5. Configurar el device

Por cable, una vez, antes de entregarlo:

```
AT+STACFG="<red>","<clave>"      guardar la red WiFi
AT+UPCFG="<ip>",<puerto>         a qué servicio enviar
AT+STA=on                        conectar ahora
AT+DEVICE                        ver firmware, MAC y chip id
AT+HEALTH?                       el estado completo
```

O desde el panel, en `/config`, que hace lo mismo con un formulario.

Para TLS, la CA va en la tarjeta como `/SD:/ca.pem` — se copia por USB cuando
el device monta como disco.

---

## 6. Comprobar que funciona

```
AT+START            empezar a grabar
AT+STOP             parar
AT+LIST             ver las grabaciones
AT+HTTPUP=<sesión>  enviarla
AT+HTTPUP?          cómo va
```

El device sube solo cada 15 minutos lo que no esté ya enviado, así que
`AT+HTTPUP` es sólo para forzarlo.

El audio llega como `.opus` **sin envoltorio Ogg** — ningún reproductor lo
abre. El receptor le pone el envoltorio al recibirlo y deja un `.ogg` al lado.

---

## 7. Lo que hay que saber antes de tocarlo

Cosas que costaron tiempo averiguar y no son evidentes:

**La FLASH está al 94%.** Cualquier función nueva compite por lo que queda.
Antes de añadir una librería, mira cuánto ocupa.

**Las pilas de los hilos están justas.** Este firmware se ha caído dos veces por
una pila mal dimensionada. Cualquier buffer de más de ~1 KB va al heap.
`AT+HTTPUP?` informa de lo que le sobra al hilo de subida: úsalo.

**El log de la microSD es la única ventana** a lo que pasa dentro cuando el
device deja de responder. El panel lo trae con un botón.

**La MAC cambia en cada arranque** (`CONFIG_WIFI_RANDOM_MAC_ADDRESS`). Para
redes con lista blanca esto es bloqueante y está sin resolver.

**El canal AT por UDP responde en la red sin autenticación alguna**, y acepta
`AT+FACTORY` y `AT+FORMAT`. Cualquiera en la misma red puede borrar un device.
Pendiente de cerrar.
