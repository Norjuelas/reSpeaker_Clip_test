# Entorno de compilacion NCS v3.3.0.  Uso:  source tools/ncs-env.sh
#
# zephyr-env.sh NO basta: no pone `west` en el PATH. west vive dentro del bundle
# de toolchain de Nordic, con su propio Python, y el fichero environment.json de
# ese bundle es la lista autoritativa de lo que hay que exportar.
#
# Si el hash del bundle cambia, sacalo de un build anterior en vez de buscarlo:
#     grep -i west build-*/clip/zephyr/CMakeCache.txt
#
# OJO al instalar firmware: el bundle define NRFUTIL_HOME apuntando a su propio
# nrfutil, que solo trae el comando `device`. `mcu-manager` -- el que instala
# firmware -- vive en el ~/.nrfutil del usuario. Por eso NRFUTIL_HOME se deja
# fuera a proposito: compilar no lo necesita, y ponerlo rompe el flasheo con
# "nrfutil command 'mcu-manager' not found" justo despues de un build correcto.

T="${CLIP_NCS_TOOLCHAIN:-$HOME/ncs/toolchains/911f4c5c26}"
if [ ! -d "$T" ]; then
    echo "tools/ncs-env.sh: no existe $T" >&2
    echo "  exporta CLIP_NCS_TOOLCHAIN con la ruta correcta del bundle" >&2
    return 1 2>/dev/null || exit 1
fi

export PATH="$T/bin:$T/usr/bin:$T/usr/local/bin:$T/opt/bin:$T/opt/nanopb/generator-bin:$T/nrfutil/bin:$T/opt/zephyr-sdk/arm-zephyr-eabi/bin:$T/opt/zephyr-sdk/riscv64-zephyr-elf/bin:$PATH"
export LD_LIBRARY_PATH="$T/lib:$T/lib/x86_64-linux-gnu:$T/usr/local/lib:$LD_LIBRARY_PATH"
export GIT_EXEC_PATH="$T/usr/local/libexec/git-core"
export GIT_TEMPLATE_DIR="$T/usr/local/share/git-core/templates"
export PYTHONHOME="$T/usr/local"
export PYTHONPATH="$T/usr/local/lib/python3.12:$T/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$T/opt/zephyr-sdk"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$HOME/ncs/v3.3.0/zephyr}"

# Kconfig descubre los modulos antes de que exista CMake, asi que esto tiene que
# ser variable de entorno y no -D.
export ZEPHYR_EXTRA_MODULES="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
