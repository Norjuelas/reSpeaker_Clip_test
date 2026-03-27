/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>

#include "display.h"
#include "icons.h"
#include "audio.h"
#include "ble.h"
#include "clip.h"
#include "transfer.h"

LOG_MODULE_REGISTER(display, CONFIG_CLIP_LOG_LEVEL);

/* =============================================================================
 * Display Configuration
 * ============================================================================= */

#define OLED_WIDTH   88
#define OLED_HEIGHT  48
#define OLED_BUF_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

#define UI_MIRROR_X  1
#define UI_MIRROR_Y  0

/* UI Thread Configuration */
#define DISPLAY_EVENT_QUEUE_SIZE  16
#define DISPLAY_ANIMATION_PERIOD  K_MSEC(50)
#define DISPLAY_STATUS_TIMEOUT_MS  3000
#define DISPLAY_REC_WAVE_TIMEOUT_MS 5000

/* =============================================================================
 * Recording Animation Configuration
 * ============================================================================= */

#define FAST_ANIM_BAR_COUNT       13
#define FAST_ANIM_BAR_WIDTH       1
#define FAST_ANIM_BAR_GAP         4
#define FAST_ANIM_MAX_HEIGHT      10
#define FAST_ANIM_MIN_HEIGHT      2
#define FAST_ANIM_PERIOD          75
#define FAST_ANIM_PHASE_SHIFT     5
#define FAST_ANIM_EDGE_MARGIN     14
#define FAST_ANIM_WAVE_PEAKS      3
#define FAST_ANIM_WAVE_WIDTH      1.0f

#define NORMAL_ANIM_BAR_COUNT     13
#define NORMAL_ANIM_BAR_WIDTH     2
#define NORMAL_ANIM_BAR_GAP       3
#define NORMAL_ANIM_MAX_HEIGHT    12
#define NORMAL_ANIM_MIN_HEIGHT    2
#define NORMAL_ANIM_PERIOD        120
#define NORMAL_ANIM_PHASE_SHIFT   8
#define NORMAL_ANIM_EDGE_MARGIN   14
#define NORMAL_ANIM_WAVE_PEAKS    1
#define NORMAL_ANIM_WAVE_WIDTH    1.0f

/* Dot Circle Animation */
#define DOT_CIRCLE_STABLE_RADIUS  4
#define DOT_CIRCLE_MAX_RADIUS     8
#define DOT_CIRCLE_ANIM_FRAMES    8

/* Mark Animation */
#define MARK_ANIM_FRAMES          15
#define MARK_WHITE_CIRCLE_MAX_RADIUS   6
#define MARK_WHITE_CIRCLE_STABLE_RADIUS 4
#define MARK_BLACK_CIRCLE_MAX_RADIUS   4
#define MARK_BLACK_CIRCLE_STABLE_RADIUS 3
#define MARK_LINE_THICKNESS           2
#define MARK_LINE_STABLE_LENGTH       12
#define MARK_LINE_MAX_LENGTH          14
#define MARK_LINE_OFFSET_FROM_WHITE   2

/* =============================================================================
 * Recording Animation Types
 * ============================================================================= */

typedef enum {
	REC_ANIM_NORMAL,
	REC_ANIM_FAST,
} rec_anim_type_t;

/* =============================================================================
 * Fast Animation Bar State
 * ============================================================================= */

struct fast_anim_bar {
	int8_t phase_offset;
	uint8_t current_height;
};

/* =============================================================================
 * Module State
 * ============================================================================= */

/* Display device */
static const struct device *display_dev = NULL;

/* Display buffer */
static uint8_t display_buffer[OLED_BUF_SIZE];

/* Current UI state */
static enum ui_state g_ui_state = UI_STATE_OFF;

/* Recording animation state */
static rec_anim_type_t g_current_anim_type = REC_ANIM_NORMAL;
static struct fast_anim_bar g_fast_bars[FAST_ANIM_BAR_COUNT];
static uint32_t g_fast_anim_frame = 0;
static bool g_fast_anim_inited = false;

/* Display status */
static struct display_status g_status = {
	.battery_percent = 100,
	.battery_charging = false,
	.ble_connected = false,
	.transferring = false,
};

/* Recording mode */
static bool g_recording = false;
static bool g_enhanced_mode = false;

/* REC_DOT animation tracking */
static bool g_dot_animation_played = false;
static int64_t g_rec_wave_start_ms = 0;
static int64_t g_status_bar_start_ms = 0;

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static void display_thread_fn(void *p1, void *p2, void *p3);
static void display_anim_work_handler(struct k_work *work);
static void display_timeout_work_handler(struct k_work *work);
static void set_ui_state(enum ui_state new_state);
static void render_current_state(void);

/* Event queue */
K_MSGQ_DEFINE(display_event_queue, sizeof(enum ui_event),
		     DISPLAY_EVENT_QUEUE_SIZE, 4);

/* UI thread */
K_THREAD_DEFINE(display_thread, 2048,
		display_thread_fn, NULL, NULL, NULL,
		6, 0, 0);

/* Work for animation timer */
static struct k_work_delayable display_anim_work;

/* Work for timeout */
static struct k_work_delayable display_timeout_work;

