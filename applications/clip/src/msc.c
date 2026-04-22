/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>

#include "msc.h"
#include "usb_cdc.h"
#include "storage.h"
#include "audio.h"
#include "transfer.h"

LOG_MODULE_REGISTER(msc, CONFIG_CLIP_LOG_LEVEL);

/* Map SD card as LUN 0 */
USBD_DEFINE_MSC_LUN(sd_lun, "SD", "Seeed", "Clip SD", "1.00");

static bool msc_active;

int msc_init(void)
{
	/* MSC class is registered on demand by msc_enable().
	 * USB device is owned by usb_cdc module.
	 */
	LOG_INF("MSC ready (disabled)");
	return 0;
}

int msc_enable(void)
{
	struct usbd_context *usbd = usb_cdc_get_usbd();
	int err;

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

	/* Unmount FATFS so USB MSC has exclusive disk access */
	storage_cleanup();

	/* Disable USB, add MSC class, re-enable (brief disconnect) */
	usbd_disable(usbd);

	err = usbd_register_class(usbd, "msc_0", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("MSC class: %d", err);
		usbd_enable(usbd);
		storage_remount();
		return err;
	}

	err = usbd_enable(usbd);
	if (err) {
		LOG_ERR("usbd enable: %d", err);
		usbd_unregister_class(usbd, "msc_0", USBD_SPEED_FS, 1);
		storage_remount();
		return err;
	}

	msc_active = true;
	LOG_INF("MSC enabled");
	return 0;
}

int msc_disable(void)
{
	struct usbd_context *usbd = usb_cdc_get_usbd();

	if (!msc_active) {
		return 0;
	}

	/* Disable USB, remove MSC class, re-enable */
	usbd_disable(usbd);
	usbd_unregister_class(usbd, "msc_0", USBD_SPEED_FS, 1);
	usbd_enable(usbd);

	msc_active = false;

	/* Remount FATFS for application use */
	storage_remount();

	LOG_INF("MSC disabled");
	return 0;
}

bool msc_is_enabled(void)
{
	return msc_active;
}
