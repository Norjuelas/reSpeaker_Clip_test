/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PM_STATE_SUSPEND_TO_RAM test for ReSpeaker Clip (nRF5340) with the
 * peripheral rails turned off. No nRF7002 / MPSL / WiFi / BT stack.
 *
 * Network core forced off; nPM1300 LDO1/LDO2 disabled and BUCK2 forced
 * to PFM; OLED/RFSW load switches held LOW.
 *
 * Tests SD card mount/unmount/poweroff in a loop every 30s to verify
 * current returns to ~250µA after each cycle.
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
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/pm/device.h>
#include <ff.h>
#include <hal/nrf_reset.h>

LOG_MODULE_REGISTER(suspend_to_ram, LOG_LEVEL_INF);

#define SUSPEND_INDICATE_PIN 27
#define RFSW_EN_PIN  29  /* gpio0.29 - rfsw_reg (RF switch) */
#define OLED_EN_PIN   8  /* gpio1.8  - oled_reg (OLED panel VDD) */

/* SPI4 pins: SCK=0.6, MOSI=0.8, MISO=0.7, CS=0.9 */
#define SPI4_SCK  6
#define SPI4_MISO 7
#define SPI4_MOSI 8
#define SPI4_CS   9

static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct device *buck2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_buck2));
static const struct device *ldo1  = DEVICE_DT_GET(DT_NODELABEL(npm1300_ldo1));
static const struct device *ldo2  = DEVICE_DT_GET(DT_NODELABEL(npm1300_ldo2));

static const struct device *sdhc_dev = DEVICE_DT_GET(DT_NODELABEL(sdhc0));
static const struct device *spi4_dev = DEVICE_DT_GET(DT_NODELABEL(spi4));

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};
static bool sd_suspended;

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

static void disconnect_spi4_pins(void)
{
	gpio_pin_configure(gpio0, SPI4_SCK,  GPIO_INPUT | GPIO_PULL_DOWN);
	gpio_pin_configure(gpio0, SPI4_MISO, GPIO_INPUT | GPIO_PULL_DOWN);
	gpio_pin_configure(gpio0, SPI4_MOSI, GPIO_INPUT | GPIO_PULL_DOWN);
	gpio_pin_configure(gpio0, SPI4_CS,   GPIO_INPUT | GPIO_PULL_DOWN);
}

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

static int test_sd_card(void)
{
	int rc;
	struct fs_statvfs stat;

	/* Resume devices if they were previously suspended */
	if (sd_suspended) {
		if (device_is_ready(spi4_dev)) {
			pm_device_action_run(spi4_dev, PM_DEVICE_ACTION_RESUME);
			LOG_INF("SPI4 resumed");
		}
		if (device_is_ready(sdhc_dev)) {
			pm_device_action_run(sdhc_dev, PM_DEVICE_ACTION_RESUME);
			LOG_INF("SDHC resumed");
		}
		sd_suspended = false;
	}

	/* Reset FATFS object for fresh mount */
	memset(&fat_fs, 0, sizeof(fat_fs));

	/* Power on SD card via LDO2 */
	if (device_is_ready(ldo2)) {
		regulator_enable(ldo2);
		k_msleep(50);
		LOG_INF("LDO2 (SD) enabled");
	}

	/* Init disk */
	rc = disk_access_init("SD");
	if (rc) {
		LOG_ERR("disk_access_init failed: %d", rc);
		goto power_off;
	}
	LOG_INF("SD disk initialized");

	/* Mount filesystem */
	rc = fs_mount(&mp);
	if (rc) {
		LOG_ERR("fs_mount failed: %d", rc);
		goto power_off;
	}
	LOG_INF("SD mounted");

	/* Check free space */
	rc = fs_statvfs("/SD:", &stat);
	if (rc == 0) {
		LOG_INF("SD: %u MB free",
			(unsigned)(stat.f_bfree * stat.f_bsize / (1024 * 1024)));
	}

	/* Write a test file */
	struct fs_file_t file;

	fs_file_t_init(&file);
	rc = fs_open(&file, "/SD:/test_suspend.txt", FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (rc == 0) {
		fs_write(&file, "SD card test before suspend\n", 28);
		fs_close(&file);
		LOG_INF("Test file written");
	}

	/* Unmount filesystem */
	rc = fs_unmount(&mp);
	if (rc) {
		LOG_ERR("fs_unmount failed: %d", rc);
		goto power_off;
	}
	LOG_INF("SD unmounted");

	/* Force SDHC and SPI4 into suspend */
	if (device_is_ready(sdhc_dev)) {
		pm_device_action_run(sdhc_dev, PM_DEVICE_ACTION_SUSPEND);
		LOG_INF("SDHC suspended");
	}
	if (device_is_ready(spi4_dev)) {
		pm_device_action_run(spi4_dev, PM_DEVICE_ACTION_SUSPEND);
		LOG_INF("SPI4 suspended");
	}
	sd_suspended = true;

	k_msleep(100);

power_off:
	/* Power off SD card via LDO2 */
	if (device_is_ready(ldo2)) {
		regulator_disable(ldo2);
		LOG_INF("LDO2 (SD) disabled");
	}

	/* Disconnect SPI4 pins to prevent leakage */
	disconnect_spi4_pins();
	LOG_INF("SPI4 pins disconnected");

	LOG_INF("SD card cycle complete");
	return rc;
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

	/* Shut down all peripheral rails */
	shutdown_peripheral_rails();

	pm_notifier_register(&notifier);

	bt_enable(bt_ready);

	LOG_INF("=== suspend_to_ram ready (30s SD cycle) ===");
	LOG_INF("Probe GPIO0.%d: HIGH = in suspend-to-RAM.",
		SUSPEND_INDICATE_PIN);

	while (1) {
		LOG_INF("--- SD cycle start ---");
		test_sd_card();
		LOG_INF("--- SD cycle end, sleeping 30s ---");
		k_sleep(K_SECONDS(30));
	}

	return 0;
}
