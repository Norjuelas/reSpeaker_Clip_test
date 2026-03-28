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

#include "battery.h"
#include "clip.h"
#include "clip_event.h"
#include "display.h"
#include "ble.h"
#include "transfer.h"

LOG_MODULE_REGISTER(battery, CONFIG_CLIP_LOG_LEVEL);

/* Charger status bitmasks (BCHGCHARGESTATUS register) */
#define CHG_STATUS_COMPLETE_MASK BIT(1)
#define CHG_STATUS_TRICKLE_MASK  BIT(2)
#define CHG_STATUS_CC_MASK       BIT(3)
#define CHG_STATUS_CV_MASK       BIT(4)

/* Voltage to SoC lookup table */
struct volt_soc_entry {
	uint32_t mv;
	uint8_t  soc;
};

static const struct volt_soc_entry volt_soc_table[] = {
	{ 4150, 100 },
	{ 4060,  90 },
	{ 3980,  80 },
	{ 3900,  70 },
	{ 3820,  60 },
	{ 3740,  50 },
	{ 3660,  40 },
	{ 3570,  30 },
	{ 3480,  20 },
	{ 3390,  10 },
	{ 3300,   0 },
};

/* Device references */
static const struct device *pmic_dev;
static const struct device *charger_dev;

/* Cached state */
static uint8_t last_percent;
static bool last_charging;

/* 60-second periodic battery level polling */
static struct k_work_delayable battery_level_work;

static uint8_t voltage_to_soc(uint32_t mv)
{
	const int n = ARRAY_SIZE(volt_soc_table);

	if (mv >= volt_soc_table[0].mv) {
		return 100;
	}
	if (mv <= volt_soc_table[n - 1].mv) {
		return 0;
	}

	for (int i = 0; i < n - 1; i++) {
		const struct volt_soc_entry *hi = &volt_soc_table[i];
		const struct volt_soc_entry *lo = &volt_soc_table[i + 1];

		if (mv <= hi->mv && mv > lo->mv) {
			uint32_t range_mv = hi->mv - lo->mv;
			uint32_t offset_mv = mv - lo->mv;
			uint8_t range_soc = hi->soc - lo->soc;

			return lo->soc + (uint8_t)(offset_mv * range_soc / range_mv);
		}
	}

	return 0;
}

static void read_and_update(void)
{
	struct sensor_value val;
	struct clip_context *ctx = clip_get_context();

	if (!device_is_ready(charger_dev)) {
		return;
	}

	if (sensor_sample_fetch(charger_dev) != 0) {
		LOG_WRN("Battery sensor sample fetch failed");
		return;
	}

	/* Read battery voltage → SoC */
	if (sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &val) == 0) {
		uint32_t voltage_mv = (uint32_t)(val.val1 * 1000 + val.val2 / 1000);
		uint8_t percent = voltage_to_soc(voltage_mv);

		if (percent != last_percent) {
			last_percent = percent;
			bt_bas_set_battery_level(percent);
			ctx->status.battery_percent = percent;
			LOG_INF("Battery: %u%% (%u mV)", percent, voltage_mv);
		}
	}

	/* Read charging status */
	if (sensor_channel_get(charger_dev, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &val) == 0) {
		int32_t status = val.val1;
		bool charging = (status & (CHG_STATUS_TRICKLE_MASK |
					   CHG_STATUS_CC_MASK |
					   CHG_STATUS_CV_MASK)) != 0;

		if (charging != last_charging) {
			last_charging = charging;
			ctx->status.battery_charging = charging;

			if (charging) {
				bt_bas_bls_set_battery_charge_state(
					BT_BAS_BLS_CHARGE_STATE_CHARGING);

				/* Set charge type */
				if (status & CHG_STATUS_TRICKLE_MASK) {
					bt_bas_bls_set_battery_charge_type(
						BT_BAS_BLS_CHARGE_TYPE_TRICKLE);
				} else if (status & CHG_STATUS_CV_MASK) {
					bt_bas_bls_set_battery_charge_type(
						BT_BAS_BLS_CHARGE_TYPE_CONSTANT_VOLTAGE);
				} else if (status & CHG_STATUS_CC_MASK) {
					bt_bas_bls_set_battery_charge_type(
						BT_BAS_BLS_CHARGE_TYPE_CONSTANT_CURRENT);
				}

				LOG_INF("Charging: type=%s",
					(status & CHG_STATUS_TRICKLE_MASK) ? "trickle" :
					(status & CHG_STATUS_CV_MASK) ? "CV" : "CC");
			} else {
				bt_bas_bls_set_battery_charge_state(
					BT_BAS_BLS_CHARGE_STATE_DISCHARGING_ACTIVE);
				LOG_INF("Discharging");
			}
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
		LOG_INF("PMIC event: Charge completed");
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

	pmic_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300));
	charger_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));

	if (!device_is_ready(charger_dev)) {
		LOG_WRN("NPM1300 charger not ready");
		return -ENODEV;
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

	LOG_INF("Battery monitor initialized (level poll: 60s)");

	return 0;
}
