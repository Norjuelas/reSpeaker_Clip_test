/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_BATTERY_H
#define CLIP_BATTERY_H

#include <stdbool.h>

/**
 * @brief Initialize battery monitoring
 *
 * Sets up NPM1300 interrupt callbacks for charging events (VBUS, charge complete)
 * and starts periodic battery level polling (60s interval).
 * Updates BLE Battery Service with level and charging status.
 *
 * @return 0 on success, negative error code on failure
 */
int battery_init(void);

/**
 * @brief Poll battery status immediately
 *
 * Reads sensors, updates fuel gauge SoC, and refreshes display/BLE.
 * Call this when the user triggers a status bar display to get fresh readings.
 */
void battery_poll(void);

/**
 * @brief Persist the nRF Fuel Gauge state to settings (LittleFS)
 *
 * Saves the fuel gauge internal state so the SoC is continuous across reboots
 * (without this, every reboot re-estimates SoC from the resting voltage and
 * the displayed % jumps). Called on SoC change (infrequent) + on graceful
 * shutdown/reboot (a fresh copy before power-off).
 */
void battery_save_fg_state(void);

/**
 * @brief Whether VBUS (USB cable power) is present, per the PMIC
 *
 * Cached from the last NPM1300 read; kept fresh by the PMIC's own
 * VBUS_DETECTED/VBUS_REMOVED interrupts. Use this — not the nRF5340 USB
 * controller's VBUS messages, which report a phantom removal whenever the
 * WiFi radio powers on.
 *
 * @return true if a USB cable is powering the device
 */
bool battery_vbus_present(void);

/**
 * @brief Estado interno del gate de carga, para diagnosticar G7 por cable.
 *
 * Tres numeros que hasta ahora habia que deducir del log de la tarjeta — que
 * en produccion es la unica salida y se lee como una foto vieja mientras el
 * aparato la usa. Con esto, "no esta cargando" se resuelve en una consulta:
 *
 *   thermal_off  el latch termico por software tiene la carga apagada. Si es
 *                true, el aparato NO deberia estar cargando y todo va bien.
 *   idle_s       SEGUNDOS que lleva con VBUS puesto y el PMIC sin ver la
 *                celda; 0 si no lo esta. Antes era una cuenta de sondeos, y
 *                era una unidad enganosa: read_and_update() se llama desde
 *                seis sitios, asi que "sondeos" no equivale a minutos.
 *   rearms       ciclos apagado/encendido que se han tenido que hacer desde
 *                el arranque. Si crece, G7 sigue vivo aunque en el momento de
 *                preguntar el aparato este cargando.
 */
void battery_charge_gate_state(bool *thermal_off, uint32_t *idle_s,
			       uint32_t *rearms);

#endif /* CLIP_BATTERY_H */
