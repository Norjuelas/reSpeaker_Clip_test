/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_NRF70_FW_PROVISION_H
#define CLIP_NRF70_FW_PROVISION_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Ensure the nRF7002 firmware patch is present in its flash partition
 *
 * Copies the patch from the SD card when the partition is empty or holds a
 * different version. Must run before the WiFi interface is brought up: without
 * the patch, net_if_up() fails with -EIO and neither AP nor station mode work.
 *
 * Safe to call on every boot — an already-provisioned partition is detected by
 * its header and left alone, so the SD card is only written once.
 *
 * @return 0 if the partition holds the patch
 * @retval -ENODEV no SD card
 * @retval -ENOENT the patch file is not on the card
 * @retval -EFBIG  the patch does not fit the partition
 */
int nrf70_fw_provision(void);

/** Snapshot of what the patch partition currently holds. */
struct nrf70_fw_status {
	bool provisioned;        /**< false when the partition reads as erased */
	uint32_t signature;      /**< 0xDEAD1EAF for a valid patch */
	uint32_t version;
	uint32_t len;            /**< payload length declared by the header */
	uint32_t partition_size;
	int last_result;         /**< return of the boot provisioning; -EINPROGRESS = never ran */
	bool wrote_this_boot;    /**< true if the partition was written on this boot */
};

/**
 * @brief Read the patch partition header
 *
 * Lets AT+NRF70FW? answer whether provisioning worked without having to catch
 * boot-time logs — which the production snippet does not write to the SD card.
 *
 * @param out Filled in on success
 * @return 0 on success, negative error code on failure
 */
int nrf70_fw_partition_status(struct nrf70_fw_status *out);

#endif /* CLIP_NRF70_FW_PROVISION_H */
