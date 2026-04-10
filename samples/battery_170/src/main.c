/*
 * Battery Model with NPM1300 Fuel Gauge for ReSpeaker Clip
 * Using Nordic nRF Fuel Gauge Library
 *
 * Battery: HSZ 362123
 *   - Chemistry: Li-Polymer
 *   - Nominal Voltage: 3.7V
 *   - Capacity: 170mAh (0.629Wh)
 *   - Charge Voltage: 4.2V
 *   - Discharge Cut-off: 3.0V
 *
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/logging/log.h>
#include <nrf_fuel_gauge.h>

LOG_MODULE_REGISTER(battery_170, LOG_LEVEL_INF);

#define SLEEP_TIME_MS 1000

/* NPM1300 devices */
static const struct device *charger_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));

/* Charger status bitmasks */
#define CHG_STATUS_COMPLETE_MASK BIT(1)
#define CHG_STATUS_TRICKLE_MASK  BIT(2)
#define CHG_STATUS_CC_MASK       BIT(3)
#define CHG_STATUS_CV_MASK       BIT(4)

static int64_t ref_time;
static volatile bool vbus_connected;

/* Battery model - using Nordic's preset model */
static const struct battery_model battery_model = {
#include "battery_model.inc"
};

static int read_sensors(const struct device *charger, float *voltage, float *current, float *temp,
			int32_t *chg_status)
{
	struct sensor_value value;
	int ret;

	ret = sensor_sample_fetch(charger);
	if (ret < 0) {
		LOG_ERR("sensor_sample_fetch failed: %d", ret);
		return ret;
	}

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
	*voltage = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &value);
	*temp = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
	*current = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &value);
	*chg_status = value.val1;

	/* Debug: Raw sensor readings */
	LOG_DBG("RAW: V=%.3fV, I=%.3fmA, T=%.1fC, CHG=0x%02x",
		*voltage, *current * 1000, *temp, (unsigned int)*chg_status);

	return 0;
}

static int charge_status_inform(int32_t chg_status)
{
	union nrf_fuel_gauge_ext_state_info_data state_info;

	/* Debug: print raw charger status */
	LOG_DBG("Raw charger status: 0x%02x", (unsigned int)chg_status);
	LOG_DBG("  COMPLETE: %d, TRICKLE: %d, CC: %d, CV: %d",
		(chg_status & CHG_STATUS_COMPLETE_MASK) != 0,
		(chg_status & CHG_STATUS_TRICKLE_MASK) != 0,
		(chg_status & CHG_STATUS_CC_MASK) != 0,
		(chg_status & CHG_STATUS_CV_MASK) != 0);

	if (chg_status & CHG_STATUS_COMPLETE_MASK) {
		LOG_DBG("→ Charge COMPLETE");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg_status & CHG_STATUS_TRICKLE_MASK) {
		LOG_DBG("→ Trickle charging");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg_status & CHG_STATUS_CC_MASK) {
		LOG_DBG("→ Constant current charging");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
	} else if (chg_status & CHG_STATUS_CV_MASK) {
		LOG_DBG("→ Constant voltage charging");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
		LOG_DBG("→ Charger idle");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
	}

	return nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE,
					       &state_info);
}

static bool poll_vbus_status(void)
{
	struct sensor_value val;
	int ret = sensor_channel_get(charger_dev, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS, &val);
	if (ret < 0) {
		return false;
	}
	return val.val1 != 0;
}

/* PMIC interrupt not supported on this board */
#define HAS_PMIC_INT 0

/* VBUS debounce settings */
#define VBUS_DEBOUNCE_COUNT 3  /* Require 3 consecutive same readings */
static uint8_t vbus_debounce_cnt = 0;
static bool vbus_stable_state = false;

/* Debounced VBUS status with hysteresis */
static bool get_vbus_status_debounced(bool vbus_raw)
{
	if (vbus_raw == vbus_stable_state) {
		/* Same state as before, increment counter */
		if (vbus_debounce_cnt < VBUS_DEBOUNCE_COUNT) {
			vbus_debounce_cnt++;
		}
	} else {
		/* State changed, reset counter */
		vbus_debounce_cnt = 0;
	}

	/* Only update stable state after debounce threshold */
	if (vbus_debounce_cnt >= VBUS_DEBOUNCE_COUNT) {
		if (vbus_stable_state != vbus_raw) {
			vbus_stable_state = vbus_raw;
			LOG_INF("VBUS state changed: %s", vbus_stable_state ? "CONNECTED" : "DISCONNECTED");
		}
	}

	return vbus_stable_state;
}

