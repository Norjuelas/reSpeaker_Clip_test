/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/logging/log.h>
#include <nrf_fuel_gauge.h>

#include "battery.h"
#include "clip.h"
#include "clip_event.h"
#include "display.h"
#include "ble.h"
#include "transfer.h"
#include "haptic.h"

LOG_MODULE_REGISTER(battery, CONFIG_CLIP_LOG_LEVEL);

/* Charger status bitmasks (BCHGCHARGESTATUS register) */
#define CHG_STATUS_COMPLETE_MASK BIT(1)
#define CHG_STATUS_TRICKLE_MASK  BIT(2)
#define CHG_STATUS_CC_MASK       BIT(3)
#define CHG_STATUS_CV_MASK       BIT(4)

/* Battery full threshold (SoC %) */
#define BATTERY_FULL_THRESHOLD  99

/* Battery model - using Nordic's preset model */
static const struct battery_model battery_model = {
#include "battery_model.inc"
};

/* Device references */
static const struct device *pmic_dev;
static const struct device *charger_dev;

/* Cached state */
static uint8_t last_percent;
static bool last_charging;
static bool low_battery_warned;
static int64_t fg_ref_time;

/* Fuel gauge state */
static bool fg_initialized;

/* 60-second periodic battery level polling */
static struct k_work_delayable battery_level_work;

static int read_sensors(float *voltage, float *current, float *temp, int32_t *chg_status)
{
	struct sensor_value val;
	int ret;

	ret = sensor_sample_fetch(charger_dev);
	if (ret < 0) {
		LOG_WRN("Battery sensor sample fetch failed: %d", ret);
		return ret;
	}

	sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
	*voltage = (float)val.val1 + ((float)val.val2 / 1000000);

	sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_TEMP, &val);
	*temp = (float)val.val1 + ((float)val.val2 / 1000000);

	sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_AVG_CURRENT, &val);
	*current = (float)val.val1 + ((float)val.val2 / 1000000);

	sensor_channel_get(charger_dev, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &val);
	*chg_status = val.val1;

	return 0;
}