/* =============================================================================
 * Pixel Operations
 * ============================================================================= */

static inline void map_xy(int *x, int *y)
{
	if (UI_MIRROR_X) {
		*x = (OLED_WIDTH - 1) - *x;
	}
	if (UI_MIRROR_Y) {
		*y = (OLED_HEIGHT - 1) - *y;
	}
}

static inline void set_pixel_direct(uint8_t *buf, int x, int y)
{
	if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT)
		return;
	buf[(y / 8) * OLED_WIDTH + x] |= (1 << (y % 8));
}

static inline void clear_pixel_direct(uint8_t *buf, int x, int y)
{
	if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT)
		return;
	buf[(y / 8) * OLED_WIDTH + x] &= ~(1 << (y % 8));
}

static void clear_screen(uint8_t *buf)
{
	memset(buf, 0, OLED_BUF_SIZE);
}

/* =============================================================================
 * Recording Animation - Wave with Audio Energy
 * ============================================================================= */

#define GET_ANIM_BAR_COUNT()     (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_BAR_COUNT : NORMAL_ANIM_BAR_COUNT)
#define GET_ANIM_BAR_WIDTH()     (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_BAR_WIDTH : NORMAL_ANIM_BAR_WIDTH)
#define GET_ANIM_BAR_GAP()       (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_BAR_GAP : NORMAL_ANIM_BAR_GAP)
#define GET_ANIM_MAX_HEIGHT()    (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_MAX_HEIGHT : NORMAL_ANIM_MAX_HEIGHT)
#define GET_ANIM_MIN_HEIGHT()    (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_MIN_HEIGHT : NORMAL_ANIM_MIN_HEIGHT)
#define GET_ANIM_PERIOD()        (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_PERIOD : NORMAL_ANIM_PERIOD)
#define GET_ANIM_PHASE_SHIFT()   (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_PHASE_SHIFT : NORMAL_ANIM_PHASE_SHIFT)
#define GET_ANIM_EDGE_MARGIN()   (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_EDGE_MARGIN : NORMAL_ANIM_EDGE_MARGIN)
#define GET_ANIM_WAVE_PEAKS()    (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_WAVE_PEAKS : NORMAL_ANIM_WAVE_PEAKS)
#define GET_ANIM_WAVE_WIDTH()    (g_current_anim_type == REC_ANIM_FAST ? FAST_ANIM_WAVE_WIDTH : NORMAL_ANIM_WAVE_WIDTH)

static int fast_anim_wave(int phase, int period, int min_val, int max_val)
{
	int normalized = phase % period;
	float angle = (float)normalized * 6.28318f / (float)period;

	int wave_peaks = GET_ANIM_WAVE_PEAKS();
	float wave_width = GET_ANIM_WAVE_WIDTH();
	float scaled_angle = (float)wave_peaks * angle * wave_width;

	float factor = (1.0f - (float)cos(scaled_angle)) / 2.0f;

	return min_val + (int)(factor * (max_val - min_val));
}

static void fast_anim_init(void)
{
	static rec_anim_type_t last_anim_type = REC_ANIM_NORMAL;

	if (g_fast_anim_inited && (last_anim_type == g_current_anim_type)) {
		return;
	}

	g_fast_anim_inited = true;
	g_fast_anim_frame = 0;
	last_anim_type = g_current_anim_type;

	int bar_count = GET_ANIM_BAR_COUNT();
	int phase_shift = GET_ANIM_PHASE_SHIFT();
	int min_height = GET_ANIM_MIN_HEIGHT();

	for (int i = 0; i < bar_count; i++) {
		g_fast_bars[i].phase_offset = (int8_t)(i * phase_shift);
		g_fast_bars[i].current_height = (uint8_t)min_height;
	}
}

static void fast_anim_step(void)
{
	g_fast_anim_frame++;

	int bar_count = GET_ANIM_BAR_COUNT();
	int period = GET_ANIM_PERIOD();
	int min_height = GET_ANIM_MIN_HEIGHT();
	int max_height = GET_ANIM_MAX_HEIGHT();

	/* Get audio energy level (0-10) for scaling */
	int energy = audio_get_energy_level();
	if (energy < 0) energy = 0;
	if (energy > 10) energy = 10;

	int scaled_max = min_height + ((max_height - min_height) * energy / 10);

	for (int i = 0; i < bar_count; i++) {
		int current_phase = (int)g_fast_anim_frame + g_fast_bars[i].phase_offset;
		int new_height = fast_anim_wave(current_phase, period, min_height, scaled_max);
		g_fast_bars[i].current_height = (uint8_t)new_height;
	}
}

