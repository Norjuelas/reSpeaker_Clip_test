/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/input/button.h>

#include "button.h"
#include "audio.h"
#include "haptic.h"
#include "display.h"
#include "clip.h"

LOG_MODULE_REGISTER(button, CONFIG_CLIP2_LOG_LEVEL);

/* Button device from device tree */
static const struct device *button_dev = DEVICE_DT_GET(DT_NODELABEL(usr_btn));

/* Callback function and user data */
static button_callback_t button_cb = NULL;
static void *button_user_data = NULL;

/* Work queue for deferred button actions (needs larger stack for audio operations) */
K_THREAD_STACK_DEFINE(button_work_stack, CONFIG_CLIP2_BUTTON_WORK_STACK_SIZE);
static struct k_work_q button_work_q;

/* Work items for different actions */
static struct k_work start_work;
static struct k_work stop_work;
static struct k_work bookmark_work;
static struct k_work status_work;
static struct k_work poweroff_show_work;
static struct k_work poweroff_exec_work;

/* Power-off state tracking */
static atomic_t poweroff_pending = ATOMIC_INIT(0);

/* Forward declarations */
static void button_event_callback(const struct device *dev, enum button_action action);
static void button_start_work_handler(struct k_work *work);
static void button_stop_work_handler(struct k_work *work);
static void button_bookmark_work_handler(struct k_work *work);
static void button_status_work_handler(struct k_work *work);
static void button_poweroff_show_handler(struct k_work *work);
static void button_poweroff_exec_handler(struct k_work *work);

/* Work handlers - run in work queue with larger stack */
static void button_start_work_handler(struct k_work *work)
{
	enum audio_mode mode;
	int err;
	struct clip_context *c = clip_get_context();

	ARG_UNUSED(work);

	/* Check if audio is actually recording */
	if (audio_is_recording()) {
		LOG_WRN("Button: Start work but audio is recording, ignoring");
		return;
	}

	/* Mode mapping: NORMAL=stereo, ENHANCED=mono+DSP */
	mode = (c->config.mode == MODE_NORMAL) ? AUDIO_MODE_STEREO : AUDIO_MODE_MERGE;
	err = audio_start_recording(mode);
	if (err == 0) {
		c->state = CLIP_STATE_RECORDING;
		/* Haptic feedback */
		haptic_play_pattern(HAPTIC_SHORT);
		/* Update display */
		display_post_event(UI_EVENT_REC_START);
		display_set_recording(true, c->config.mode == MODE_ENHANCED);
	} else if (err == -EBUSY) {
		LOG_WRN("Button: Audio module busy (stopping previous recording), ignoring");
	} else {
		LOG_ERR("Button: Failed to start recording: %d", err);
	}
}

static void button_stop_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);
	struct clip_context *c = clip_get_context();

	/* Check if audio is actually recording */
	if (!audio_is_recording()) {
		LOG_WRN("Button: Stop work but audio is not recording, ignoring");
		return;
	}

	/* Stop recording */
	err = audio_stop_recording();
	if (err == 0) {
		/* Update state */
		c->state = CLIP_STATE_IDLE;
		/* Haptic feedback */
		haptic_play_pattern(HAPTIC_SHORT);
		/* Update display */
		display_post_event(UI_EVENT_REC_STOP);
		display_set_recording(false, false);
	} else if (err == -EBUSY) {
		LOG_WRN("Button: Audio module busy (stopping previous recording)");
	} else {
		LOG_ERR("Button: Failed to stop recording: %d", err);
	}
}

static void button_bookmark_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	LOG_INF("Button: Bookmark work handler - adding bookmark");

	err = audio_add_bookmark();
	if (err == 0) {
		LOG_INF("Button: Bookmark added successfully");
		/* Update display - no haptic feedback for bookmark */
		display_post_event(UI_EVENT_MARK);
	} else {
		LOG_ERR("Button: Failed to add bookmark: %d", err);
	}
}

static void button_status_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Show status bar */
	display_post_event(UI_EVENT_STATUS_SHOW);
}

static void button_poweroff_show_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Button: 3s long press - showing power-off screen");
	display_post_event(UI_EVENT_POWER_OFF_SHOW);
	/* Mark that the next BUTTON_RELEASE should execute the power-off */
	atomic_set(&poweroff_pending, 1);
}

static void button_poweroff_exec_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Button released - executing power-off with double haptic");

	/* Play double haptic feedback before powering off */
	haptic_play_pattern(HAPTIC_DOUBLE);

	/* Small delay to let haptic complete */
	k_sleep(K_MSEC(400));

	/* Clear display before shutting down */
	display_post_event(UI_EVENT_TIMEOUT); /* Will turn off display */

	LOG_INF("Entering ship mode (power off)");

	/* Enter npm1300 ship mode */
	const struct device *regulators = DEVICE_DT_GET(DT_NODELABEL(npm1300_regulators));
	if (!device_is_ready(regulators)) {
		LOG_ERR("npm1300 regulators not ready, cannot power off");
		return;
	}

	int ret = regulator_parent_ship_mode(regulators);
	if (ret != 0) {
		LOG_ERR("Failed to enter ship mode: %d", ret);
	}
	/* If the write succeeded the system powers off immediately */
}

