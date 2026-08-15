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

Dos imágenes, y **no son intercambiables**:

```sh
export R=$(pwd)

# Producción con TLS y sin Bluetooth — la que va a tienda
west build --build-dir build-tls --board clip/nrf5340/cpuapp applications/clip \
  -- -DSNIPPET_ROOT=$R/applications/clip -DSNIPPET=production \
     -DEXTRA_CONF_FILE=$R/applications/clip/overlay-tls.conf

# Desarrollo: HTTP en claro, con Bluetooth. Para depurar
west build --build-dir build-prov --board clip/nrf5340/cpuapp applications/clip \
  -- -DSNIPPET_ROOT=$R/applications/clip -DSNIPPET=production
```

`ZEPHYR_EXTRA_MODULES` tiene que ser variable de entorno, no de CMake: Kconfig
descubre los módulos antes de que CMake exista.

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