static void draw_fast_animation(uint8_t *buf)
{
	const int x_mid = OLED_WIDTH / 2;
	const int y_mid = OLED_HEIGHT / 2;

	int bar_count = GET_ANIM_BAR_COUNT();
	int bar_width = GET_ANIM_BAR_WIDTH();
	int bar_gap = GET_ANIM_BAR_GAP();
	int edge_margin = GET_ANIM_EDGE_MARGIN();

	int total_width = (bar_count * bar_width) + ((bar_count - 1) * bar_gap);
	int available_width = OLED_WIDTH - (2 * edge_margin);
	int start_x = edge_margin + ((available_width - total_width) / 2);

	for (int i = 0; i < bar_count; i++) {
		int bar_x = start_x + i * (bar_width + bar_gap);
		int bar_height = g_fast_bars[i].current_height;

		for (int dx = 0; dx < bar_width; dx++) {
			int x = bar_x + dx;
			if (x < 0 || x >= OLED_WIDTH) continue;

			for (int dy = 0; dy <= bar_height; dy++) {
				if (y_mid - dy >= 0) {
					set_pixel_direct(buf, x, y_mid - dy);
				}
				if (y_mid + dy < OLED_HEIGHT && dy > 0) {
					set_pixel_direct(buf, x, y_mid + dy);
				}
			}
		}
	}
}

static void render_recording_wave(uint8_t *buf)
{
	clear_screen(buf);
	draw_fast_animation(buf);
}

/* =============================================================================
 * Dot Circle Animation
 * ============================================================================= */

static void draw_circle_mark(uint8_t *buf, int radius, float scale)
{
	const int x_mid = OLED_WIDTH / 2;
	const int y_mid = OLED_HEIGHT / 2;

	int scaled_radius = (int)(radius * scale);
	if (scaled_radius < 1) scaled_radius = 1;

	for (int dy = -scaled_radius; dy <= scaled_radius; dy++) {
		for (int dx = -scaled_radius; dx <= scaled_radius; dx++) {
			int x = x_mid + dx;
			int y = y_mid + dy;

			if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
				continue;
			}

			int dist_sq = dx * dx + dy * dy;
			int radius_sq = scaled_radius * scaled_radius;

			if (dist_sq <= radius_sq) {
				set_pixel_direct(buf, x, y);
			}
		}
	}
}

static void render_dot_circle(uint8_t *buf, int frame)
{
	clear_screen(buf);

	/* Calculate current radius: stable → max → stable */
	const int total_frames = DOT_CIRCLE_ANIM_FRAMES;
	float phase = (float)frame / (float)total_frames;
	float sine_val = (1.0f - (float)cosf(phase * 6.28318f)) / 2.0f;

	int current_radius = DOT_CIRCLE_STABLE_RADIUS +
		(int)((DOT_CIRCLE_MAX_RADIUS - DOT_CIRCLE_STABLE_RADIUS) * sine_val);

	draw_circle_mark(buf, current_radius, 1.0f);
}

/* =============================================================================
 * Mark Animation
 * ============================================================================= */

static void draw_black_circle(uint8_t *buf, int radius, float scale)
{
	const int x_mid = OLED_WIDTH / 2;
	const int y_mid = OLED_HEIGHT / 2;

	int scaled_radius = (int)(radius * scale);
	if (scaled_radius < 0) scaled_radius = 0;

	for (int dy = -scaled_radius; dy <= scaled_radius; dy++) {
		for (int dx = -scaled_radius; dx <= scaled_radius; dx++) {
			int x = x_mid + dx;
			int y = y_mid + dy;

			if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
				continue;
			}

			int dist_sq = dx * dx + dy * dy;
			int radius_sq = scaled_radius * scaled_radius;

			if (dist_sq <= radius_sq) {
				clear_pixel_direct(buf, x, y);
			}
		}
	}
}

static void draw_vertical_lines(uint8_t *buf, int white_circle_radius,
				int line_length, int thickness, int offset_pixels)
{
	const int x_mid = OLED_WIDTH / 2;
	const int y_mid = OLED_HEIGHT / 2;

	int top_start_y = y_mid - white_circle_radius - offset_pixels;
	int bottom_start_y = y_mid + white_circle_radius + offset_pixels;

	for (int i = 0; i < line_length; i++) {
		for (int t = 0; t < thickness; t++) {
			int x = x_mid - (thickness / 2) + t;
			int y = top_start_y - i;
			if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < OLED_HEIGHT) {
				set_pixel_direct(buf, x, y);
			}
		}
	}

	for (int i = 0; i < line_length; i++) {
		for (int t = 0; t < thickness; t++) {
			int x = x_mid - (thickness / 2) + t;
			int y = bottom_start_y + i;
			if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < OLED_HEIGHT) {
				set_pixel_direct(buf, x, y);
			}
		}
	}
}

static float get_animation_progress(int frame, int total_frames)
{
	float phase = (float)frame / (float)total_frames;
	float sine_val = (1.0f - (float)cosf(phase * 6.28318f)) / 2.0f;
	return sine_val;
}

