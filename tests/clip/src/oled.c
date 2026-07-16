/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>
#include "oled.h"

LOG_MODULE_REGISTER(oled, LOG_LEVEL_INF);

/* Display dimensions */
#define OLED_WIDTH  88
#define OLED_HEIGHT 48
#define OLED_BUF_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

/* Display device */
static const struct device *display_dev = NULL;

/* Display buffer */
static uint8_t display_buffer[OLED_BUF_SIZE];

/* Write buffer to display */
static void oled_write_buffer(void)
{
	if (!display_dev) {
		return;
	}

	struct display_buffer_descriptor desc = {
		.buf_size = OLED_BUF_SIZE,
		.width = OLED_WIDTH,
		.height = OLED_HEIGHT,
		.pitch = OLED_WIDTH,
	};

	display_write(display_dev, 0, 0, &desc, display_buffer);
}

/* Clear display (set all pixels to 0) */
static void oled_clear_buffer(void)
{
	memset(display_buffer, 0, OLED_BUF_SIZE);
}

/* Fill display (set all pixels to 1) */
static void oled_fill_buffer(void)
{
	memset(display_buffer, 0xFF, OLED_BUF_SIZE);
}

/* Compact 5x7 glyphs for the persistent battery screen. They are rendered at
 * 2x scale, which lets all of the important telemetry fit on the 88x48 OLED
 * without importing a large general-purpose font into the test image. */
