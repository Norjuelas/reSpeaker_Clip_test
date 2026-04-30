/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF7002 OTP MAC address programming tool.
 *
 * Shell commands:
 *   nrf70 otp status                    - Show OTP state and MAC addresses
 *   nrf70 otp read                      - Raw dump of all OTP fields
 *   nrf70 otp write_mac0 AA:BB:CC:DD:EE:FF  - Write MAC0
 *   nrf70 otp write_mac1 AA:BB:CC:DD:EE:FF  - Write MAC1
 *   nrf70 otp lock                      - Lock OTP (IRREVERSIBLE!)
 *
 * Build:
 *   west build --build-dir build-otp --pristine --board clip/nrf5340/cpuapp tests/otp
 *   west flash --build-dir build-otp && nrfutil device reset
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/wifi/nrf_wifi/bus/rpu_hw_if.h>
#include <common/rpu_if.h>

LOG_MODULE_REGISTER(nrf70_otp, CONFIG_WIFI_LOG_LEVEL);

/* =============================================================================
 * Low-level OTP access (from Nordic radio_test ficr_prog.c)
 * ============================================================================= */

static void write_word(unsigned int addr, unsigned int data)
{
	rpu_write(addr, &data, 4);
}

static void read_word(unsigned int addr, unsigned int *data)
{
	rpu_read(addr, data, 4);
}

static void set_otp_timing_reg_40mhz(void)
{
	write_word(OTP_TIMING_REG1_ADDR, OTP_TIMING_REG1_VAL);
	write_word(OTP_TIMING_REG2_ADDR, OTP_TIMING_REG2_VAL);
}

static int poll_otp_ready(void)
{
	unsigned int status;

	for (int i = 0; i < 100; i++) {
		read_word(OTP_POLL_ADDR, &status);
		if ((status & OTP_READY) == OTP_READY) {
			return 0;
		}
	}
	LOG_ERR("OTP not ready");
	return -ENOEXEC;
}

static int req_otp_standby_mode(void)
{
	write_word(OTP_RWSBMODE_ADDR, 0x0);
	return poll_otp_ready();
}

static int otp_set_voltage(unsigned int voltage)
{
	int err = req_otp_standby_mode();

	if (err) {
		return err;
	}
	write_word(OTP_VOLTCTRL_ADDR, voltage);
	return 0;
}

static int poll_otp_flag(unsigned int mask)
{
	unsigned int status;

	for (int i = 0; i < 100; i++) {
		read_word(OTP_POLL_ADDR, &status);
		if ((status & mask) == mask) {
			return 0;
		}
	}
	return -ENOEXEC;
}

static int req_otp_read_mode(void)
{
	write_word(OTP_RWSBMODE_ADDR, OTP_READ_MODE);
	return poll_otp_ready();
}

static int req_otp_byte_write_mode(void)
{
	write_word(OTP_RWSBMODE_ADDR, OTP_BYTE_WRITE_MODE);
	return poll_otp_ready();
}

static int read_otp_location(unsigned int offset, unsigned int *val)
{
	write_word(OTP_RDENABLE_ADDR, offset);
	int err = poll_otp_flag(OTP_READ_VALID);

	if (err) {
		return err;
	}
	read_word(OTP_READREG_ADDR, val);
	return 0;
}

static int write_otp_location(unsigned int offset, unsigned int data)
{
	write_word(OTP_WRENABLE_ADDR, offset);
	write_word(OTP_WRITEREG_ADDR, data);
	return poll_otp_flag(OTP_WR_DONE);
}

/* =============================================================================
 * High-level OTP operations
 * ============================================================================= */

static int read_otp_memory(unsigned int addr, unsigned int *buf, int len)
{
	int err;

	err = poll_otp_ready();
	if (err) {
		return err;
	}
	set_otp_timing_reg_40mhz();

	err = otp_set_voltage(OTP_VOLTCTRL_1V8);
	if (err) {
		return err;
	}
	err = req_otp_read_mode();
	if (err) {
		return err;
	}
	for (int i = 0; i < len; i++) {
		read_otp_location(addr + i, &buf[i]);
	}
	return req_otp_standby_mode();
}

