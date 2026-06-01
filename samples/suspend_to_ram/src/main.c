/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PM_STATE_SUSPEND_TO_RAM test for ReSpeaker Clip (nRF5340) with the
 * peripheral rails turned off. No nRF7002 / MPSL / WiFi / BT stack.
 *
 * Network core forced off; nPM1300 LDO1/LDO2 disabled and BUCK2 forced
 * to PFM; OLED/RFSW load switches held LOW.
 *
 * Measure with a Power Profiler Kit on the 3V3 rail.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/dt-bindings/regulator/npm13xx.h>
#include <hal/nrf_reset.h>

LOG_MODULE_REGISTER(suspend_to_ram, LOG_LEVEL_INF);

#define SUSPEND_INDICATE_PIN 27
#define RFSW_EN_PIN  29  /* gpio0.29 - rfsw_reg (RF switch) */
#define OLED_EN_PIN   8  /* gpio1.8  - oled_reg (OLED panel VDD) */

static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct device *buck2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_buck2));
static const struct device *ldo1  = DEVICE_DT_GET(DT_NODELABEL(npm1300_ldo1));
static const struct device *ldo2  = DEVICE_DT_GET(DT_NODELABEL(npm1300_ldo2));

static void on_pm_state_entry(enum pm_state state)
{
	if (state == PM_STATE_SUSPEND_TO_RAM) {
		gpio_pin_set(gpio0, SUSPEND_INDICATE_PIN, 1);
	}
}

static void on_pm_state_exit(enum pm_state state)
{
	ARG_UNUSED(state);
	gpio_pin_set(gpio0, SUSPEND_INDICATE_PIN, 0);
}

static struct pm_notifier notifier = {
	.state_entry = on_pm_state_entry,
	.state_exit = on_pm_state_exit,
};

static void shutdown_peripheral_rails(void)
{
	gpio_pin_configure(gpio1, OLED_EN_PIN, GPIO_OUTPUT_LOW);

	if (device_is_ready(ldo1)) {
		regulator_disable(ldo1);
		LOG_INF("LDO1 (mic) disabled");
	}
	if (device_is_ready(ldo2)) {
		regulator_disable(ldo2);
		LOG_INF("LDO2 (SD) disabled");
	}
	if (device_is_ready(buck2)) {
		int ret = regulator_set_mode(buck2, NPM13XX_BUCK_MODE_PFM);

		if (ret) {
			LOG_WRN("BUCK2 PFM mode failed: %d", ret);
		} else {
			LOG_INF("BUCK2 set to PFM");
		}
	}
}

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_NAME_COMPLETE, 'C', 'l', 'i', 'p'),
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return;
	}
	LOG_INF("Bluetooth initialized");

	err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Advertising failed: %d", err);
		return;
	}
	LOG_INF("Advertising as \"Clip\" started");
}

int main(void)
{
	if (!device_is_ready(gpio0) || !device_is_ready(gpio1)) {
		LOG_ERR("gpio not ready");
		return -1;
	}

	int ret = gpio_pin_configure(gpio0, SUSPEND_INDICATE_PIN, GPIO_OUTPUT_INACTIVE);

	if (ret != 0) {
		LOG_ERR("gpio_pin_configure failed: %d", ret);
		return -1;
	}

	shutdown_peripheral_rails();

	pm_notifier_register(&notifier);

	bt_enable(bt_ready);

	LOG_INF("=== suspend_to_ram ready (BT adv + 60s hello) ===");
	LOG_INF("Probe GPIO0.%d: HIGH = in suspend-to-RAM.",
		SUSPEND_INDICATE_PIN);

	while (1) {
		LOG_INF("hello");
		k_sleep(K_SECONDS(60));
	}

	return 0;
}
