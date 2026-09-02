# Activa el entorno de compilacion. Uso:  source env.sh
#
# Equivalente en Linux de `nrfutil sdk-manager toolchain launch` en el Mac.
# El bundle de toolchain trae su propio python y su propio west, y ambos
# necesitan las librerias del bundle: sin LD_LIBRARY_PATH, west muere con
# "libpython3.12.so.1.0: cannot open shared object file".
#
# Verificado 2026-09-01: west 1.5.0, Python 3.12.4, CMake 4.2.1, Ninja 1.13.2,
# arm-zephyr-eabi-gcc 12.2.0 (Zephyr SDK 0.17.0) -- las mismas versiones que
# registro el Mac, salvo el hash del bundle, que es por SO.

NCS_VERSION="${NCS_VERSION:-v3.3.0}"
NCS_ROOT="${NCS_ROOT:-$HOME/ncs}"
NCS_TOOLCHAIN="${NCS_TOOLCHAIN:-$NCS_ROOT/toolchains/911f4c5c26}"

if [ ! -d "$NCS_TOOLCHAIN" ]; then
    echo "env.sh: no existe $NCS_TOOLCHAIN" >&2
    echo "  toolchains disponibles:" >&2
    ls "$NCS_ROOT/toolchains" 2>/dev/null | sed 's/^/    /' >&2
    return 1 2>/dev/null || exit 1
fi

export PATH="$NCS_TOOLCHAIN/usr/local/bin:$NCS_TOOLCHAIN/bin:$PATH"
export LD_LIBRARY_PATH="$NCS_TOOLCHAIN/usr/local/lib:$NCS_TOOLCHAIN/lib:$LD_LIBRARY_PATH"
export ZEPHYR_SDK_INSTALL_DIR="$NCS_TOOLCHAIN/opt/zephyr-sdk"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

. "$NCS_ROOT/$NCS_VERSION/zephyr/zephyr-env.sh"

# Variable de ENTORNO, no de CMake: Kconfig descubre los modulos antes de que
# CMake exista. Con -D no funciona y el sintoma es "No board named 'clip'".
export ZEPHYR_EXTRA_MODULES="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

echo "entorno listo:"
echo "  west         $(west --version 2>/dev/null | sed 's/West version: //')"
echo "  gcc          $("$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" --version 2>/dev/null | head -1)"
echo "  ZEPHYR_BASE  $ZEPHYR_BASE"
echo "  modulo       $ZEPHYR_EXTRA_MODULES"