static unsigned int check_protection(unsigned int *buf)
{
	if (buf[REGION_PROTECT] == OTP_PROGRAMMED &&
	    buf[REGION_PROTECT + 1] == OTP_PROGRAMMED &&
	    buf[REGION_PROTECT + 2] == OTP_PROGRAMMED &&
	    buf[REGION_PROTECT + 3] == OTP_PROGRAMMED) {
		return OTP_PROGRAMMED;
	}
	if (buf[REGION_PROTECT] == OTP_FRESH_FROM_FAB &&
	    buf[REGION_PROTECT + 1] == OTP_FRESH_FROM_FAB &&
	    buf[REGION_PROTECT + 2] == OTP_FRESH_FROM_FAB &&
	    buf[REGION_PROTECT + 3] == OTP_FRESH_FROM_FAB) {
		return OTP_FRESH_FROM_FAB;
	}
	if (buf[REGION_PROTECT] == OTP_ENABLE_PATTERN &&
	    buf[REGION_PROTECT + 1] == OTP_ENABLE_PATTERN &&
	    buf[REGION_PROTECT + 2] == OTP_ENABLE_PATTERN &&
	    buf[REGION_PROTECT + 3] == OTP_ENABLE_PATTERN) {
		return OTP_ENABLE_PATTERN;
	}
	return 0xDEADBEEF;
}

static int write_mac_to_otp(unsigned int index, unsigned int *words)
{
	int err;

	err = poll_otp_ready();
	if (err) {
		return err;
	}
	set_otp_timing_reg_40mhz();

	err = otp_set_voltage(OTP_VOLTCTRL_2V5);
	if (err) {
		return err;
	}
	err = req_otp_byte_write_mode();
	if (err) {
		return err;
	}

	/* Write 2 words for the MAC address */
	for (int i = 0; i < 2; i++) {
		err = write_otp_location(MAC0_ADDR + 2 * index + i, words[i]);
		if (err) {
			LOG_ERR("Failed to write MAC%d word%d", index, i);
			goto exit;
		}
		LOG_INF("MAC%d word%d = 0x%08x", index, i, words[i]);
	}

	/* Update REGION_DEFAULTS flag */
	unsigned int mask = (index == 0) ? MAC0_ADDR_FLAG_MASK : MAC1_ADDR_FLAG_MASK;

	write_otp_location(REGION_DEFAULTS, mask);
	return 0;

exit:
	req_otp_standby_mode();
	otp_set_voltage(OTP_VOLTCTRL_1V8);
	return err;
}

static int lock_otp(void)
{
	int err;

	err = poll_otp_ready();
	if (err) {
		return err;
	}
	set_otp_timing_reg_40mhz();

	err = otp_set_voltage(OTP_VOLTCTRL_2V5);
	if (err) {
		return err;
	}
	err = req_otp_byte_write_mode();
	if (err) {
		return err;
	}

	for (int i = 0; i < 4; i++) {
		write_otp_location(REGION_PROTECT + i, OTP_PROGRAMMED);
		LOG_INF("REGION_PROTECT%d = LOCKED", i);
	}

	req_otp_standby_mode();
	otp_set_voltage(OTP_VOLTCTRL_1V8);
	return 0;
}

static int unlock_otp(void)
{
	int err;

	err = poll_otp_ready();
	if (err) {
		return err;
	}
	set_otp_timing_reg_40mhz();

	err = otp_set_voltage(OTP_VOLTCTRL_2V5);
	if (err) {
		return err;
	}
	err = req_otp_byte_write_mode();
	if (err) {
		return err;
	}

	for (int i = 0; i < 4; i++) {
		write_otp_location(REGION_PROTECT + i, OTP_ENABLE_PATTERN);
	}

	req_otp_standby_mode();
	otp_set_voltage(OTP_VOLTCTRL_1V8);
	return 0;
}

/* =============================================================================
 * MAC address helpers
 * ============================================================================= */

static int parse_mac(const char *str, unsigned int words[2])
{
	unsigned int b[6];

	if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
		   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
		return -EINVAL;
	}
	/* All zeros or multicast */
	if (b[0] == 0 && b[1] == 0 && b[2] == 0 &&
	    b[3] == 0 && b[4] == 0 && b[5] == 0) {
		return -EINVAL;
	}
	if (b[0] & 0x01) {
		return -EINVAL;
	}
	words[0] = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
	words[1] = b[4] | (b[5] << 8);
	return 0;
}