static void render_mark_animation(uint8_t *buf, int frame)
{
	clear_screen(buf);

	float anim_value = get_animation_progress(frame, MARK_ANIM_FRAMES);

	int white_radius = MARK_WHITE_CIRCLE_STABLE_RADIUS +
		(int)((MARK_WHITE_CIRCLE_MAX_RADIUS - MARK_WHITE_CIRCLE_STABLE_RADIUS) * anim_value);
	draw_circle_mark(buf, white_radius, 1.0f);

	int black_radius;
	if (anim_value <= 0.5f) {
		black_radius = (int)(MARK_BLACK_CIRCLE_MAX_RADIUS * (anim_value * 2.0f));
	} else {
		float contract = 1.0f - ((anim_value - 0.5f) * 2.0f);
		black_radius = MARK_BLACK_CIRCLE_STABLE_RADIUS +
			(int)((MARK_BLACK_CIRCLE_MAX_RADIUS - MARK_BLACK_CIRCLE_STABLE_RADIUS) * contract);
	}
	draw_black_circle(buf, black_radius, 1.0f);

	int line_length = MARK_LINE_STABLE_LENGTH +
		(int)((MARK_LINE_MAX_LENGTH - MARK_LINE_STABLE_LENGTH) * anim_value);
	draw_vertical_lines(buf, white_radius, line_length, MARK_LINE_THICKNESS,
			    MARK_LINE_OFFSET_FROM_WHITE);
}

/* Mark animation frame counter */
static int g_mark_frame = 0;

/* =============================================================================
 * Pause Icon
 * ============================================================================= */

static void render_pause_icon(uint8_t *buf)
{
	for (int y = 17; y < 31; y++) {
		for (int x = 37; x < 40; x++) {
			set_pixel_direct(buf, x, y);
		}
		for (int x = 48; x < 51; x++) {
			set_pixel_direct(buf, x, y);
		}
	}
}

/* =============================================================================
 * Display Flush
 * ============================================================================= */

static void flush_display(void)
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

/* =============================================================================
 * Status Bar Rendering
 * ============================================================================= */

/* 6x12 font for battery percentage (0-9) */
static const uint8_t digit_6x12[10][12] = {
	{0x00, 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x88, 0x70, 0x00, 0x00, 0x00}, /* 0 */
	{0x00, 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00, 0x00, 0x00}, /* 1 */
	{0x00, 0x70, 0x88, 0x08, 0x08, 0x70, 0x80, 0x80, 0xF8, 0x00, 0x00, 0x00}, /* 2 */
	{0x00, 0x70, 0x88, 0x08, 0x30, 0x08, 0x08, 0x88, 0x70, 0x00, 0x00, 0x00}, /* 3 */
	{0x00, 0x80, 0x80, 0x90, 0x90, 0x90, 0xF8, 0x10, 0x10, 0x00, 0x00, 0x00}, /* 4 */
	{0x00, 0xF8, 0x80, 0x80, 0xF0, 0x08, 0x08, 0x08, 0xF0, 0x00, 0x00, 0x00}, /* 5 */
	{0x00, 0x70, 0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0x70, 0x00, 0x00, 0x00}, /* 6 */
	{0x00, 0xF8, 0x88, 0x08, 0x10, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00}, /* 7 */
	{0x00, 0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x88, 0x70, 0x00, 0x00, 0x00}, /* 8 */
	{0x00, 0x70, 0x88, 0x88, 0x88, 0x78, 0x08, 0x08, 0x70, 0x00, 0x00, 0x00}, /* 9 */
};

/* 8x8 percent sign */
static const uint8_t percent_8x8[8] = {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00};

static void draw_digit(uint8_t *buf, char c, int x, int y)
{
	if (c >= '0' && c <= '9') {
		int digit = c - '0';
		const uint8_t *bitmap = digit_6x12[digit];
		for (int row = 0; row < 12; row++) {
			uint8_t row_data = bitmap[row];
			for (int col = 0; col < 6; col++) {
				if (row_data & (0x80 >> col)) {
					set_pixel_direct(buf, x + col, y + row);
				}
			}
		}
	}
}

static void draw_percent(uint8_t *buf, int x, int y)
{
	for (int row = 0; row < 8; row++) {
		uint8_t row_data = percent_8x8[row];
		for (int col = 0; col < 8; col++) {
			if (row_data & (1 << (7 - col))) {
				set_pixel_direct(buf, x + col, y + row);
			}
		}
	}
}

/**
 * @brief Draw battery icon based on charging status and level
 */
static void draw_battery_by_level(uint8_t *buf, int x, int y, uint8_t percent, bool charging)
{
	if (charging) {
		const uint8_t *bmp = icon_get_bitmap(ICON_BATTERY_CHARGING, NULL, NULL);
		if (bmp) {
			icon_draw_bitmap(buf, x, y, bmp, ICON_WIDTH, ICON_HEIGHT);
		}
		return;
	}

	/* Draw empty battery outline */
	const uint8_t *outline = icon_get_bitmap(ICON_BATTERY_LOW, NULL, NULL);
	if (outline) {
		icon_draw_bitmap(buf, x, y, outline, ICON_WIDTH, ICON_HEIGHT);
	}

	/* Fill bar: icon fill area is rows 4–12 (9 rows) × cols 6–9 (4 px) */
	int fill_rows = ((int)percent * 9 + 50) / 100;
	if (fill_rows > 9) fill_rows = 9;
	if (percent > 0 && fill_rows == 0) fill_rows = 1;

	for (int row = 12; row >= (13 - fill_rows); row--) {
		for (int col = 6; col <= 9; col++) {
			set_pixel_direct(buf, x + col, y + row);
		}
	}
}

/**
 * @brief Draw BLE connection icon
 */
