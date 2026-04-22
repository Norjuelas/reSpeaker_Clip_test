/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MSC is always exposed via USB alongside CDC ACM.
 * AT+MSC=on unmounts FATFS (exclusive host access, recording blocked).
 * AT+MSC=off remounts FATFS (app access restored).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "msc.h"
#include "storage.h"
#include "audio.h"
#include "transfer.h"

LOG_MODULE_REGISTER(msc, CONFIG_CLIP_LOG_LEVEL);

static bool msc_active;

int msc_init(void)
{
	/* MSC LUN and USB class are registered in usb_cdc_init().
	 * Drive appears automatically when USB is connected.
	 */
	LOG_INF("MSC ready");
	return 0;
}

int msc_enable(void)
{
	if (msc_active) {
		return 0;
	}

	if (audio_is_recording()) {
		LOG_WRN("cannot enable MSC while recording");
		return -EBUSY;
	}

	if (transfer_is_active()) {
		transfer_cancel();
	}

	/* Unmount FATFS so USB host has exclusive disk access */
	storage_cleanup();

	msc_active = true;
	LOG_INF("MSC enabled (FATFS unmounted)");
	return 0;
}

int msc_disable(void)
{
	if (!msc_active) {
		return 0;
	}

	/* Remount FATFS for application use */
	storage_remount();

	msc_active = false;
	LOG_INF("MSC disabled (FATFS remounted)");
	return 0;
}

bool msc_is_enabled(void)
{
	return msc_active;
}
