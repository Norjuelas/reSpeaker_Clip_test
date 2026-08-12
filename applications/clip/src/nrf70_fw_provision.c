/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Writes the nRF7002 firmware patch into its flash partition from the SD card.
 *
 * The patch lives in nrf70_fw_partition instead of inside the application image,
 * which frees ~87KB of internal flash. Normally MCUboot would own that partition
 * and update it over the air, but the bootloader already programmed on every
 * unit handles two images and cannot be replaced without SWD through a sealed
 * enclosure. So the application fills the partition itself.
 *
 * Runs before WiFi is brought up. Without it, net_if_up() fails with -EIO and
 * neither AP nor station mode work at all.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>

#include "nrf70_fw_provision.h"
#include "storage.h"

LOG_MODULE_REGISTER(nrf70_fw, CONFIG_CLIP_LOG_LEVEL);

/* The partition the driver actually reads.
 *
 * Not nrf70_fw_partition: with SB_CONFIG_WIFI_PATCHES_EXT_FLASH_STORE the
 * sysbuild generates its own nrf70_wifi_fw partition, and that is the one
 * nrf70_fw_ext opens — verified on hardware, the driver reported
 * fa_id=12 off=0x7c0000 while this code was writing to id 11 at 0x7a0000.
 * Writing to the wrong one produced a valid-looking partition and a WiFi
 * stack that could never load its firmware. */
#define NRF70_FW_PARTITION_ID FIXED_PARTITION_ID(nrf70_wifi_fw)

/* Mirrors struct nrf70_fw_image_info (nrf_wifi/fw_if/umac_if/inc/fw/patch_info.h).
 * Only the leading fields are needed to tell a provisioned partition from an
 * erased one, so the hash and payload are not declared here. */
struct fw_header {
	uint32_t signature;
	uint32_t num_images;
	uint32_t version;
	uint32_t feature_flags;
	uint32_t len;
};

/* Chunk size for the SD -> flash copy. Taken from the heap for the duration of
 * the copy rather than held statically: static RAM is at ~97% and this buffer
 * is needed once per device lifetime, not on every boot. */
#define COPY_CHUNK 2048

/* Outcome of the provisioning attempt at boot. Kept because the boot-time logs
 * proved unreliable to retrieve: AT+NRF70FW? serves this instead of us having
 * to catch the log window. -EINPROGRESS means it never ran. */
static int last_provision_result = -EINPROGRESS;
static bool last_provision_wrote;

/**
 * Whether the partition already holds this exact patch.
 *
 * Compares the header rather than hashing 87KB on every boot: an erased
 * partition reads as 0xFFFFFFFF, and a different patch version differs in
 * signature, version or length. The WiFi driver verifies the hash before use,
 * so a subtler mismatch is caught there rather than silently accepted.
 */
static bool partition_matches(const struct flash_area *fa,
			      const struct fw_header *want)
{
	struct fw_header have;

	if (flash_area_read(fa, 0, &have, sizeof(have)) != 0) {
		return false;
	}

	if (have.signature == 0xFFFFFFFFU) {
		LOG_WRN("nRF70 patch partition is erased");
		return false;
	}

	return have.signature == want->signature &&
	       have.version == want->version &&
	       have.len == want->len;
}