static void draw_ble_icon(uint8_t *buf, int x, int y, bool connected)
{
	if (connected) {
		const uint8_t *bitmap = icon_get_bitmap(ICON_BLE_CONNECTED, NULL, NULL);
		if (bitmap) {
			icon_draw_bitmap(buf, x, y, bitmap, ICON_WIDTH, ICON_HEIGHT);
		}
	}
}

static void render_status_bar(uint8_t *buf)
{
	clear_screen(buf);

	/* Battery icon at (4, 16) */
	draw_battery_by_level(buf, 4, 16, g_status.battery_percent, g_status.battery_charging);

	/* Battery percentage text */
	char pct_str[8];
	snprintk(pct_str, sizeof(pct_str), "%u", g_status.battery_percent);
	int digit_x = 22;
	int digit_y = 20;
	for (const char *p = pct_str; *p && digit_x < OLED_WIDTH - 6; p++) {
		draw_digit(buf, *p, digit_x, digit_y);
		digit_x += 7;
	}

	/* Percent symbol */
	draw_percent(buf, digit_x + 1, digit_y);

	/* BLE icon at (68, 17) */
	if (g_status.ble_connected) {
		draw_ble_icon(buf, 68, 17, true);
	}
}

/* =============================================================================
 * Power Off Screen
 * ============================================================================= */

static void render_power_off(uint8_t *buf)
{
	clear_screen(buf);

	/* 6x12 font for text */
	static const uint8_t font_6x12_upper[26][12] = {
		/* A */
		{0x00, 0x00, 0x08, 0x1C, 0x24, 0x44, 0x7C, 0x44, 0x44, 0x44, 0x00, 0x00},
		/* B */
		{0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x44, 0x44, 0x44, 0x78, 0x00, 0x00},
		/* C */
		{0x00, 0x00, 0x38, 0x44, 0x40, 0x40, 0x40, 0x40, 0x44, 0x38, 0x00, 0x00},
		/* D */
		{0x00, 0x00, 0x70, 0x48, 0x44, 0x44, 0x44, 0x44, 0x48, 0x70, 0x00, 0x00},
		/* E */
		{0x00, 0x00, 0x7C, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x7C, 0x00, 0x00},
		/* F */
		{0x00, 0x00, 0x7C, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00},
		/* L */
		{0x00, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7C, 0x00, 0x00},
		/* O */
		{0x00, 0x00, 0x38, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00, 0x00},
		/* P */
		{0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00},
		/* R */
		{0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x50, 0x48, 0x44, 0x44, 0x00, 0x00},
		/* S */
		{0x00, 0x00, 0x3C, 0x40, 0x40, 0x38, 0x04, 0x04, 0x44, 0x38, 0x00, 0x00},
		/* T */
		{0x00, 0x00, 0x7C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00},
		/* V */
		{0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x44, 0x28, 0x28, 0x10, 0x00, 0x00},
	};

	static const uint8_t font_6x12_lower[20][12] = {
		/* a */
		{0x00, 0x00, 0x00, 0x00, 0x38, 0x04, 0x3C, 0x44, 0x44, 0x3C, 0x00, 0x00},
		/* e */
		{0x00, 0x00, 0x00, 0x00, 0x38, 0x44, 0x7C, 0x40, 0x44, 0x38, 0x00, 0x00},
		/* f */
		{0x00, 0x00, 0x0C, 0x10, 0x10, 0x38, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00},
		/* h */
		{0x00, 0x00, 0x40, 0x40, 0x78, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00},
		/* i */
		{0x00, 0x00, 0x00, 0x10, 0x00, 0x30, 0x10, 0x10, 0x10, 0x38, 0x00, 0x00},
		/* n */
		{0x00, 0x00, 0x00, 0x00, 0x78, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00},
		/* o */
		{0x00, 0x00, 0x00, 0x00, 0x38, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00, 0x00},
		/* p */
		{0x00, 0x00, 0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x40, 0x40, 0x40, 0x00},
		/* s */
		{0x00, 0x00, 0x00, 0x00, 0x3C, 0x40, 0x38, 0x04, 0x44, 0x38, 0x00, 0x00},
		/* t */
		{0x00, 0x00, 0x20, 0x20, 0x78, 0x20, 0x20, 0x20, 0x20, 0x1C, 0x00, 0x00},
	};

	auto void draw_char(char c, int x, int y) {
		const uint8_t *font_data;
		if (c >= 'A' && c <= 'Z') {
			font_data = font_6x12_upper[c - 'A'];
		} else if (c >= 'a' && c <= 'z') {
			font_data = font_6x12_lower[c - 'a'];
		} else {
			return;
		}
		for (int row = 0; row < 12; row++) {
			uint8_t row_data = font_data[row];
			for (int col = 0; col < 6; col++) {
				if (row_data & (0x80 >> col)) {
					set_pixel_direct(buf, x + col, y + row);
				}
			}
		}
	}

	const char *line1 = "Release to";
	const char *line2 = "Power Off";
	int x1 = (OLED_WIDTH - 60) / 2;
	int x2 = (OLED_WIDTH - 54) / 2;
	int y1 = (OLED_HEIGHT - 28) / 2;
	int y2 = y1 + 12 + 4;

	int x = x1;
	for (const char *p = line1; *p; p++) {
		draw_char(*p, x, y1);
		x += 6;
	}
	x = x2;
	for (const char *p = line2; *p; p++) {
		draw_char(*p, x, y2);
		x += 6;
	}
}