static void format_mac(const unsigned int words[2], char *buf, size_t len)
{
	snprintk(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 (uint8_t)(words[0]), (uint8_t)(words[0] >> 8),
		 (uint8_t)(words[0] >> 16), (uint8_t)(words[0] >> 24),
		 (uint8_t)(words[1]), (uint8_t)(words[1] >> 8));
}

/* =============================================================================
 * Shell commands
 * ============================================================================= */

static int cmd_status(const struct shell *shell, size_t argc, char *argv[])
{
	unsigned int val[OTP_MAX_WORD_LEN];
	unsigned int prot;
	char mac[18];
	int err;

	err = read_otp_memory(0, val, OTP_MAX_WORD_LEN);
	if (err) {
		shell_error(shell, "Failed to read OTP");
		return -ENOEXEC;
	}

	prot = check_protection(val);

	shell_print(shell, "=== nRF70 OTP ===");
	switch (prot) {
	case OTP_PROGRAMMED:
		shell_print(shell, "Protection: LOCKED");
		break;
	case OTP_ENABLE_PATTERN:
		shell_print(shell, "Protection: OPEN (R/W)");
		break;
	case OTP_FRESH_FROM_FAB:
		shell_print(shell, "Protection: FRESH (unprogrammed)");
		break;
	default:
		shell_print(shell, "Protection: INVALID (0x%08x)", prot);
		break;
	}

	shell_print(shell, "REGION_DEFAULTS: 0x%08x", val[REGION_DEFAULTS]);

	/* MAC0 */
	if (!(val[REGION_DEFAULTS] & (~MAC0_ADDR_FLAG_MASK))) {
		unsigned int m[2] = { val[MAC0_ADDR], val[MAC0_ADDR + 1] };
		format_mac(m, mac, sizeof(mac));
		shell_print(shell, "MAC0: %s", mac);
	} else {
		shell_print(shell, "MAC0: not programmed");
	}

	/* MAC1 */
	if (!(val[REGION_DEFAULTS] & (~MAC1_ADDR_FLAG_MASK))) {
		unsigned int m[2] = { val[MAC1_ADDR], val[MAC1_ADDR + 1] };
		format_mac(m, mac, sizeof(mac));
		shell_print(shell, "MAC1: %s", mac);
	} else {
		shell_print(shell, "MAC1: not programmed");
	}

	return 0;
}

static int cmd_read(const struct shell *shell, size_t argc, char *argv[])
{
	unsigned int val[OTP_MAX_WORD_LEN];
	char mac[18];
	int err;

	err = read_otp_memory(0, val, OTP_MAX_WORD_LEN);
	if (err) {
		shell_error(shell, "Failed to read OTP");
		return -ENOEXEC;
	}

	shell_print(shell, "=== OTP Raw Dump ===");
	shell_print(shell, "REGION_PROTECT: %08x %08x %08x %08x",
		    val[REGION_PROTECT], val[REGION_PROTECT + 1],
		    val[REGION_PROTECT + 2], val[REGION_PROTECT + 3]);
	shell_print(shell, "REGION_DEFAULTS: 0x%08x", val[REGION_DEFAULTS]);
	shell_print(shell, "INFO.PART:    0x%08x", val[INFO_PART]);
	shell_print(shell, "INFO.VARIANT: 0x%08x", val[INFO_VARIANT]);
	shell_print(shell, "INFO.UUID:    %08x %08x %08x %08x",
		    val[INFO_UUID], val[INFO_UUID + 1],
		    val[INFO_UUID + 2], val[INFO_UUID + 3]);

	shell_print(shell, "MAC0 raw: %08x %08x", val[MAC0_ADDR], val[MAC0_ADDR + 1]);
	{
		unsigned int m[2] = { val[MAC0_ADDR], val[MAC0_ADDR + 1] };
		format_mac(m, mac, sizeof(mac));
		shell_print(shell, "MAC0:     %s", mac);
	}
	shell_print(shell, "MAC1 raw: %08x %08x", val[MAC1_ADDR], val[MAC1_ADDR + 1]);
	{
		unsigned int m[2] = { val[MAC1_ADDR], val[MAC1_ADDR + 1] };
		format_mac(m, mac, sizeof(mac));
		shell_print(shell, "MAC1:     %s", mac);
	}
	shell_print(shell, "CALIB_XO: 0x%02x", val[CALIB_XO] & 0xFF);

	return 0;
}