static int charge_status_inform(int32_t chg_status)
{
	union nrf_fuel_gauge_ext_state_info_data state_info;

	if (chg_status & CHG_STATUS_COMPLETE_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg_status & CHG_STATUS_TRICKLE_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg_status & CHG_STATUS_CC_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
	} else if (chg_status & CHG_STATUS_CV_MASK) {
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
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

static void read_and_update(void)
{
	struct clip_context *ctx = clip_get_context();
	float voltage, current, temp;
	int32_t chg_status;
	int ret;
	uint8_t percent;
	bool charging;
	bool charger_connected;
	bool vbus_connected;
	bool is_trickle, is_cc, is_cv, charger_complete;

	if (!device_is_ready(charger_dev)) {
		return;
	}

	/* Read sensors */
	ret = read_sensors(&voltage, &current, &temp, &chg_status);
	if (ret < 0) {
		return;
	}

	/* Get VBUS status */
	vbus_connected = poll_vbus_status();

	/* Parse charger status bits */
	is_trickle = (chg_status & CHG_STATUS_TRICKLE_MASK) != 0;
	is_cc = (chg_status & CHG_STATUS_CC_MASK) != 0;
	is_cv = (chg_status & CHG_STATUS_CV_MASK) != 0;
	charger_complete = (chg_status & CHG_STATUS_COMPLETE_MASK) != 0;

	/* Determine charger connected status */
	charger_connected = vbus_connected && (is_trickle || is_cc || is_cv || charger_complete);

	/* Update VBUS state in fuel gauge */
	if (fg_initialized) {
		ret = nrf_fuel_gauge_ext_state_update(
			vbus_connected ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
				       : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
			NULL);
		if (ret < 0) {
			LOG_WRN("Could not update VBUS state: %d", ret);
		}

		/* Update charge status if changed */
		static int32_t chg_status_prev;
		if (chg_status != chg_status_prev) {
			chg_status_prev = chg_status;
			charge_status_inform(chg_status);
		}

		/* Convert current for Nordic lib (negative=charging) */
		float current_nordic = -current;

		/* Calculate time delta */
		float delta = (float)k_uptime_delta(&fg_ref_time) / 1000.f;

		/* Process fuel gauge to get SoC */
		float soc = nrf_fuel_gauge_process(voltage, current_nordic, temp, delta, NULL);
		percent = (uint8_t)soc;

		/* Determine charging status for display/BLE:
		 * - Charging if VBUS connected AND (trickle/CC/CV active OR not yet full)
		 * - Not charging only if VBUS disconnected OR (charger_complete AND soc >= 99%)
		 */
		bool battery_full = (percent >= BATTERY_FULL_THRESHOLD);
		charging = charger_connected && (!charger_complete || !battery_full);

		/* Debug log for charging state */
		if (charger_connected && !last_charging) {
			LOG_DBG("Charger: VBUS=%d, trickle=%d, CC=%d, CV=%d, complete=%d, SoC=%u%%, full=%d",
				vbus_connected, is_trickle, is_cc, is_cv, charger_complete, percent, battery_full);
		}
	} else {
		/* Fallback: use simple voltage-based SoC before fuel gauge init */
		if (voltage >= 4.15f) {
			percent = 100;
		} else if (voltage <= 3.3f) {
			percent = 0;
		} else {
			/* Linear approximation */
			percent = (uint8_t)((voltage - 3.3f) / (4.15f - 3.3f) * 100.0f);
		}
		bool battery_full = (percent >= BATTERY_FULL_THRESHOLD);
		charging = charger_connected && (!charger_complete || !battery_full);
	}

	/* Update battery percent */
	if (percent != last_percent) {
		last_percent = percent;
		bt_bas_set_battery_level(percent);
		ctx->status.battery_percent = percent;
		LOG_INF("Battery: %u%% (%u mV)", percent, (uint32_t)(voltage * 1000));

		/* Low battery warning */
		if (!charging) {
			if (percent <= 15 && !low_battery_warned) {
				display_post_error("Low Battery");
				haptic_play_pattern(HAPTIC_SHORT);
				low_battery_warned = true;
			}
		}
	}

	/* Update charging status */
	if (charging != last_charging) {
		last_charging = charging;
		ctx->status.battery_charging = charging;

		if (charging) {
			low_battery_warned = false;
			bt_bas_bls_set_battery_charge_state(
				BT_BAS_BLS_CHARGE_STATE_CHARGING);

			/* Set charge type */
			if (is_trickle) {
				bt_bas_bls_set_battery_charge_type(
					BT_BAS_BLS_CHARGE_TYPE_TRICKLE);
			} else if (is_cv) {
				bt_bas_bls_set_battery_charge_type(
					BT_BAS_BLS_CHARGE_TYPE_CONSTANT_VOLTAGE);
			} else if (is_cc) {
				bt_bas_bls_set_battery_charge_type(
					BT_BAS_BLS_CHARGE_TYPE_CONSTANT_CURRENT);
			} else {
				/* Charger connected but in trickle/termination phase */
				bt_bas_bls_set_battery_charge_type(
					BT_BAS_BLS_CHARGE_TYPE_TRICKLE);
			}

			LOG_INF("Charging: type=%s",
				is_trickle ? "trickle" : is_cv ? "CV" : is_cc ? "CC" : "trickle");
		} else {
			bt_bas_bls_set_battery_charge_state(
				BT_BAS_BLS_CHARGE_STATE_DISCHARGING_ACTIVE);
			LOG_INF("Discharging");
		}
	}

	/* Update display with current status */
	struct display_status ds = {
		.battery_percent = last_percent,
		.battery_charging = last_charging,
		.ble_connected = ble_is_connected(),
		.transferring = transfer_is_active(),
	};
	display_update_status(&ds);
}

/* NPM1300 event callback — called from system work queue context */
static struct gpio_callback pmic_cb;

static void pmic_event_callback(const struct device *dev, struct gpio_callback *cb,
				uint32_t pins)
{
	if (pins & BIT(NPM13XX_EVENT_VBUS_DETECTED)) {
		LOG_INF("PMIC event: VBUS detected");
		clip_post_event(CLIP_EVENT_USB_CONNECTED);
	}
	if (pins & BIT(NPM13XX_EVENT_VBUS_REMOVED)) {
		LOG_INF("PMIC event: VBUS removed");
	}
	if (pins & BIT(NPM13XX_EVENT_CHG_COMPLETED)) {
		LOG_DBG("PMIC event: Charge completed (charger flag set)");
	}
	if (pins & BIT(NPM13XX_EVENT_CHG_ERROR)) {
		LOG_INF("PMIC event: Charge error");
	}

	/* Read and update battery status on any event */
	read_and_update();
}

/* 60-second periodic battery level polling */
static void battery_level_handler(struct k_work *work)
{
	read_and_update();
	k_work_schedule(&battery_level_work, K_SECONDS(60));
}

int battery_init(void)
{
	int ret;
	struct nrf_fuel_gauge_init_parameters init_params = {
		.model = &battery_model,
		.opt_params = NULL,
		.state = NULL,
	};
	float max_charge_current;
	float term_charge_current;
	int32_t chg_status;
	struct sensor_value value;

	pmic_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300));
	charger_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));

	if (!device_is_ready(charger_dev)) {
		LOG_WRN("NPM1300 charger not ready");
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
	ret = read_sensors(&init_params.v0, &init_params.i0, &init_params.t0, &chg_status);
	if (ret < 0) {
		LOG_WRN("Failed to read sensors for fuel gauge init: %d", ret);
		/* Continue with basic battery monitoring */
	} else {
		/* Print initial readings */
		LOG_INF("Initial sensor readings:");
		LOG_INF("  Voltage: %.3f V (%d mV)", init_params.v0, (int)(init_params.v0 * 1000));
		LOG_INF("  Current: %.3f A (%d mA)", init_params.i0, (int)(init_params.i0 * 1000));
		LOG_INF("  Temperature: %.1f C", init_params.t0);
		LOG_INF("  Charger status: 0x%02x", (unsigned int)chg_status);

		/* Convert current: Zephyr uses negative=discharging, Nordic lib uses negative=charging */
		init_params.i0 = -init_params.i0;

		/* Get charge current limits */
		sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
		max_charge_current = (float)value.val1 + ((float)value.val2 / 1000000);
		term_charge_current = max_charge_current / 10.f;

		/* Initialize fuel gauge */
		ret = nrf_fuel_gauge_init(&init_params, NULL);
		if (ret < 0) {
			LOG_WRN("Failed to initialize fuel gauge: %d, using voltage-based SoC", ret);
		} else {
			fg_initialized = true;

			/* Configure charge current limits */
			nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
						      &(union nrf_fuel_gauge_ext_state_info_data){
							      .charge_current_limit = max_charge_current});
			nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
						      &(union nrf_fuel_gauge_ext_state_info_data){
							      .charge_term_current = term_charge_current});

			/* Set initial charge status */
			charge_status_inform(chg_status);

			/* Initialize VBUS state */
			bool vbus_connected = poll_vbus_status();
			nrf_fuel_gauge_ext_state_update(
				vbus_connected ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
					       : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
				NULL);

			fg_ref_time = k_uptime_get();
		}
	}

	/* Register PMIC event callbacks for charging-related events */
	gpio_init_callback(&pmic_cb, pmic_event_callback,
			   BIT(NPM13XX_EVENT_VBUS_DETECTED) |
			   BIT(NPM13XX_EVENT_VBUS_REMOVED) |
			   BIT(NPM13XX_EVENT_CHG_COMPLETED) |
			   BIT(NPM13XX_EVENT_CHG_ERROR));

	ret = mfd_npm13xx_add_callback(pmic_dev, &pmic_cb);
	if (ret != 0) {
		LOG_WRN("PMIC interrupt callback failed: %d (polling only)", ret);
		/* Continue with polling only */
	}

	/* Initial read */
	read_and_update();

	/* Start periodic battery level polling */
	k_work_init_delayable(&battery_level_work, battery_level_handler);
	k_work_schedule(&battery_level_work, K_SECONDS(60));

	LOG_INF("Battery monitor initialized (level poll: 60s, fuel gauge: %s)",
		fg_initialized ? "enabled" : "disabled");

	return 0;
}
