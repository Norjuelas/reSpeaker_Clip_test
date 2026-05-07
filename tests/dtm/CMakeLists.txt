cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dtm)

set(DTM_SRC ${ZEPHYR_NRF_MODULE_DIR}/samples/bluetooth/direct_test_mode)

target_include_directories(app PRIVATE ${DTM_SRC}/src)

target_sources_ifndef(CONFIG_DTM_TRANSPORT_HCI app PRIVATE
  ${DTM_SRC}/src/transport/dtm_uart_twowire.c
)

target_sources_ifdef(CONFIG_DTM_TRANSPORT_HCI app PRIVATE
  ${DTM_SRC}/src/transport/dtm_hci.c
)

if(CONFIG_DTM_TRANSPORT_HCI AND "${BOARD}" STREQUAL "nrf5340dk")
  target_sources(app PRIVATE ${DTM_SRC}/src/transport/hci_uart_remote.c)
  target_include_directories(app PRIVATE ${DTM_SRC}/rpc)
else()
  target_sources_ifdef(CONFIG_DTM_TRANSPORT_HCI app PRIVATE
    ${DTM_SRC}/src/transport/hci_uart.c
  )
endif()

target_sources(app PRIVATE
  ${DTM_SRC}/src/dtm.c
  ${DTM_SRC}/src/dtm_hw.c
  ${DTM_SRC}/src/main.c
)
