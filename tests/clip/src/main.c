/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include "wifi.h"
#include "ble.h"
#include "button.h"
#include "sdcard.h"
#include "mic.h"
#include "oled.h"
#include "pmic.h"
#include "motor.h"
#include "imu.h"
#include "usb.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting...");
	k_sleep(K_MSEC(100));
	sys_reboot(SYS_REBOOT_WARM);
	return 0;
}

SHELL_CMD_REGISTER(reboot, NULL, "Reboot device", cmd_reboot);

int main(void)
{
	int ret;

	printk("Starting %s with CPU frequency: %d MHz\n", CONFIG_BOARD, SystemCoreClock / 1000000);

	/* Initialize Button */
	ret = button_init();
	if (ret != 0) {
		LOG_ERR("Button initialization failed: %d", ret);
	}

	/* Initialize SD Card (auto-mounts if present) */
	ret = sdcard_init();
	if (ret != 0) {
		LOG_ERR("SD card initialization failed: %d", ret);
		/* Continue anyway, SD card is optional */
	}

	/* Initialize Microphone */
	ret = mic_init();
	if (ret != 0) {
		LOG_ERR("Microphone initialization failed: %d", ret);
		/* Continue anyway, MIC is optional */
	}

	/* Initialize OLED display */
	ret = oled_init();
	if (ret != 0) {
		LOG_ERR("OLED display initialization failed: %d", ret);
		/* Continue anyway, OLED is optional */
	}

	/* Initialize PMIC (NPM1300) */
	ret = pmic_init();
	if (ret != 0) {
		LOG_ERR("PMIC initialization failed: %d", ret);
		/* Continue anyway, PMIC is optional for testing */
	}

	/* Initialize Motor */
	ret = motor_init();
	if (ret != 0) {
		LOG_ERR("Motor initialization failed: %d", ret);
		/* Continue anyway, Motor is optional for testing */
	}

	/* Initialize IMU (software I2C) */
	ret = imu_init();
	if (ret != 0) {
		LOG_WRN("IMU initialization failed: %d (optional)", ret);
	}

	/* Initialize USB MSC */
	ret = usb_msc_init();
	if (ret != 0) {
		LOG_WRN("USB MSC initialization failed: %d", ret);
	}

	/* Initialize BLE - starts advertising automatically */
	ret = ble_init();
	if (ret != 0) {
		LOG_ERR("Bluetooth initialization failed: %d", ret);
		/* Continue anyway, BLE is optional */
	}

	/* Initialize WiFi */
	ret = wifi_run_test();
	if (ret != 0) {
		LOG_ERR("WiFi initialization failed: %d", ret);
		/* Continue anyway, WiFi can be configured via shell */
	}

	printk("System ready\n");
	printk("WiFi: Use 'wifi on' to start AP, 'wifi status' to check\n");
	printk("BLE: Advertising as '%s'\n", CONFIG_BT_DEVICE_NAME);
	printk("SD card: Use 'sd mount' to mount, 'fs ls /SD:' to list files\n");
	printk("Flash: Use 'flash' commands for SPI flash operations\n");
	printk("MIC: Use 'mic capture [time_sec]' to capture audio\n");
	printk("OLED: Use 'oled test' to run display tests, 'oled help' for more\n");
	printk("PMIC: Use 'pmic status' to check battery, 'pmic ship' to power off\n");
	printk("Motor: Use 'motor pulse' or 'motor pattern' to test vibration\n");
	printk("IMU: Use 'imu init' to initialize, 'imu read' for sensor data\n");
	printk("USB: Use 'usb msc on' to expose SD card, 'mic record' to record WAV\n");

	return 0;
}