static void print_battery_status(float voltage, float current_ma, float temp,
				  float soc, float tte, float ttf, int32_t chg_status)
{
	bool is_trickle = (chg_status & CHG_STATUS_TRICKLE_MASK) != 0;
	bool is_cc = (chg_status & CHG_STATUS_CC_MASK) != 0;
	bool is_cv = (chg_status & CHG_STATUS_CV_MASK) != 0;
	bool charger_complete = (chg_status & CHG_STATUS_COMPLETE_MASK) != 0;
	bool is_charging = is_trickle || is_cc || is_cv;
	bool charger_connected = is_charging || charger_complete;

	/* Battery is considered fully charged only when SoC >= 99% */
	bool battery_full = (soc >= 99.0f);

	/* Determine current direction for verification */
	const char *current_dir = (current_ma > 5) ? "Charging +" :
				  (current_ma < -5) ? "Discharging -" : "Idle ~";

	LOG_INF("==============================================");
	LOG_INF("  HSZ 362123 Battery Monitor (Nordic Fuel Gauge)");
	LOG_INF("==============================================");
	LOG_INF("Hardware Readings:");
	LOG_INF("  Voltage:     %d mV", (int)(voltage * 1000));
	LOG_INF("  Current:     %d mA (%s)", (int)current_ma, current_dir);
	LOG_INF("  Temperature: %d C", (int)temp);
	LOG_INF("");
	LOG_INF("Charger Status (Reg: 0x%02x):", (unsigned int)chg_status);
	LOG_INF("  Connected:   %s", charger_connected ? "YES" : "NO");
	if (charger_connected && !battery_full) {
		LOG_INF("  Charging:    YES (%s)",
			is_trickle ? "Trickle" : is_cc ? "CC" : "CV");
	} else if (charger_connected && battery_full) {
		LOG_INF("  Charging:    NO (Battery Full)");
	} else {
		LOG_INF("  Charging:    NO (Idle)");
	}
	LOG_INF("");
	LOG_INF("State of Charge (SoC):");
	LOG_INF("  SoC (Nordic FG):  %.1f%%", soc);
	LOG_INF("  Battery Full:     %s", battery_full ? "YES" : "NO");
	LOG_INF("");
	LOG_INF("Time Estimates:");
	/* Use charger status to determine valid time estimates */
	if (charger_connected && !battery_full) {
		/* Charging - show TTF */
		if (ttf >= 0) {
			LOG_INF("  Time to Full:  %.0f min (%.1f hours)", ttf, ttf / 60.0);
		} else {
			LOG_INF("  Time to Full:  Calculating...");
		}
		LOG_INF("  Time to Empty: N/A (Charging)");
	} else if (!charger_connected && !battery_full) {
		/* Discharging - show TTE */
		if (tte >= 0) {
			LOG_INF("  Time to Empty: %.0f min (%.1f hours)", tte, tte / 60.0);
		} else {
			LOG_INF("  Time to Empty: Calculating...");
		}
		LOG_INF("  Time to Full:  N/A (Discharging)");
	} else {
		/* Battery full */
		LOG_INF("  Time to Empty: N/A (Battery Full)");
		LOG_INF("  Time to Full:  N/A (Battery Full)");
	}
	LOG_INF("==============================================");
}