static const char battery_glyph_chars[] = " 0123456789%BATVmCHGDIS+-";
static const uint8_t battery_glyphs[][5] = {
	{0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
	{0x3e, 0x51, 0x49, 0x45, 0x3e}, /* 0 */
	{0x00, 0x42, 0x7f, 0x40, 0x00}, /* 1 */
	{0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
	{0x21, 0x41, 0x45, 0x4b, 0x31}, /* 3 */
	{0x18, 0x14, 0x12, 0x7f, 0x10}, /* 4 */
	{0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
	{0x3c, 0x4a, 0x49, 0x49, 0x30}, /* 6 */
	{0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
	{0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
	{0x06, 0x49, 0x49, 0x29, 0x1e}, /* 9 */
	{0x63, 0x13, 0x08, 0x64, 0x63}, /* % */
	{0x7f, 0x49, 0x49, 0x49, 0x36}, /* B */
	{0x7e, 0x11, 0x11, 0x11, 0x7e}, /* A */
	{0x01, 0x01, 0x7f, 0x01, 0x01}, /* T */
	{0x1f, 0x20, 0x40, 0x20, 0x1f}, /* V */
	{0x7c, 0x04, 0x18, 0x04, 0x78}, /* m */
	{0x3e, 0x41, 0x41, 0x41, 0x22}, /* C */
	{0x7f, 0x08, 0x08, 0x08, 0x7f}, /* H */
	{0x3e, 0x41, 0x49, 0x49, 0x7a}, /* G */
	{0x7f, 0x41, 0x41, 0x22, 0x1c}, /* D */
	{0x00, 0x41, 0x7f, 0x41, 0x00}, /* I */
	{0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
	{0x08, 0x08, 0x3e, 0x08, 0x08}, /* + */
	{0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
};

static void oled_set_pixel(uint8_t *buf, int x, int y)
{
	if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
		return;
	}
	buf[(y / 8) * OLED_WIDTH + x] |= BIT(y % 8);
}

static void oled_draw_battery_char(char c, int x, int y, int scale)
{
	const char *match = strchr(battery_glyph_chars, c);
	if (match == NULL) {
		return;
	}

	const uint8_t *glyph = battery_glyphs[match - battery_glyph_chars];
	for (int col = 0; col < 5; col++) {
		for (int row = 0; row < 7; row++) {
			if (!(glyph[col] & BIT(row))) {
				continue;
			}
			for (int dy = 0; dy < scale; dy++) {
				for (int dx = 0; dx < scale; dx++) {
					oled_set_pixel(display_buffer, x + col * scale + dx,
						       y + row * scale + dy);
				}
			}
		}
	}
}

static void oled_draw_battery_text(const char *text, int x, int y, int scale)
{
	while (*text != '\0') {
		oled_draw_battery_char(*text++, x, y, scale);
		x += 6 * scale;
	}
}

void oled_show_battery(uint8_t percent, uint32_t voltage_mv, bool charging,
		       int32_t temp_c)
{
	char line[12];

	if (display_dev == NULL) {
		return;
	}

	oled_clear_buffer();

	/* Three 2x-scale rows: SoC, voltage, then charge state + NTC temperature. */
	snprintk(line, sizeof(line), "BAT%u%%", percent);
	oled_draw_battery_text(line, 2, 0, 2);

	snprintk(line, sizeof(line), "%umV", voltage_mv);
	oled_draw_battery_text(line, 2, 16, 2);

	snprintk(line, sizeof(line), "%s%dC", charging ? "CHG" : "DIS", temp_c);
	oled_draw_battery_text(line, 2, 32, 2);

	oled_write_buffer();
}

/* Draw a simple test pattern */
static void oled_draw_test_pattern(void)
{
	oled_clear_buffer();

	/* Draw border */
	for (int x = 0; x < OLED_WIDTH; x++) {
		/* Top and bottom rows */
		display_buffer[x] = 0x80;
		display_buffer[OLED_BUF_SIZE - OLED_WIDTH + x] = 0x01;
	}
	for (int y = 1; y < OLED_HEIGHT / 8 - 1; y++) {
		/* Left and right columns */
		display_buffer[y * OLED_WIDTH] = 0xFF;
		display_buffer[(y + 1) * OLED_WIDTH - 1] = 0xFF;
	}

	/* Draw diagonal line */
	for (int i = 0; i < OLED_WIDTH && i < OLED_HEIGHT; i++) {
		int byte_idx = (i / 8) * OLED_WIDTH + i;
		int bit_idx = i % 8;
		if (byte_idx < OLED_BUF_SIZE) {
			display_buffer[byte_idx] |= (1 << bit_idx);
		}
	}

	oled_write_buffer();
	LOG_INF("Test pattern displayed");
}

/* Draw animated circle */
static void oled_draw_circle(int radius, int center_x, int center_y)
{
	oled_clear_buffer();

	/* Draw circle using midpoint circle algorithm */
	int x = radius;
	int y = 0;
	int err = 0;

	while (x >= y) {
		/* Draw 8 octants */
		int points[8][2] = {
			{center_x + x, center_y + y},
			{center_x + y, center_y + x},
			{center_x - y, center_y + x},
			{center_x - x, center_y + y},
			{center_x - x, center_y - y},
			{center_x - y, center_y - x},
			{center_x + y, center_y - x},
			{center_x + x, center_y - y}
		};

		for (int i = 0; i < 8; i++) {
			int px = points[i][0];
			int py = points[i][1];
			if (px >= 0 && px < OLED_WIDTH && py >= 0 && py < OLED_HEIGHT) {
				int byte_idx = (py / 8) * OLED_WIDTH + px;
				int bit_idx = py % 8;
				if (byte_idx < OLED_BUF_SIZE) {
					display_buffer[byte_idx] |= (1 << bit_idx);
				}
			}
		}

		if (err <= 0) {
			y += 1;
			err += 2 * y + 1;
		}
		if (err > 0) {
			x -= 1;
			err -= 2 * x + 1;
		}
	}

	oled_write_buffer();
}

/* Initialize OLED display */
int oled_init(void)
{
	LOG_INF("Initializing OLED display...");

	/* Get display device from devicetree */
	display_dev = DEVICE_DT_GET(DT_NODELABEL(ch1115));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	LOG_INF("OLED display ready (%dx%d)", OLED_WIDTH, OLED_HEIGHT);

	/* Clear display on startup */
	oled_clear_buffer();
	oled_write_buffer();

	return 0;
}

/* Test 1: Clear display */
void oled_test_clear(void)
{
	LOG_INF("Test: Clear display");
	oled_clear_buffer();
	oled_write_buffer();
	k_sleep(K_SECONDS(1));
}

/* Test 2: Fill display */
void oled_test_fill(void)
{
	LOG_INF("Test: Fill display");
	oled_fill_buffer();
	oled_write_buffer();
	k_sleep(K_SECONDS(1));
}

/* Test 3: Test pattern */
void oled_test_pattern(void)
{
	LOG_INF("Test: Display test pattern");
	oled_draw_test_pattern();
	k_sleep(K_SECONDS(2));
}

/* Test 4: Animated circle (breathing effect) */
void oled_test_circle_anim(void)
{
	LOG_INF("Test: Circle animation");

	for (int i = 0; i < 20; i++) {
		int radius = 5 + (i % 10);
		oled_draw_circle(radius, OLED_WIDTH / 2, OLED_HEIGHT / 2);
		k_sleep(K_MSEC(200));
	}

	oled_clear_buffer();
	oled_write_buffer();
}

/* Test 5: Pixel manipulation test */
void oled_test_pixels(void)
{
	LOG_INF("Test: Pixel manipulation");

	oled_clear_buffer();

	/* Draw checkerboard pattern */
	for (int y = 0; y < OLED_HEIGHT; y++) {
		for (int x = 0; x < OLED_WIDTH; x++) {
			if ((x + y) % 2 == 0) {
				int byte_idx = (y / 8) * OLED_WIDTH + x;
				int bit_idx = y % 8;
				if (byte_idx < OLED_BUF_SIZE) {
					display_buffer[byte_idx] |= (1 << bit_idx);
				}
			}
		}
	}

	oled_write_buffer();
	k_sleep(K_SECONDS(2));
	oled_clear_buffer();
	oled_write_buffer();
}

/* Run all OLED tests */
void oled_run_all_tests(void)
{
	LOG_INF("Running all OLED tests...");

	oled_test_clear();
	oled_test_fill();
	oled_test_pattern();
	oled_test_circle_anim();
	oled_test_pixels();

	LOG_INF("All OLED tests completed!");
}

/* ============================================================================
 * Shell Commands
 * ============================================================================ */

/* Shell command: oled clear */
static int cmd_oled_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	oled_test_clear();
	shell_print(sh, "Display cleared");

	return 0;
}

/* Shell command: oled fill */
static int cmd_oled_fill(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	oled_test_fill();
	shell_print(sh, "Display filled");

	return 0;
}

/* Shell command: oled pattern */
static int cmd_oled_pattern(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	oled_test_pattern();
	shell_print(sh, "Test pattern displayed");

	return 0;
}

/* Shell command: oled circle */
static int cmd_oled_circle(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	oled_test_circle_anim();
	shell_print(sh, "Circle animation completed");

	return 0;
}

/* Shell command: oled pixels */
static int cmd_oled_pixels(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	oled_test_pixels();
	shell_print(sh, "Pixel test completed");

	return 0;
}

/* Shell command: oled test (run all tests) */
static int cmd_oled_test(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Running all OLED tests...");
	oled_run_all_tests();
	shell_print(sh, "All tests completed!");

	return 0;
}

/* Shell command: oled brightness */
static int cmd_oled_brightness(const struct shell *sh, size_t argc, char **argv)
{
	int brightness;

	if (argc < 2) {
		shell_print(sh, "Usage: oled brightness <0-255>");
		return -EINVAL;
	}

	brightness = atoi(argv[1]);
	if (brightness < 0 || brightness > 255) {
		shell_print(sh, "Error: brightness must be between 0 and 255");
		return -EINVAL;
	}

	if (display_dev) {
		display_set_contrast(display_dev, (uint8_t)brightness);
		shell_print(sh, "Brightness set to %d", brightness);
	} else {
		shell_print(sh, "Error: display not initialized");
		return -ENODEV;
	}

	return 0;
}

/* Shell command table */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_oled,
	/* Basic operations */
	SHELL_CMD(clear, NULL, "Clear display", cmd_oled_clear),
	SHELL_CMD(fill, NULL, "Fill display (all on)", cmd_oled_fill),
	SHELL_CMD(pattern, NULL, "Show test pattern", cmd_oled_pattern),
	SHELL_CMD(circle, NULL, "Circle animation", cmd_oled_circle),
	SHELL_CMD(pixels, NULL, "Pixel test (checkerboard)", cmd_oled_pixels),
	/* Brightness control */
	SHELL_CMD_ARG(brightness, NULL, "Set brightness (0-255)", cmd_oled_brightness, 1, 1),
	/* Run all tests */
	SHELL_CMD(test, NULL, "Run all tests", cmd_oled_test),
	SHELL_SUBCMD_SET_END
);

/* Root command: oled */
SHELL_CMD_REGISTER(oled, &sub_oled, "OLED display commands", NULL);
