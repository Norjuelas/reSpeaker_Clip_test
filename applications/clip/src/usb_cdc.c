/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include "usb_cdc.h"
#include "at_server.h"
#include "transport.h"

LOG_MODULE_REGISTER(usb_cdc, CONFIG_CLIP_LOG_LEVEL);

/* USB device definition (Seeed VID 0x2886) */
USBD_DEVICE_DEFINE(clip_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(usbd)),
		   0x2886, 0x0020);

/* String descriptors */
USBD_DESC_LANG_DEFINE(clip_lang);
USBD_DESC_MANUFACTURER_DEFINE(clip_mfr, "Seeed");
USBD_DESC_PRODUCT_DEFINE(clip_product, "ReSpeaker Clip");

/* Configuration descriptor (bus-powered, 100mA) */
USBD_DESC_CONFIG_DEFINE(clip_fs_cfg, "Default");
USBD_CONFIGURATION_DEFINE(clip_fs_config, 0, 100, &clip_fs_cfg);

/* CDC ACM UART device */
static const struct device *const cdc_dev =
	DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart));

/* RX state */
static uint8_t rx_line_buf[CONFIG_CLIP_AT_MAX_CMD_LEN];
static uint16_t rx_line_pos;
static bool dtr_set;

/* TX state */
static uint8_t tx_buf[128];
static uint16_t tx_len;
static bool tx_busy;

/* Semaphore for DTR detection */
static K_SEM_DEFINE(dtr_sem, 0, 1);

struct usbd_context *usb_cdc_get_usbd(void)
{
	return &clip_usbd;
}

bool usb_cdc_is_connected(void)
{
	return dtr_set;
}

/* USB message callback */
static void usb_msg_cb(struct usbd_context *const ctx,
		       const struct usbd_msg *msg)
{
	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		uint32_t dtr = 0U;

		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			dtr_set = true;
			k_sem_give(&dtr_sem);
		} else {
			dtr_set = false;
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
		uint32_t baudrate;

		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_BAUD_RATE, &baudrate);
		LOG_INF("CDC baud %u", baudrate);
	}
}

/* Submit accumulated line to AT server */
static void submit_rx_line(uint16_t len)
{
	if (len == 0) {
		return;
	}

	/* Strip trailing \r */
	if (len > 0 && rx_line_buf[len - 1] == '\r') {
		len--;
	}

	if (len == 0) {
		return;
	}

	at_server_submit_cmd(rx_line_buf, len, TRANSPORT_TYPE_USB);
}

/* UART interrupt handler */
static void cdc_irq_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		/* RX processing */
		if (uart_irq_rx_ready(dev)) {
			uint8_t byte;
			int recv = uart_fifo_read(dev, &byte, 1);

			if (recv > 0) {
				if (byte == '\n') {
					submit_rx_line(rx_line_pos);
					rx_line_pos = 0;
				} else if (byte == '\r') {
					/* Ignore standalone \r, handled with \n */
				} else {
					if (rx_line_pos < sizeof(rx_line_buf) - 1) {
						rx_line_buf[rx_line_pos++] = byte;
					}
				}
			}
		}

		/* TX processing */
		if (uart_irq_tx_ready(dev)) {
			if (tx_len > 0) {
				int sent = uart_fifo_fill(dev, tx_buf, tx_len);

				if (sent > 0) {
					tx_len -= sent;
					if (tx_len > 0) {
						memmove(tx_buf, tx_buf + sent,
							tx_len);
					}
				}
			}

			if (tx_len == 0) {
				uart_irq_tx_disable(dev);
				tx_busy = false;
			}
		}
	}
}

int usb_cdc_send_response(const uint8_t *data, uint16_t len)
{
	if (!dtr_set || len == 0) {
		return 0;
	}

	/* Simple blocking write for AT responses (short) */
	for (uint16_t i = 0; i < len; i++) {
		uart_poll_out(cdc_dev, data[i]);
	}

	return len;
}

int usb_cdc_init(void)
{
	int err;

	if (!device_is_ready(cdc_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return -ENODEV;
	}

	/* Add USB descriptors */
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

	/* Add FS configuration */
	err = usbd_add_configuration(&clip_usbd, USBD_SPEED_FS, &clip_fs_config);
	if (err) {
		LOG_ERR("add config: %d", err);
		return err;
	}

	/* Register CDC ACM class only (MSC registered on demand) */
	err = usbd_register_class(&clip_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("CDC ACM class: %d", err);
		return err;
	}

	/* Initialize USB device */
	err = usbd_init(&clip_usbd);
	if (err) {
		LOG_ERR("usbd init: %d", err);
		return err;
	}

	/* Set message callback */
	usbd_msg_register_cb(&clip_usbd, usb_msg_cb);

	/* Enable USB device */
	if (!usbd_can_detect_vbus(&clip_usbd)) {
		err = usbd_enable(&clip_usbd);
		if (err) {
			LOG_ERR("usbd enable: %d", err);
			return err;
		}
	}

	/* Setup UART interrupt for RX */
	uart_irq_callback_set(cdc_dev, cdc_irq_handler);

	LOG_INF("USB CDC ready");
	return 0;
}