int main(void)
{
	struct sensor_value value;
	struct nrf_fuel_gauge_init_parameters init_params = {
		.model = &battery_model,
		.opt_params = NULL,
		.state = NULL,
	};
	float max_charge_current;
	float term_charge_current;
	int32_t chg_status;
	int ret;

	if (!device_is_ready(charger_dev)) {
		LOG_ERR("Charger device not ready");
		return -ENODEV;
	}

	LOG_INF("==============================================");
	LOG_INF("  HSZ 362123 Battery Model Initialization");
	LOG_INF("==============================================");
	LOG_INF("Battery: HSZ 362123");
	LOG_INF("Capacity: 170mAh (0.629Wh)");
	LOG_INF("Voltage: 3.7V (nominal)");
	LOG_INF("");
	LOG_INF("nRF Fuel Gauge version: %s", nrf_fuel_gauge_version);
	LOG_INF("");

	/* Read initial sensor values */
	ret = read_sensors(charger_dev, &init_params.v0, &init_params.i0, &init_params.t0, &chg_status);
	if (ret < 0) {
		LOG_ERR("Failed to read sensors");
		return ret;
	}

	/* Print initial readings for debugging */
	LOG_INF("Initial sensor readings:");
	LOG_INF("  Voltage: %.3f V (%d mV)", init_params.v0, (int)(init_params.v0 * 1000));
	LOG_INF("  Current: %.3f A (%d mA)", init_params.i0, (int)(init_params.i0 * 1000));
	LOG_INF("  Temperature: %.1f C", init_params.t0);
	LOG_INF("  Charger status: 0x%02x", (unsigned int)chg_status);

	/* Convert current: Zephyr uses negative=discharging, Nordic lib uses negative=charging */
	init_params.i0 = -init_params.i0;

	LOG_INF("After conversion for Nordic FG:");
	LOG_INF("  Current: %.3f A (%d mA)", init_params.i0, (int)(init_params.i0 * 1000));

	/* Get charge current limits */
	sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
	max_charge_current = (float)value.val1 + ((float)value.val2 / 1000000);
	term_charge_current = max_charge_current / 10.f;

	/* Initialize fuel gauge */
	ret = nrf_fuel_gauge_init(&init_params, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize fuel gauge: %d", ret);
		return ret;
	}

	/* Configure charge current limits */
	ret = nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
					      &(union nrf_fuel_gauge_ext_state_info_data){
						      .charge_current_limit = max_charge_current});
	if (ret < 0) {
		LOG_WRN("Could not set charge current limit: %d", ret);
	}

	ret = nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
					      &(union nrf_fuel_gauge_ext_state_info_data){
						      .charge_term_current = term_charge_current});
	if (ret < 0) {
		LOG_WRN("Could not set term current: %d", ret);
	}

	/* Set initial charge status */
	ret = charge_status_inform(chg_status);
	if (ret < 0) {
		LOG_WRN("Could not set charge status: %d", ret);
	}

	ref_time = k_uptime_get();

#if HAS_PMIC_INT
	if (setup_vbus_callback() != 0) {
		LOG_WRN("Failed to add PMIC callback");
	}
	LOG_INF("VBUS interrupt enabled");
#else
	LOG_INF("No PMIC interrupt pin, using VBUS polling");
#endif

	vbus_connected = poll_vbus_status();
	LOG_INF("VBUS status: %s", vbus_connected ? "connected" : "disconnected");
	LOG_INF("");
	LOG_INF("Starting battery monitoring...");
	LOG_INF("");

	/* Main loop */
	while (1) {
#if !HAS_PMIC_INT
		/* Get raw VBUS status and apply debouncing */
		bool vbus_raw = poll_vbus_status();
		vbus_connected = get_vbus_status_debounced(vbus_raw);
		LOG_DBG("VBUS raw: %s, stable: %s", vbus_raw ? "YES" : "NO",
			vbus_connected ? "YES" : "NO");
#endif

		/* Update VBUS state */
		ret = nrf_fuel_gauge_ext_state_update(
			vbus_connected ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
				       : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
			NULL);
		if (ret < 0) {
			LOG_WRN("Could not update VBUS state: %d", ret);
		}

		/* Read sensors */
		float voltage, current, temp;
		ret = read_sensors(charger_dev, &voltage, &current, &temp, &chg_status);
		if (ret < 0) {
			LOG_WRN("Failed to read sensors: %d", ret);
			k_sleep(K_MSEC(SLEEP_TIME_MS));
			continue;
		}

		/* Debug: log raw sensor readings */
		LOG_DBG("Sensors: V=%.3fV, I=%.3fmA, T=%.1fC", voltage, current * 1000, temp);

		/* Convert current for Nordic lib */
		float current_nordic = -current;

		/* Update charge status if changed */
		static int32_t chg_status_prev;
		if (chg_status != chg_status_prev) {
			chg_status_prev = chg_status;
			LOG_INF("Charger status changed: 0x%02x → 0x%02x", chg_status_prev, chg_status);
			charge_status_inform(chg_status);
		}

		/* Calculate time delta */
		float delta = (float)k_uptime_delta(&ref_time) / 1000.f;

		/* Process fuel gauge */
		float soc = nrf_fuel_gauge_process(voltage, current_nordic, temp, delta, NULL);
		float tte = nrf_fuel_gauge_tte_get();
		float ttf = nrf_fuel_gauge_ttf_get();

		/* Debug: print raw TTE/TTF values */
		LOG_DBG("FG State: SoC=%.1f%%, TTE=%.1f min, TTF=%.1f min, delta=%.2f s",
			soc, tte, ttf, delta);

		/* Print status */
		print_battery_status(voltage, current * 1000, temp, soc, tte, ttf, chg_status);

		k_sleep(K_MSEC(SLEEP_TIME_MS));
	}

	return 0;
}