static int do_write_mac(const struct shell *shell, int index, const char *mac_str)
{
	unsigned int val[OTP_MAX_WORD_LEN];
	unsigned int prot;
	unsigned int words[2];
	int err;

	err = parse_mac(mac_str, words);
	if (err) {
		shell_error(shell, "Invalid MAC: %s (format: AA:BB:CC:DD:EE:FF)", mac_str);
		return -EINVAL;
	}

	/* Check protection */
	err = read_otp_memory(REGION_PROTECT, val, 4);
	if (err) {
		shell_error(shell, "Failed to read OTP");
		return -ENOEXEC;
	}

	prot = check_protection(val);
	if (prot == OTP_PROGRAMMED) {
		shell_error(shell, "OTP is LOCKED - cannot write");
		return -EPERM;
	}

	/* Unlock if fresh */
	if (prot == OTP_FRESH_FROM_FAB) {
		shell_print(shell, "OTP fresh - unlocking...");
		unlock_otp();
	}

	shell_print(shell, "Writing MAC%d: %s", index, mac_str);
	err = write_mac_to_otp(index, words);
	if (err) {
		shell_error(shell, "Write failed (err=%d)", err);
		return -ENOEXEC;
	}
	shell_print(shell, "MAC%d written OK", index);

	/* Verify */
	err = read_otp_memory(MAC0_ADDR + 2 * index, words, 2);
	if (err == 0) {
		char buf[18];

		format_mac(words, buf, sizeof(buf));
		shell_print(shell, "Verify: %s", buf);
	}

	return 0;
}

static int cmd_write_mac0(const struct shell *shell, size_t argc, char *argv[])
{
	return do_write_mac(shell, 0, argv[1]);
}

static int cmd_write_mac1(const struct shell *shell, size_t argc, char *argv[])
{
	return do_write_mac(shell, 1, argv[1]);
}

static int cmd_lock(const struct shell *shell, size_t argc, char *argv[])
{
	unsigned int val[4];
	unsigned int prot;
	int err;

	err = read_otp_memory(REGION_PROTECT, val, 4);
	if (err) {
		shell_error(shell, "Failed to read OTP");
		return -ENOEXEC;
	}

	prot = check_protection(val);
	if (prot == OTP_PROGRAMMED) {
		shell_warn(shell, "Already LOCKED");
		return 0;
	}
	if (prot == OTP_FRESH_FROM_FAB) {
		shell_error(shell, "OTP is fresh - write MAC first");
		return -ENOEXEC;
	}

	shell_warn(shell, "WARNING: This is IRREVERSIBLE!");
	shell_print(shell, "Locking OTP...");
	err = lock_otp();
	if (err) {
		shell_error(shell, "Lock failed (err=%d)", err);
		return -ENOEXEC;
	}
	shell_print(shell, "OTP LOCKED");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	otp_subcmds,
	SHELL_CMD_ARG(status, NULL, "Show OTP status and MAC addresses",
		      cmd_status, 1, 0),
	SHELL_CMD_ARG(read, NULL, "Raw dump of all OTP fields",
		      cmd_read, 1, 0),
	SHELL_CMD_ARG(write_mac0, NULL,
		      "Write MAC0 (format: AA:BB:CC:DD:EE:FF)",
		      cmd_write_mac0, 2, 0),
	SHELL_CMD_ARG(write_mac1, NULL,
		      "Write MAC1 (format: AA:BB:CC:DD:EE:FF)",
		      cmd_write_mac1, 2, 0),
	SHELL_CMD_ARG(lock, NULL, "Lock OTP region (IRREVERSIBLE!)",
		      cmd_lock, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nrf70, &otp_subcmds,
		   "nRF70 OTP programming", NULL);

int main(void)
{
	printk("nRF70 OTP Programming Tool\n");
	printk("Commands: nrf70 otp status/read/write_mac0/write_mac1/lock\n");
	return 0;
}