/* =============================================================================
 * Pairing Guide Page
 * ============================================================================= */

/**
 * @brief 6x12 font for pairing guide (uppercase and lowercase)
 */
static const uint8_t font_6x12_pg[52][12] = {
	/* A */
	{0x00, 0x00, 0x08, 0x1C, 0x24, 0x44, 0x7C, 0x44, 0x44, 0x44, 0x00, 0x00},
	/* B */
	{0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x44, 0x44, 0x44, 0x78, 0x00, 0x00},
	/* C */
	{0x00, 0x00, 0x38, 0x44, 0x40, 0x40, 0x40, 0x40, 0x44, 0x38, 0x00, 0x00},
	/* D */
	{0x00, 0x00, 0x70, 0x48, 0x44, 0x44, 0x44, 0x44, 0x48, 0x70, 0x00, 0x00},
	/* E */
	{0x00, 0x00, 0x7C, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x7C, 0x00, 0x00},
	/* F */
	{0x00, 0x00, 0x7C, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00},
	/* L */
	{0x00, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7C, 0x00, 0x00},
	/* O */
	{0x00, 0x00, 0x38, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00, 0x00},
	/* P */
	{0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00},
	/* R */
	{0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x50, 0x48, 0x44, 0x44, 0x00, 0x00},
	/* S */
	{0x00, 0x00, 0x3C, 0x40, 0x40, 0x38, 0x04, 0x04, 0x44, 0x38, 0x00, 0x00},
	/* a */
	{0x00, 0x00, 0x00, 0x00, 0x38, 0x04, 0x3C, 0x44, 0x44, 0x3C, 0x00, 0x00},
	/* e */
	{0x00, 0x00, 0x00, 0x00, 0x38, 0x44, 0x7C, 0x40, 0x44, 0x38, 0x00, 0x00},
	/* h */
	{0x00, 0x00, 0x40, 0x40, 0x78, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00},
	/* i */
	{0x00, 0x00, 0x00, 0x10, 0x00, 0x30, 0x10, 0x10, 0x10, 0x38, 0x00, 0x00},
	/* n */
	{0x00, 0x00, 0x00, 0x00, 0x78, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00},
	/* o */
	{0x00, 0x00, 0x00, 0x00, 0x38, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00, 0x00},
	/* p */
	{0x00, 0x00, 0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x40, 0x40, 0x40, 0x00},
	/* r */
	{0x00, 0x00, 0x00, 0x00, 0x3C, 0x40, 0x38, 0x04, 0x44, 0x38, 0x00, 0x00},
	/* t */
	{0x00, 0x00, 0x20, 0x20, 0x78, 0x20, 0x20, 0x20, 0x20, 0x1C, 0x00, 0x00},
};

static void render_pairing_guide(uint8_t *buf)
{
	clear_screen(buf);

	/* Draw PHONE icon on the right, top row */
	int phone_x = OLED_WIDTH - ICON_WIDTH - 2;
	int phone_y = 0;
	const uint8_t *phone_bitmap = icon_get_bitmap(ICON_PHONE, NULL, NULL);
	if (phone_bitmap) {
		icon_draw_bitmap(buf, phone_x, phone_y, phone_bitmap, ICON_WIDTH, ICON_HEIGHT);
	}

	/* Get BLE device name */
	const char *device_name = "Clip ----";  /* Fallback */

	/* Draw device name on the left, top row */
	int name_x = 2;
	int name_y = 2;
	int x = name_x;
	for (const char *p = device_name; *p && x < OLED_WIDTH - 6; p++) {
		/* Only show first 8 chars to fit with phone icon */
		if (x < phone_x - 2) {
			int idx = (*p - 'A');
			if (idx >= 0 && idx < 26) {
				const uint8_t *font_data = font_6x12_pg[idx];
				for (int row = 0; row < 12; row++) {
					uint8_t row_data = font_data[row];
					for (int col = 0; col < 6; col++) {
						if (row_data & (0x80 >> col)) {
							set_pixel_direct(buf, x + col, name_y + row);
						}
					}
				}
			}
		}
		x += 6;
	}

	/* Draw "Open App" centered below */
	const char *prompt = "Open App";
	int prompt_len = strlen(prompt);
	int prompt_width = prompt_len * 6;
	int prompt_x = (OLED_WIDTH - prompt_width) / 2;
	int prompt_y = name_y + 12 + 6;

	x = prompt_x;
	for (const char *p = prompt; *p; p++) {
		int idx = (*p - 'A');
		if (idx >= 0 && idx < 26) {
			const uint8_t *font_data = font_6x12_pg[idx];
			for (int row = 0; row < 12; row++) {
				uint8_t row_data = font_data[row];
				for (int col = 0; col < 6; col++) {
					if (row_data & (0x80 >> col)) {
						set_pixel_direct(buf, x + col, prompt_y + row);
					}
				}
			}
		}
		x += 6;
	}
}

