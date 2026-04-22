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
#include "storage.h"
#include "audio.h"
#include "transfer.h"

LOG_MODULE_REGISTER(msc, CONFIG_CLIP_LOG_LEVEL);

/* USB device definition (Seeed VID 0x2886) */
USBD_DEVICE_DEFINE(clip_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(usbd)),
		   0x2886, 0x0020);

/* String descriptors */
USBD_DESC_LANG_DEFINE(clip_lang);
USBD_DESC_MANUFACTURER_DEFINE(clip_mfr, "Seeed");
USBD_DESC_PRODUCT_DEFINE(clip_product, "Clip SD Card");

/* Configuration descriptor (bus-powered, 100mA) */
USBD_DESC_CONFIG_DEFINE(clip_fs_cfg, "Default");
USBD_CONFIGURATION_DEFINE(clip_fs_config, 0, 100, &clip_fs_cfg);

/* Map SD card as LUN 0 */
USBD_DEFINE_MSC_LUN(sd_lun, "SD", "Seeed", "Clip SD", "1.00");

static bool msc_active;

int msc_init(void)
{
	int err;

	err = usbd_add_descriptor(&clip_usbd, &clip_lang);
	if (err) {
		LOG_ERR("lang desc: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&clip_usbd, &clip_mfr);
	if (err) {
		LOG_ERR("mfr desc: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&clip_usbd, &clip_product);
	if (err) {
		LOG_ERR("product desc: %d", err);
		return err;
	}

	err = usbd_add_configuration(&clip_usbd, USBD_SPEED_FS, &clip_fs_config);
	if (err) {
		LOG_ERR("add config: %d", err);
		return err;
	}

	err = usbd_register_all_classes(&clip_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("register classes: %d", err);
		return err;
	}

	err = usbd_init(&clip_usbd);
	if (err) {
		LOG_ERR("usbd init: %d", err);
		return err;
	}

	LOG_INF("USB MSC ready (disabled)");
	return 0;
}

int msc_enable(void)
{
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

	err = usbd_enable(&clip_usbd);
	if (err) {
		LOG_ERR("usbd enable: %d", err);
		/* Remount since MSC failed */
		storage_remount();
		return err;
	}

	msc_active = true;
	LOG_INF("MSC enabled");
	return 0;
}

int msc_disable(void)
{
	if (!msc_active) {
		return 0;
	}

	usbd_disable(&clip_usbd);
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
