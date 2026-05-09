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
#include <nrfx_clock.h>
#include <hal/nrf_oscillators.h>
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

/* LFXO capacitance commands */
static const char *lfxo_cap_name(uint32_t val)
{
	switch (val) {
	case 0: return "External";
	case 1: return "6 pF";
	case 2: return "7 pF";
	case 3: return "9 pF";
	default: return "Unknown";
	}
}

static int cmd_lfxo_cap_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t cap = NRF_OSCILLATORS->XOSC32KI.INTCAP;
	shell_print(sh, "LFXO capacitance: %u (%s)", cap, lfxo_cap_name(cap));
	return 0;
}

static int cmd_lfxo_cap_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "Usage: lfxo cap <0-3>");
		shell_print(sh, "  0 = External capacitors");
		shell_print(sh, "  1 = 6 pF internal");
		shell_print(sh, "  2 = 7 pF internal");
		shell_print(sh, "  3 = 9 pF internal");
		return -EINVAL;
	}

	int val = atoi(argv[1]);
	if (val < 0 || val > 3) {
		shell_print(sh, "Error: value must be 0-3");
		return -EINVAL;
	}

	NRF_OSCILLATORS->XOSC32KI.INTCAP = (uint32_t)val;
	shell_print(sh, "LFXO capacitance set to: %u (%s)", val, lfxo_cap_name(val));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lfxo,
	SHELL_CMD(cap, NULL, "Get/set LFXO capacitance (0=ext, 1=6pF, 2=7pF, 3=9pF)",
		  NULL),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lfxo_cap,
	SHELL_CMD(get, NULL, "Get LFXO capacitance", cmd_lfxo_cap_get),
	SHELL_CMD_ARG(set, NULL, "Set LFXO capacitance (0-3)", cmd_lfxo_cap_set, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(lfxo, &sub_lfxo_cap, "LFXO 32.768kHz crystal capacitance", NULL);

/* HFXO capacitance commands */
static int cmd_hfxo_cap_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t reg = NRF_OSCILLATORS->XOSC32MCAPS;
	bool enabled = (reg & OSCILLATORS_XOSC32MCAPS_ENABLE_Msk) != 0;
	uint32_t capvalue = (reg & OSCILLATORS_XOSC32MCAPS_CAPVALUE_Msk)
		>> OSCILLATORS_XOSC32MCAPS_CAPVALUE_Pos;

	if (enabled) {
		shell_print(sh, "HFXO capacitance: internal (CAPVALUE=%u)", capvalue);
	} else {
		shell_print(sh, "HFXO capacitance: external");
	}
	return 0;
}

static int cmd_hfxo_cap_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "Usage: hfxo set <picofarad>");
		shell_print(sh, "  Range: 7.0 - 20.0 pF, step 0.5 pF");
		shell_print(sh, "  Example: hfxo set 9.5");
		shell_print(sh, "  Use 0 to disable (external capacitors)");
		return -EINVAL;
	}

	float pf = strtof(argv[1], NULL);
	if (pf == 0.0f) {
		NRF_OSCILLATORS->XOSC32MCAPS =
			OSCILLATORS_XOSC32MCAPS_ENABLE_Disabled <<
			OSCILLATORS_XOSC32MCAPS_ENABLE_Pos;
		shell_print(sh, "HFXO capacitance: external");
		return 0;
	}

	if (pf < 7.0f || pf > 20.0f) {
		shell_print(sh, "Error: value must be 7.0-20.0 pF or 0 for external");
		return -EINVAL;
	}

	uint32_t capvalue = NRF_OSCILLATORS_HFXO_CAP_CALCULATE(NRF_FICR, pf);
	NRF_OSCILLATORS->XOSC32MCAPS =
		(OSCILLATORS_XOSC32MCAPS_ENABLE_Enabled <<
		 OSCILLATORS_XOSC32MCAPS_ENABLE_Pos) |
		(capvalue << OSCILLATORS_XOSC32MCAPS_CAPVALUE_Pos);
	shell_print(sh, "HFXO capacitance set to: %.1f pF (CAPVALUE=%u)", pf, capvalue);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hfxo,
	SHELL_CMD(get, NULL, "Get HFXO capacitance", cmd_hfxo_cap_get),
	SHELL_CMD_ARG(set, NULL, "Set HFXO capacitance in pF (7.0-20.0, 0=external)",
		      cmd_hfxo_cap_set, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(hfxo, &sub_hfxo, "HFXO 32MHz crystal capacitance", NULL);

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