/* =============================================================================
 * State Machine
 * ============================================================================= */

static void set_ui_state(enum ui_state new_state)
{
	if (g_ui_state != new_state) {
		LOG_DBG("UI state: %d -> %d", g_ui_state, new_state);

		/* Handle display blanking on state transitions */
		if (new_state == UI_STATE_OFF) {
			/* Entering OFF state - blank display (low power mode) */
			if (display_dev) {
				display_blanking_on(display_dev);
			}
		} else {
			/* Any non-OFF state - ensure display is unblanked */
			if (display_dev) {
				display_blanking_off(display_dev);
			}
		}

		/* Cancel any pending animation work to prevent duplicate execution */
		k_work_cancel_delayable(&display_anim_work);

		g_ui_state = new_state;

		/* Reset animation counters */
		g_mark_frame = 0;

		/* Initialize dot animation on first entry to REC_DOT */
		if (new_state == UI_STATE_REC_WAVE) {
			g_dot_animation_played = false;
			g_rec_wave_start_ms = k_uptime_get();
		}

		/* Initialize status bar timeout on entry */
		if (new_state == UI_STATE_STATUS_BAR) {
			g_status_bar_start_ms = k_uptime_get();
		}
	}
}

static void handle_event(enum ui_event event)
{
	switch (event) {
	case UI_EVENT_REC_START:
		g_recording = true;
		/* Set animation type based on mode */
		g_current_anim_type = g_enhanced_mode ? REC_ANIM_FAST : REC_ANIM_NORMAL;
		fast_anim_init();
		set_ui_state(UI_STATE_REC_WAVE);
		/* Schedule transition to dot animation */
		k_work_schedule(&display_timeout_work, K_MSEC(DISPLAY_REC_WAVE_TIMEOUT_MS));
		break;

	case UI_EVENT_REC_STOP:
		g_recording = false;
		k_work_cancel_delayable(&display_timeout_work);
		set_ui_state(UI_STATE_STATUS_BAR);
		k_work_schedule(&display_timeout_work, K_MSEC(DISPLAY_STATUS_TIMEOUT_MS));
		break;

	case UI_EVENT_REC_PAUSE:
		set_ui_state(UI_STATE_PAUSED);
		break;

	case UI_EVENT_REC_RESUME:
		if (g_recording) {
			g_current_anim_type = g_enhanced_mode ? REC_ANIM_FAST : REC_ANIM_NORMAL;
			set_ui_state(UI_STATE_REC_DOT);
		}
		break;

	case UI_EVENT_MARK:
		g_mark_frame = 0;
		set_ui_state(UI_STATE_MARKING);
		break;

	case UI_EVENT_STATUS_SHOW:
		set_ui_state(UI_STATE_STATUS_BAR);
		k_work_schedule(&display_timeout_work, K_MSEC(DISPLAY_STATUS_TIMEOUT_MS));
		break;

	case UI_EVENT_BONDED:
		if (g_ui_state == UI_STATE_PAIRING_GUIDE) {
			set_ui_state(UI_STATE_STATUS_BAR);
			k_work_schedule(&display_timeout_work, K_MSEC(DISPLAY_STATUS_TIMEOUT_MS));
		}
		break;

	case UI_EVENT_POWER_OFF_SHOW:
		set_ui_state(UI_STATE_POWER_OFF);
		break;

	case UI_EVENT_TIMEOUT:
		if (g_ui_state == UI_STATE_STATUS_BAR) {
			set_ui_state(UI_STATE_OFF);
		} else if (g_ui_state == UI_STATE_REC_WAVE) {
			set_ui_state(UI_STATE_REC_DOT);
		}
		break;

	default:
		break;
	}
}

static void render_current_state(void)
{
	switch (g_ui_state) {
	case UI_STATE_OFF:
		clear_screen(display_buffer);
		flush_display();
		break;

	case UI_STATE_STATUS_BAR:
		render_status_bar(display_buffer);
		flush_display();
		/* Check 3s timeout - transition to OFF */
		if (k_uptime_get() - g_status_bar_start_ms >= DISPLAY_STATUS_TIMEOUT_MS) {
			set_ui_state(UI_STATE_OFF);
		}
		break;

	case UI_STATE_REC_WAVE:
		fast_anim_step();
		render_recording_wave(display_buffer);
		flush_display();
		/* After 5s, transition to DOT with animation */
		if (k_uptime_get() - g_rec_wave_start_ms >= DISPLAY_REC_WAVE_TIMEOUT_MS) {
			set_ui_state(UI_STATE_REC_DOT);
		}
		break;

	case UI_STATE_REC_DOT:
		if (!g_dot_animation_played) {
			/* Play dot animation (8 frames) */
			for (int f = 0; f < DOT_CIRCLE_ANIM_FRAMES; f++) {
				render_dot_circle(display_buffer, f);
				flush_display();
				k_sleep(K_MSEC(60));
			}
			g_dot_animation_played = true;
			/* Last frame is already the stable frame, no need to re-render */
		} else {
			/* Show stable frame */
			render_dot_circle(display_buffer, DOT_CIRCLE_ANIM_FRAMES - 1);
			flush_display();
		}
		break;

	case UI_STATE_MARKING:
		if (g_mark_frame == 0) {
			/* Play mark animation synchronously (15 frames @ 10ms = 150ms) */
			for (int f = 0; f < MARK_ANIM_FRAMES; f++) {
				render_mark_animation(display_buffer, f);
				flush_display();
				k_sleep(K_MSEC(10));
			}
			g_mark_frame = MARK_ANIM_FRAMES;
			/* Transition back to recording */
			if (g_recording) {
				set_ui_state(UI_STATE_REC_DOT);
			} else {
				set_ui_state(UI_STATE_STATUS_BAR);
				k_work_schedule(&display_timeout_work, K_MSEC(DISPLAY_STATUS_TIMEOUT_MS));
			}
		}
		/* Don't render again - animation is complete and state has changed */
		break;

	case UI_STATE_PAUSED:
		clear_screen(display_buffer);
		render_pause_icon(display_buffer);
		flush_display();
		break;

	case UI_STATE_POWER_OFF:
		render_power_off(display_buffer);
		flush_display();
		break;

	case UI_STATE_PAIRING_GUIDE:
		render_pairing_guide(display_buffer);
		flush_display();
		break;

	default:
		break;
	}
}