static int copy_file_to_partition(struct fs_file_t *file,
				  const struct flash_area *fa,
				  size_t total)
{
	size_t written = 0;
	uint8_t *copy_buf;
	int err;

	LOG_WRN("Writing %u bytes of nRF70 patch to flash", (unsigned int)total);

	copy_buf = k_malloc(COPY_CHUNK);
	if (!copy_buf) {
		LOG_ERR("No heap for the copy buffer");
		return -ENOMEM;
	}

	err = flash_area_erase(fa, 0, fa->fa_size);
	if (err) {
		LOG_ERR("Erase failed: %d", err);
		k_free(copy_buf);
		return err;
	}

	if (fs_seek(file, 0, FS_SEEK_SET) < 0) {
		k_free(copy_buf);
		return -EIO;
	}

	while (written < total) {
		ssize_t got = fs_read(file, copy_buf, COPY_CHUNK);

		if (got <= 0) {
			LOG_ERR("Short read at %u: %d", (unsigned int)written, (int)got);
			k_free(copy_buf);
			return got < 0 ? (int)got : -EIO;
		}

		/* flash_area_write needs write-block-aligned lengths; the final
		 * chunk is padded rather than truncated. The header's len field
		 * bounds the real payload, so trailing bytes are ignored. */
		size_t chunk = (size_t)got;

		if (chunk % 4) {
			size_t pad = 4 - (chunk % 4);

			memset(copy_buf + chunk, 0xFF, pad);
			chunk += pad;
		}

		err = flash_area_write(fa, written, copy_buf, chunk);
		if (err) {
			LOG_ERR("Write failed at %u: %d", (unsigned int)written, err);
			k_free(copy_buf);
			return err;
		}

		written += (size_t)got;
	}

	k_free(copy_buf);
	LOG_WRN("nRF70 patch provisioned (%u bytes)", (unsigned int)written);

	return 0;
}

int nrf70_fw_partition_status(struct nrf70_fw_status *out)
{
	const struct flash_area *fa;
	struct fw_header have;
	int err;

	if (!out) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));

	err = flash_area_open(NRF70_FW_PARTITION_ID, &fa);
	if (err) {
		return err;
	}

	out->partition_size = fa->fa_size;
	err = flash_area_read(fa, 0, &have, sizeof(have));
	flash_area_close(fa);

	if (err) {
		return err;
	}

	out->signature = have.signature;
	out->version = have.version;
	out->len = have.len;
	out->provisioned = (have.signature != 0xFFFFFFFFU && have.signature != 0);
	out->last_result = last_provision_result;
	out->wrote_this_boot = last_provision_wrote;

	return 0;
}

int nrf70_fw_provision(void)
{
	const struct flash_area *fa;
	struct fs_file_t file;
	struct fs_dirent stat;
	struct fw_header want;
	int err;

	err = flash_area_open(NRF70_FW_PARTITION_ID, &fa);
	if (err) {
		LOG_ERR("Cannot open nRF70 patch partition: %d", err);
		return err;
	}

	/* The SD card is power-gated when idle, so ask for it explicitly. */
	if (storage_ensure_mounted() != 0) {
		LOG_ERR("No SD card — cannot provision the nRF70 patch");
		flash_area_close(fa);
		last_provision_result = -ENODEV;
		return -ENODEV;
	}

	if (fs_stat(CONFIG_CLIP_NRF70_FW_PATH, &stat) != 0) {
		/* Not fatal by itself: a partition already provisioned keeps
		 * working, and the file only needs to be present once. */
		LOG_WRN("%s not found on the SD card", CONFIG_CLIP_NRF70_FW_PATH);
		flash_area_close(fa);
		last_provision_result = -ENOENT;
		return -ENOENT;
	}

	if (stat.size > fa->fa_size) {
		LOG_ERR("Patch is %u bytes, partition holds %u",
			(unsigned int)stat.size, (unsigned int)fa->fa_size);
		flash_area_close(fa);
		return -EFBIG;
	}

	fs_file_t_init(&file);
	err = fs_open(&file, CONFIG_CLIP_NRF70_FW_PATH, FS_O_READ);
	if (err) {
		LOG_ERR("Cannot open %s: %d", CONFIG_CLIP_NRF70_FW_PATH, err);
		flash_area_close(fa);
		return err;
	}

	if (fs_read(&file, &want, sizeof(want)) != (ssize_t)sizeof(want)) {
		LOG_ERR("Patch file is too short to hold a header");
		fs_close(&file);
		flash_area_close(fa);
		return -EIO;
	}

	if (partition_matches(fa, &want)) {
		LOG_WRN("nRF70 patch already provisioned (version %u)",
			(unsigned int)want.version);
		err = 0;
	} else {
		err = copy_file_to_partition(&file, fa, (size_t)stat.size);
		last_provision_wrote = (err == 0);
	}

	fs_close(&file);
	flash_area_close(fa);
	last_provision_result = err;

	return err;
}