/* Button callback - runs in button driver thread with small stack */
/* Only submit work items here, do not call audio functions directly */
static void button_event_callback(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);

	enum clip_state current_state = clip_get_context()->state;
	bool is_recording = audio_is_recording();

	/* Log both state machine and actual audio state for debugging */
	LOG_INF("=== BUTTON EVENT: %d === (state=%d, recording=%d)",
		action, current_state, is_recording);

	/* Log button action name for easier debugging */
	const char *action_name = "UNKNOWN";
	switch (action) {
	case 0: action_name = "LONG_PRESS"; break;
	case 1: action_name = "LONG_PRESS_LEVEL_1"; break;
	case 2: action_name = "LONG_PRESS_LEVEL_2"; break;
	case 3: action_name = "LONG_PRESS_LEVEL_3"; break;
	case 4: action_name = "SINGLE_CLICK"; break;
	case 5: action_name = "DOUBLE_CLICK"; break;
	case 6: action_name = "RELEASE"; break;
	}
	LOG_INF("  Action: %s", action_name);

	switch (action) {
	case BUTTON_SINGLE_CLICK:
		/* Short press (<1s): Show status in IDLE, add bookmark during recording */
		if (current_state == CLIP_STATE_RECORDING) {
			k_work_submit_to_queue(&button_work_q, &bookmark_work);
		} else if (current_state == CLIP_STATE_IDLE) {
			k_work_submit_to_queue(&button_work_q, &status_work);
		} else {
			LOG_DBG("Button: Short press ignored (state=%d)", current_state);
		}
		break;

	case BUTTON_LONG_PRESS:       /* released after holding 1s–3s */
		/* Long press: toggle recording */
		if (audio_is_recording()) {
			k_work_submit_to_queue(&button_work_q, &stop_work);
		} else {
			k_work_submit_to_queue(&button_work_q, &start_work);
		}
		break;

	case BUTTON_LONG_PRESS_LEVEL_1: /* held 3s, auto-fires while button still down */
	case BUTTON_LONG_PRESS_LEVEL_2:
	case BUTTON_LONG_PRESS_LEVEL_3:
		/* 3-second long press: show power-off screen */
		LOG_INF("Button: 3s long press - showing power-off screen");
		k_work_submit_to_queue(&button_work_q, &poweroff_show_work);
		break;

	case BUTTON_RELEASE:
		/* Button released after auto-triggered long press */
		if (atomic_cas(&poweroff_pending, 1, 0)) {
			LOG_INF("Button released - executing power-off");
			k_work_submit_to_queue(&button_work_q, &poweroff_exec_work);
		}
		break;

	case BUTTON_DOUBLE_CLICK:
		/* Double click: Disabled - ignore */
		LOG_DBG("Button: Double-click ignored (feature disabled)");
		break;

	default:
		LOG_WRN("Button: Unknown action: %d", action);
		break;
	}

	/* Call user callback if registered */
	if (button_cb) {
		button_cb(action, button_user_data);
	}
}

int button_init(void)
{
	int err;

	LOG_INF("=== Button Init Starting ===");

	if (!device_is_ready(button_dev)) {
		LOG_ERR("Button device not ready");
		return -ENODEV;
	}

	LOG_INF("Button device ready, initializing...");

	/* Initialize work queue for button actions */
	k_work_queue_start(&button_work_q, button_work_stack,
			   CONFIG_CLIP2_BUTTON_WORK_STACK_SIZE,
			   CONFIG_CLIP2_BUTTON_WORK_PRIORITY, NULL);

	/* Initialize work items */
	k_work_init(&start_work, button_start_work_handler);
	k_work_init(&stop_work, button_stop_work_handler);
	k_work_init(&bookmark_work, button_bookmark_work_handler);
	k_work_init(&status_work, button_status_work_handler);
	k_work_init(&poweroff_show_work, button_poweroff_show_handler);
	k_work_init(&poweroff_exec_work, button_poweroff_exec_handler);

	/* Register callback for all button events */
	err = button_callback_register(button_dev, button_event_callback);
	if (err < 0) {
		LOG_ERR("Failed to register button callback: %d", err);
		return err;
	}

	LOG_INF("Button handler initialized successfully");
	return 0;
}

bool button_is_ready(void)
{
	return device_is_ready(button_dev);
}

int button_register_callback(button_callback_t callback, void *user_data)
{
	if (!callback) {
		return -EINVAL;
	}

	button_cb = callback;
	button_user_data = user_data;

	return 0;
}