/* =============================================================================
 * Work Handlers
 * ============================================================================= */

static void display_anim_work_handler(struct k_work *work)
{
	render_current_state();

	/* Reschedule animation */
	if (g_ui_state == UI_STATE_REC_WAVE ||
	    g_ui_state == UI_STATE_REC_DOT ||
	    g_ui_state == UI_STATE_MARKING) {
		k_work_schedule(&display_anim_work, DISPLAY_ANIMATION_PERIOD);
	}
}

static void display_timeout_work_handler(struct k_work *work)
{
	display_post_event(UI_EVENT_TIMEOUT);
}

/* =============================================================================
 * UI Thread
 * ============================================================================= */

static void display_thread_fn(void *p1, void *p2, void *p3)
{
	enum ui_event event;

	LOG_INF("Display thread started");

	while (true) {
		/* Wait for events */
		if (k_msgq_get(&display_event_queue, &event, K_FOREVER) == 0) {
			LOG_DBG("Display event: %d", event);
			handle_event(event);
			render_current_state();

			/* Start/stop animation work */
			if (g_ui_state == UI_STATE_REC_WAVE ||
			    g_ui_state == UI_STATE_REC_DOT ||
			    g_ui_state == UI_STATE_MARKING) {
				k_work_schedule(&display_anim_work, DISPLAY_ANIMATION_PERIOD);
			} else {
				k_work_cancel_delayable(&display_anim_work);
			}
		}
	}
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int display_init(void)
{
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!display_dev || !device_is_ready(display_dev)) {
		LOG_WRN("Display device not ready");
		display_dev = NULL;
		return -ENODEV;
	}

	LOG_INF("Display initialized: %dx%d", OLED_WIDTH, OLED_HEIGHT);

	/* Ensure display is unblanked on init */
	display_blanking_off(display_dev);

	/* Initialize work items */
	k_work_init_delayable(&display_anim_work, display_anim_work_handler);
	k_work_init_delayable(&display_timeout_work, display_timeout_work_handler);

	/* Initial state:
	 *   bonded     -> show status bar 3s then OFF
	 *   not bonded -> show pairing guide immediately
	 */
	if (ble_is_bonded()) {
		set_ui_state(UI_STATE_STATUS_BAR);
		g_status_bar_start_ms = k_uptime_get();
		render_status_bar(display_buffer);
		flush_display();
		/* Schedule 3-second timeout to turn off display */
		k_work_schedule(&display_timeout_work, K_MSEC(DISPLAY_STATUS_TIMEOUT_MS));
		LOG_INF("Initial state: STATUS_BAR (bonded)");
	} else {
		set_ui_state(UI_STATE_PAIRING_GUIDE);
		render_pairing_guide(display_buffer);
		flush_display();
		LOG_INF("Initial state: PAIRING_GUIDE");
	}

	return 0;
}

bool display_is_ready(void)
{
	return display_dev != NULL;
}

int display_post_event(enum ui_event event)
{
	return k_msgq_put(&display_event_queue, &event, K_NO_WAIT);
}

int display_update_status(const struct display_status *status)
{
	if (!status) {
		return -EINVAL;
	}

	memcpy(&g_status, status, sizeof(g_status));

	/* Update display if in status bar state */
	if (g_ui_state == UI_STATE_STATUS_BAR) {
		render_status_bar(display_buffer);
		flush_display();
	}

	return 0;
}

int display_set_recording(bool recording, bool enhanced_mode)
{
	g_recording = recording;
	g_enhanced_mode = enhanced_mode;

	if (recording) {
		g_current_anim_type = enhanced_mode ? REC_ANIM_FAST : REC_ANIM_NORMAL;
		fast_anim_init();
	}

	return 0;
}

int display_turn_off(void)
{
	clear_screen(display_buffer);
	flush_display();
	set_ui_state(UI_STATE_OFF);
	return 0;
}
