# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(clip)

# Include directories
target_include_directories(app PRIVATE include)

# Source files
target_sources(app PRIVATE
    src/main.c
    src/clip_event.c
    src/config.c
    src/transport.c
    src/at_server.c
    src/at_commands.c
    src/audio.c
    src/storage.c
    src/transfer.c
    src/haptic.c
    src/button.c
    src/display.c
    src/icons.c
    src/http_upload.c
    src/health.c
    src/upload_registry.c
    src/nrf70_fw_provision.c
    src/wifi.c
    src/battery.c
    src/usb_cdc.c
)


# Bluetooth: la implementacion real, o una superficie vacia. Sacar el host BT
# libera ~15KB de FLASH, que es lo que faltaba para que TLS entrara. Volver
# atras es CONFIG_BT=y — ningun sitio de llamada cambia. Ver Doc 14.
if(CONFIG_BT)
    target_sources(app PRIVATE src/ble.c src/transport_ble.c)
else()
    target_sources(app PRIVATE src/ble_stub.c)
endif()

# Transporte UDP: el real, o una superficie vacia. Se retira por seguridad — el
# canal AT por red no pide credenciales — y de paso libera FLASH. Volver atras
# es CLIP_UDP_TRANSPORT=y, sin tocar ningun sitio de llamada.
if(CONFIG_CLIP_UDP_TRANSPORT)
    target_sources(app PRIVATE src/wifi_udp.c src/transport_udp.c)
else()
    target_sources(app PRIVATE src/udp_stub.c)
endif()
