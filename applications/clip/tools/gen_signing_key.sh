#!/usr/bin/env bash
# Genera la clave de firma de MCUboot propia (P-256) para la flota B-Pin.
#
#   ./applications/clip/tools/gen_signing_key.sh ~/bpin-secrets
#
# ⚠ ESTE GUION NO CAMBIA LA CONFIGURACION DEL PROYECTO, Y ES A PROPOSITO.
#
# Cambiar la clave de firma exige reinstalar MCUboot, y MCUboot solo se
# instala por SWD (merged.hex). Si se cambia la clave y se sigue actualizando
# por recuperacion serie, las imagenes nuevas quedan firmadas con una clave que
# el MCUboot YA INSTALADO no reconoce: el device las rechaza y se queda sin via
# de actualizacion. Sin sonda de depuracion, eso es una unidad perdida — y si
# se hace en lote, la flota entera.
#
# El orden correcto:
#   1. Generar la clave con este guion y guardarla fuera de linea.
#   2. Conseguir sonda SWD.
#   3. Cambiar SB_CONFIG_BOOT_SIGNATURE_KEY_FILE en
#      boards/seeed/clip/Kconfig.sysbuild a la ruta de la clave nueva.
#   4. Probar el ciclo completo en UNA unidad de sacrificio: flashear
#      merged.hex por SWD, y comprobar que la actualizacion por recuperacion
#      serie sigue funcionando.
#   5. Solo entonces, el resto de la flota.
set -euo pipefail

DEST="${1:?uso: gen_signing_key.sh <directorio-de-destino>}"
mkdir -p "$DEST"
KEY="$DEST/bpin-mcuboot-signing-$(date +%Y%m%d).pem"

[ -e "$KEY" ] && { echo "ya existe $KEY — no se sobrescribe"; exit 1; }

command -v imgtool >/dev/null 2>&1 || {
  echo "falta imgtool (viene con NCS: pip install imgtool)"; exit 1; }

# P-256 y no RSA: la decision de flota es EC pura. Ademas quita RSA de la
# imagen de aplicacion, que son 22,8KB de FLASH medidos.
imgtool keygen -k "$KEY" -t ecdsa-p256
chmod 600 "$KEY"

echo
echo "Clave creada: $KEY"
echo
echo "  Huella publica (esto SI se puede compartir y conviene apuntar):"
imgtool getpub -k "$KEY" 2>/dev/null | head -20
echo
echo "SIGUIENTE PASO, Y NO ES OPCIONAL:"
echo "  Saca este fichero de este equipo y de cualquier copia automatica."
echo "  Quien lo tenga puede firmar firmware que los 100 devices aceptaran."
echo "  Si se pierde, no se puede volver a actualizar la flota."
