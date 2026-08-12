/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_NRF70_FW_PROVISION_H
#define CLIP_NRF70_FW_PROVISION_H

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

#endif /* CLIP_NRF70_FW_PROVISION_H */
