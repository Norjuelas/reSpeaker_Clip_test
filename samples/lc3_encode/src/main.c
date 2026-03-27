/*
 * Copyright (c) 2024 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <nrfx_pdm.h>
#include <nrfx_clock.h>
#include <string.h>
#include <stdio.h>

#include <sw_codec_lc3.h>

#ifdef CONFIG_SPEEXDSP
#include <speex/speex_preprocess.h>

/* SpeexDSP preprocessing parameters (runtime configurable) */
#define SPEEXDSP_NOISE_SUPPRESS_DB	30
#define SPEEXDSP_DEREVERB_ENABLE	1
#define SPEEXDSP_DEREVERB_LEVEL		40
#define SPEEXDSP_DEREVERB_DECAY		20
#endif

#include "ble.h"

LOG_MODULE_REGISTER(lc3_encode, LOG_LEVEL_INF);

/* Audio configuration */
#define SAMPLE_RATE_HZ   16000
#define SAMPLE_BITS      16
#define DMIC_CHANNELS    2        /* DMIC always captures stereo */
#define CAPTURE_MS       10       /* 10ms frames (LC3 standard) */
#define BLOCK_COUNT      16       /* 16 blocks for streaming buffer */

/* LC3 frame size (in microseconds) - LC3 standard frame durations */
#define LC3_FRAME_DURATION_US  10000  /* 10ms frame */

/* LC3 frame size (samples per channel) */
#define LC3_FRAME_SIZE  ((SAMPLE_RATE_HZ * LC3_FRAME_DURATION_US)) / 1000000

/* LC3 configuration */
#define LC3_BITRATE      32000   /* 32 kbps per channel */
#define MAX_LC3_PACKET_SIZE  200  /* Max LC3 packet size */

/* DMIC block size (always stereo capture) */
#define BLOCK_SIZE      (((SAMPLE_BITS / 8) * (SAMPLE_RATE_HZ * CAPTURE_MS)) / 1000) * DMIC_CHANNELS

/* Audio modes */
enum audio_mode {
	MODE_MONO = 0,    /* Left channel only */
	MODE_STEREO = 1,  /* Stereo output */
	MODE_MERGE = 2,   /* Mix L+R to mono */
};

static const char *mode_names[] = {"mono", "stereo", "merge"};

static const struct device *const dmic = DEVICE_DT_GET(DT_ALIAS(dmic0));
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct gpio_dt_spec mic_en = GPIO_DT_SPEC_GET_OR(DT_NODELABEL(mic_reg), gpios, {0});

/* Memory slab for DMIC buffers */
K_MEM_SLAB_DEFINE_STATIC(mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

/* LC3 encoder state */
static enum audio_mode current_mode = MODE_STEREO;
static int lc3_channels = 2;
static int lc3_bitrate = 64000;  /* 32 kbps per channel for stereo */
static uint16_t pcm_bytes_required = 0;

#ifdef CONFIG_SPEEXDSP
/* SpeexDSP preprocessor state (for noise suppression and dereverb) */
static SpeexPreprocessState *speex_pp;
static bool speex_enabled = true;  /* SpeexDSP enabled flag */
#endif

/* Flow control state */
static volatile bool streaming_active = false;

/* BLE streaming state */
static volatile bool streaming_to_ble = false;

/* UART output state */
static volatile bool streaming_to_uart = true;

/* PCM stream configuration for DMIC */
static struct pcm_stream_cfg stream = {
	.pcm_rate = SAMPLE_RATE_HZ,
	.pcm_width = SAMPLE_BITS,
	.block_size = BLOCK_SIZE,
	.mem_slab = &mem_slab,
};

/* DMIC configuration */
static struct dmic_cfg cfg = {
	.io = {
		.min_pdm_clk_freq = 1000000,
		.max_pdm_clk_freq = 3500000,
		.min_pdm_clk_dc = 40,
		.max_pdm_clk_dc = 60,
	},
	.streams = &stream,
	.channel = {
		.req_num_streams = 1,
		.req_num_chan = DMIC_CHANNELS,
	},
};

/* Buffer for merged/mono processing */
static int16_t processed_buffer[LC3_FRAME_SIZE];

/* SD Card and File System */
static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};
static bool sd_mounted = false;
static bool saving_to_sd = false;
static struct fs_file_t current_file;
static uint32_t current_file_bytes = 0;
static uint32_t total_sessions_sd = 0;

/* SD write buffer - 4KB for efficient writes */
#define SD_WRITE_BUFFER_SIZE 4096
static uint8_t sd_write_buffer[SD_WRITE_BUFFER_SIZE];
static uint32_t sd_buffer_pos = 0;

/* LC3 codec buffer */
#define LC3_CODEC_BUFFER_SIZE 4096
static uint8_t lc3_codec_buffer[LC3_CODEC_BUFFER_SIZE];
static uint32_t lc3_codec_buffer_size = LC3_CODEC_BUFFER_SIZE;

/* SD Card functions */
static int sdcard_init(void)
{
	int rc;

	LOG_INF("Initializing SD card...");

	/* Initialize SD card disk */
	rc = disk_access_init("SD");
	if (rc != 0) {
		LOG_WRN("SD card init failed: %d", rc);
		return rc;
	}

	/* Try to mount the SD card */
	rc = fs_mount(&mp);
	if (rc != 0) {
		LOG_WRN("SD card mount failed: %d (not formatted?)", rc);
		LOG_INF("SD card functions disabled");
		return 0;
	}

	sd_mounted = true;
	LOG_INF("SD card mounted at %s", mp.mnt_point);

	return 0;
}

static void list_sd_files(void)
{
	struct fs_dir_t dirp;
	struct fs_dirent entry;
	int rc;

	if (!sd_mounted) {
		fprintf(stderr, "\n[SD: Not mounted]\n");
		return;
	}

	fs_dir_t_init(&dirp);
	rc = fs_opendir(&dirp, "/SD:");
	if (rc != 0) {
		fprintf(stderr, "\n[SD: Failed to open directory: %d]\n", rc);
		return;
	}

	fprintf(stderr, "\n[SD File List:]\n");

	uint32_t total_files = 0;
	uint64_t total_bytes = 0;

	while (1) {
		rc = fs_readdir(&dirp, &entry);
		if (rc != 0 || entry.name[0] == 0) {
			break;
		}

		/* Skip directories */
		if (entry.type == FS_DIR_ENTRY_DIR) {
			continue;
		}

		total_files++;
		total_bytes += entry.size;

		/* Format size */
		if (entry.size < 1024) {
			fprintf(stderr, "  %s: %u B\n", entry.name, (uint32_t)entry.size);
		} else if (entry.size < 1024 * 1024) {
			fprintf(stderr, "  %s: %u KB\n", entry.name, (uint32_t)(entry.size / 1024));
		} else {
			fprintf(stderr, "  %s: %u MB\n", entry.name, (uint32_t)(entry.size / (1024 * 1024)));
		}
	}

	fs_closedir(&dirp);

	if (total_files > 0) {
		fprintf(stderr, "Total: %u files, ", total_files);
		if (total_bytes < 1024) {
			fprintf(stderr, "%u B\n", (uint32_t)total_bytes);
		} else if (total_bytes < 1024 * 1024) {
			fprintf(stderr, "%u KB\n", (uint32_t)(total_bytes / 1024));
		} else {
			fprintf(stderr, "%u MB\n", (uint32_t)(total_bytes / (1024 * 1024)));
		}
	} else {
		fprintf(stderr, "  (empty)\n");
	}

	fprintf(stderr, "[SD End]\n");
}

/* Flush SD write buffer to file */
static int sd_flush_buffer(void)
{
	if (sd_buffer_pos == 0 || !saving_to_sd) {
		return 0;
	}

	ssize_t written = fs_write(&current_file, sd_write_buffer, sd_buffer_pos);

	if (written < 0) {
		LOG_ERR("SD write error: %zd", written);
		saving_to_sd = false;
		fs_close(&current_file);
		return written;
	}

	if (written != sd_buffer_pos) {
		LOG_ERR("SD partial write: %zd/%u", written, sd_buffer_pos);
	}

	current_file_bytes += written;
	sd_buffer_pos = 0;

	return 0;
}

static int sd_start_file(const char *filename)
{
	int rc;
	char filepath[256];

	if (!sd_mounted) {
		return -ENODEV;
	}

	snprintf(filepath, sizeof(filepath), "/SD:/%s", filename);

	fs_file_t_init(&current_file);
	rc = fs_open(&current_file, filepath, FS_O_CREATE | FS_O_WRITE);
	if (rc != 0) {
		LOG_ERR("Failed to create file: %d", rc);
		return rc;
	}

	/* Reset counters and buffer */
	current_file_bytes = 0;
	sd_buffer_pos = 0;
	saving_to_sd = true;

	fprintf(stderr, "\n[SD: Started recording to %s]\n", filename);
	return 0;
}

static void sd_write_data(const uint8_t *data, uint32_t len)
{
	if (!saving_to_sd) {
		return;
	}

	uint32_t remaining = len;
	uint32_t offset = 0;

	while (remaining > 0) {
		uint32_t space = SD_WRITE_BUFFER_SIZE - sd_buffer_pos;
		uint32_t to_copy = (remaining < space) ? remaining : space;

		memcpy(&sd_write_buffer[sd_buffer_pos], &data[offset], to_copy);
		sd_buffer_pos += to_copy;
		offset += to_copy;
		remaining -= to_copy;

		/* Flush buffer when full */
		if (sd_buffer_pos >= SD_WRITE_BUFFER_SIZE) {
			if (sd_flush_buffer() < 0) {
				return;
			}
		}
	}
}

static void sd_end_file(void)
{
	if (!saving_to_sd) {
		return;
	}

	/* Flush any remaining data in buffer */
	sd_flush_buffer();

	fs_close(&current_file);
	saving_to_sd = false;
	total_sessions_sd++;

	fprintf(stderr, "[SD: Saved %u bytes, total sessions: %u]\n",
		current_file_bytes, total_sessions_sd);
}

static int mic_power_on(void)
{
	if (mic_en.port) {
		gpio_pin_configure_dt(&mic_en, GPIO_OUTPUT);
		gpio_pin_set_dt(&mic_en, 1);
	}
	return 0;
}

static int mic_power_off(void)
{
	if (mic_en.port) {
		gpio_pin_configure_dt(&mic_en, GPIO_OUTPUT);
		gpio_pin_set_dt(&mic_en, 0);
	}
	return 0;
}

static int init_lc3_encoder(void)
{
	int err;

	/* Uninitialize existing encoder if any */
	sw_codec_lc3_enc_uninit_all();

	/* Initialize LC3 codec */
	err = sw_codec_lc3_init(lc3_codec_buffer, &lc3_codec_buffer_size,
				LC3_FRAME_DURATION_US);
	if (err != 0) {
		LOG_ERR("Failed to initialize LC3 codec: %d", err);
		return err;
	}

	/* Initialize LC3 encoder with current mode settings */
	err = sw_codec_lc3_enc_init(SAMPLE_RATE_HZ, SAMPLE_BITS, LC3_FRAME_DURATION_US,
				     lc3_bitrate, lc3_channels, &pcm_bytes_required);
	if (err != 0) {
		LOG_ERR("Failed to create LC3 encoder: %d", err);
		return err;
	}

	LOG_INF("LC3 encoder: %d Hz, %d ch, %d bps, frame_duration=%d us, mode=%s",
		SAMPLE_RATE_HZ, lc3_channels, lc3_bitrate, LC3_FRAME_DURATION_US, mode_names[current_mode]);

	return 0;
}

static void lc3_encoder_cleanup(void)
{
	sw_codec_lc3_enc_uninit_all();
}

#ifdef CONFIG_SPEEXDSP
static int init_speex_pp(void)
{
	/* Destroy old preprocessor if exists */
	if (speex_pp) {
		speex_preprocess_state_destroy(speex_pp);
		speex_pp = NULL;
	}

	/* Create preprocessor state - process per channel */
	speex_pp = speex_preprocess_state_init(LC3_FRAME_SIZE, SAMPLE_RATE_HZ);
	if (!speex_pp) {
		LOG_ERR("Failed to create SpeexDSP preprocessor");
		return -1;
	}

	/* Set noise suppression */
	int denoise = SPEEXDSP_NOISE_SUPPRESS_DB;
	speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &denoise);

#if SPEEXDSP_DEREVERB_ENABLE
	/* Enable dereverberation */
	int dereverb = 1;
	speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_DEREVERB, &dereverb);

	/* Set dereverb level */
	int dereverb_level = SPEEXDSP_DEREVERB_LEVEL;
	speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_DEREVERB_LEVEL, &dereverb_level);

	/* Set dereverb decay */
	int dereverb_decay = SPEEXDSP_DEREVERB_DECAY;
	speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_DEREVERB_DECAY, &dereverb_decay);
#endif

	LOG_INF("SpeexDSP: noise_suppress=%d dB, dereverb=%d",
		denoise, SPEEXDSP_DEREVERB_ENABLE);

	return 0;
}

static void speex_pp_cleanup(void)
{
	if (speex_pp) {
		speex_preprocess_state_destroy(speex_pp);
		speex_pp = NULL;
	}
}

/* Apply SpeexDSP preprocessing to audio data */
static void apply_speex_pp(int16_t *audio, int frame_size)
{
	if (!speex_enabled || !speex_pp) {
		return;
	}

	/* Run preprocessing (noise suppression, dereverb, etc.) */
	speex_preprocess_run(speex_pp, audio);
}
#endif

/* Process stereo PCM data according to current mode */
static int16_t *process_pcm_frame(int16_t *stereo_input, int frame_size)
{
	/* stereo_input layout: L0, R0, L1, R1, L2, R2, ... */

	if (current_mode == MODE_MONO) {
		/* Extract left channel only */
		for (int i = 0; i < frame_size; i++) {
			processed_buffer[i] = stereo_input[i * 2];  /* Left channel */
		}
		return processed_buffer;

	} else if (current_mode == MODE_MERGE) {
		/* Mix left and right: (L + R) / 2 */
		for (int i = 0; i < frame_size; i++) {
			int32_t left = stereo_input[i * 2];
			int32_t right = stereo_input[i * 2 + 1];
			/* Average with saturation handling */
			int32_t mixed = (left + right) / 2;
			/* Clamp to int16 range */
			if (mixed > 32767) mixed = 32767;
			if (mixed < -32768) mixed = -32768;
			processed_buffer[i] = (int16_t)mixed;
		}
		return processed_buffer;

	} else {
		/* MODE_STEREO: return original stereo data */
		return stereo_input;
	}
}

static void send_header(void)
{
	/* Send header for Python script to parse */
	printf(">>> LC3_STREAM_START\n");
	printf("SAMPLE_RATE=%d\n", SAMPLE_RATE_HZ);
	printf("CHANNELS=%d\n", lc3_channels);
	printf("FRAME_SIZE=%d\n", LC3_FRAME_SIZE);
	printf("FRAME_DURATION_US=%d\n", LC3_FRAME_DURATION_US);
	printf("BITRATE=%d\n", lc3_bitrate);
	printf(">>> DATA_START\n");
	fflush(stdout);
}

static void send_encoded_frame(const uint8_t *data, uint16_t len)
{
	/* Send frame length and data in hex format */
	printf("%04x\n", (uint16_t)len);

	for (uint16_t i = 0; i < len; i++) {
		printf("%02x", data[i]);
	}
	printf("\n");
}

static void set_audio_mode(enum audio_mode mode)
{
	if (mode == current_mode) {
		return;
	}

	current_mode = mode;

	/* Update LC3 encoder settings based on mode */
	switch (mode) {
	case MODE_MONO:
		lc3_channels = 1;
		lc3_bitrate = 32000;
		break;
	case MODE_STEREO:
		lc3_channels = 2;
		lc3_bitrate = 64000;
		break;
	case MODE_MERGE:
		lc3_channels = 1;
		lc3_bitrate = 32000;
		break;
	}

	/* Reinitialize encoder with new settings */
	init_lc3_encoder();

	fprintf(stderr, "\r[MODE=%s]\n", mode_names[mode]);
}

static void update_filename_timestamp(char *filename, size_t len)
{
	/* Get current time from system uptime - approximate */
	uint64_t uptime_ms = k_uptime_get() / 1000;
	uint32_t seconds = uptime_ms % 86400;  /* Seconds within a day */
	uint32_t hours = seconds / 3600;
	uint32_t minutes = (seconds % 3600) / 60;
	uint32_t secs = seconds % 60;

	/* For now, use uptime as unique identifier */
	static uint32_t session_counter = 0;
	session_counter++;

	/* Generate just the filename without path prefix */
	snprintf(filename, len, "rec_%06u_%02u%02u%02u_%s.lc3",
		 (uint32_t)(k_uptime_get() / 1000),
			hours, minutes, secs,
			mode_names[current_mode]);
}

int main(void)
{

#ifdef CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT
	nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
#endif

	int ret;
	void *buffer = NULL;
	uint32_t size;
	int frame_count = 0;

	/* Encoding time statistics */
	int64_t encode_time_min = INT64_MAX;
	int64_t encode_time_max = 0;
	int64_t encode_time_total = 0;
	int64_t encode_time_start;

	/* DSP processing time statistics (process_pcm_frame + SpeexDSP) */
	int64_t dsp_time_min = INT64_MAX;
	int64_t dsp_time_max = 0;
	int64_t dsp_time_total = 0;

	LOG_INF("ReSpeaker Lav LC3 Streaming Encoder");

	if (!device_is_ready(dmic)) {
		LOG_ERR("DMIC device not ready");
		return -ENODEV;
	}

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	printf("\n");
	printf("========================================\n");
	printf("LC3 Streaming Encoder\n");
	printf("========================================\n");
	printf("Commands: 1=mono, 2=stereo, 3=merge\n");
	printf("          u=toggle UART, d=toggle SD\n");
	printf("          b=toggle BLE, l=list files\n");
#ifdef CONFIG_SPEEXDSP
	printf("          p=toggle SpeexDSP (NS/Dereverb)\n");
#endif
	printf("          s=start, e=stop, q=quit\n");
	printf("========================================\n");
	printf("\n");

	/* Initialize SD card */
	sdcard_init();

	/* Initialize BLE */
	ret = ble_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize BLE: %d", ret);
		/* Continue anyway, BLE is optional */
	}

	/* Initialize LC3 encoder */
	ret = init_lc3_encoder();
	if (ret < 0) {
		LOG_ERR("Failed to initialize LC3 encoder");
		return ret;
	}

#ifdef CONFIG_SPEEXDSP
	/* Initialize SpeexDSP preprocessor */
	ret = init_speex_pp();
	if (ret < 0) {
		LOG_ERR("Failed to initialize SpeexDSP preprocessor");
		/* Continue anyway, SpeexDSP is optional */
	}
#endif

	/* Configure channel map - stereo LEFT and RIGHT channels */
	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	cfg.channel.req_chan_map_lo |= dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);

	/* Power on microphone */
	mic_power_on();

	/* Configure DMIC */
	ret = dmic_configure(dmic, &cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure DMIC: %d", ret);
		goto cleanup;
	}

	/* Set microphone gain - level 6 (+20dB) */
#ifdef NRF_PDM0_S
	nrf_pdm_gain_set(NRF_PDM0_S, 0x3C, 0x3C);
#else
	nrf_pdm_gain_set(NRF_PDM0_NS, 0x3C, 0x3C);
#endif

	bool running = true;

	/* Main loop: support multiple start/stop cycles */
	while (running) {
		/* Send ready message and wait for start command */
		printf(">>> READY\n");
		printf("Send 's' to start, 'e' to stop, 'q' to quit\n");
		printf("Mode: %s (send 1/2/3 to change)\n", mode_names[current_mode]);
		printf("Output: UART:%s SD:%s BLE:%s",
			streaming_to_uart ? "ON" : "OFF",
			saving_to_sd ? "ON" : "OFF",
			streaming_to_ble ? "ON" : "OFF");
#ifdef CONFIG_SPEEXDSP
		printf(" SpeexDSP:%s", speex_enabled ? "ON" : "OFF");
#endif
		printf("\n");
		printf("  u=toggle UART, d=toggle SD, b=toggle BLE\n");
		if (sd_mounted) {
			printf("  l=list SD files\n");
		}
		fflush(stdout);

		/* Poll for start command or mode change */
		streaming_active = false;
		while (!streaming_active && running) {
			uint8_t cmd;
			int rc = uart_poll_in(uart_dev, &cmd);
			if (rc == 0) {
				if (cmd == 's' || cmd == 'S') {
					streaming_active = true;
					frame_count = 0;
					/* Reset encode time statistics */
					encode_time_min = INT64_MAX;
					encode_time_max = 0;
					encode_time_total = 0;
					/* Reset DSP time statistics */
					dsp_time_min = INT64_MAX;
					dsp_time_max = 0;
					dsp_time_total = 0;
					fprintf(stderr, "\r[START]\n");
				} else if (cmd == '1') {
					set_audio_mode(MODE_MONO);
				} else if (cmd == '2') {
					set_audio_mode(MODE_STEREO);
				} else if (cmd == '3') {
					set_audio_mode(MODE_MERGE);
				} else if (cmd == 'd' || cmd == 'D') {
					if (sd_mounted) {
						saving_to_sd = !saving_to_sd;
						fprintf(stderr, "\r[SD save: %s]\n", saving_to_sd ? "ON" : "OFF");
					}
				} else if (cmd == 'u' || cmd == 'U') {
					streaming_to_uart = !streaming_to_uart;
					fprintf(stderr, "\r[UART: %s]\n", streaming_to_uart ? "ON" : "OFF");
				} else if (cmd == 'b' || cmd == 'B') {
					streaming_to_ble = !streaming_to_ble;
					if (streaming_to_ble) {
						if (ble_is_ready()) {
							fprintf(stderr, "\r[BLE: ON - connected]\n");
						} else {
							fprintf(stderr, "\r[BLE: ON - waiting for connection...]\n");
						}
					} else {
						fprintf(stderr, "\r[BLE: OFF]\n");
					}
#ifdef CONFIG_SPEEXDSP
				} else if (cmd == 'p' || cmd == 'P') {
					speex_enabled = !speex_enabled;
					fprintf(stderr, "\r[SpeexDSP: %s]\n", speex_enabled ? "ON" : "OFF");
#endif
				} else if (cmd == 'l' || cmd == 'L') {
					list_sd_files();
				} else if (cmd == 'q' || cmd == 'Q') {
					running = false;
					break;
				}
			}
			k_msleep(10);
		}

		if (!running) {
			break;
		}

		/* Send stream header */
		send_header();

		/* Start SD file if enabled */
		char filename[128];
		if (saving_to_sd && sd_mounted) {
			update_filename_timestamp(filename, sizeof(filename));
			ret = sd_start_file(filename);
			if (ret != 0) {
				LOG_ERR("Failed to start SD file, disabling SD save");
				saving_to_sd = false;
			}
		}

		/* Start DMIC - continuous recording */
		ret = dmic_trigger(dmic, DMIC_TRIGGER_START);
		if (ret < 0) {
			LOG_ERR("START trigger failed: %d", ret);
			goto cleanup;
		}

		/* Streaming loop: read -> process -> encode -> send (+ save to SD)
		 * NO LOG OUTPUT during streaming to keep data stream clean
		 */
		uint8_t lc3_packet[MAX_LC3_PACKET_SIZE];

		while (streaming_active) {
			/* Read one audio block from DMIC */
			ret = dmic_read(dmic, 0, &buffer, &size, 500);
			if (ret < 0) {
				/* DMIC read error, exit loop */
				fprintf(stderr, "\nDMIC read error: %d (errno: %d)\n", ret, -ret);
				break;
			}

			/* Validate block size */
			if (size != BLOCK_SIZE) {
				/* Wrong size, skip this block but continue */
				k_mem_slab_free(&mem_slab, buffer);
				buffer = NULL;
				continue;
			}

			/* Measure DSP processing time (process_pcm_frame + SpeexDSP) */
			encode_time_start = k_uptime_get();

			/* Process PCM data according to mode (mono/merge/stereo) */
			int16_t *pcm_data = process_pcm_frame((int16_t *)buffer, LC3_FRAME_SIZE);

#ifdef CONFIG_SPEEXDSP
			/* Apply SpeexDSP preprocessing (noise suppression, dereverb) */
			/* For mono/merge mode, process single channel. For stereo, process both channels separately. */
			if (current_mode == MODE_MONO || current_mode == MODE_MERGE) {
				apply_speex_pp(pcm_data, LC3_FRAME_SIZE);
			} else if (current_mode == MODE_STEREO) {
				/* Stereo: process left and right channels separately */
				/* Interleaved format: L0, R0, L1, R1, ... */
				int16_t temp_left[LC3_FRAME_SIZE];
				int16_t temp_right[LC3_FRAME_SIZE];

				/* Deinterleave */
				for (int i = 0; i < LC3_FRAME_SIZE; i++) {
					temp_left[i] = pcm_data[i * 2];
					temp_right[i] = pcm_data[i * 2 + 1];
				}

				/* Process left channel */
				apply_speex_pp(temp_left, LC3_FRAME_SIZE);

				/* Process right channel */
				apply_speex_pp(temp_right, LC3_FRAME_SIZE);

				/* Interleave back */
				for (int i = 0; i < LC3_FRAME_SIZE; i++) {
					pcm_data[i * 2] = temp_left[i];
					pcm_data[i * 2 + 1] = temp_right[i];
				}
			}
#endif

			/* Save DSP processing time before LC3 encode */
			int64_t dsp_time = k_uptime_get() - encode_time_start;

			/* Update DSP time statistics */
			if (dsp_time < dsp_time_min) {
				dsp_time_min = dsp_time;
			}
			if (dsp_time > dsp_time_max) {
				dsp_time_max = dsp_time;
			}
			dsp_time_total += dsp_time;

			/* Measure LC3 encoding time */
			encode_time_start = k_uptime_get();

			/* Encode the processed audio block with LC3 */
			uint16_t lc3_data_wr_size = 0;
			ret = sw_codec_lc3_enc_run(pcm_data, pcm_bytes_required, LC3_USE_BITRATE_FROM_INIT,
						   0, MAX_LC3_PACKET_SIZE, lc3_packet, &lc3_data_wr_size);

			/* Update encoding time statistics */
			int64_t encode_time = k_uptime_get() - encode_time_start;
			if (encode_time < encode_time_min) {
				encode_time_min = encode_time;
			}
			if (encode_time > encode_time_max) {
				encode_time_max = encode_time;
			}
			encode_time_total += encode_time;

			/* Free the buffer back to slab */
			k_mem_slab_free(&mem_slab, buffer);
			buffer = NULL;

			if (ret < 0) {
				/* Encode error, skip but continue */
				fprintf(stderr, "\nLC3 encode error: %d\n", ret);
				continue;
			}

			/* Save to SD card if enabled */
			if (saving_to_sd) {
				/* Write binary frame: [2-byte length][data] */
				uint16_t frame_len = (uint16_t)lc3_data_wr_size;
				sd_write_data((uint8_t *)&frame_len, 2);
				sd_write_data(lc3_packet, lc3_data_wr_size);
			}

			/* Send to BLE if enabled */
			if (streaming_to_ble && ble_is_ready()) {
				ble_send_lc3_frame(lc3_packet, lc3_data_wr_size);
			}

			/* Send encoded frame via UART if enabled */
			if (streaming_to_uart) {
				send_encoded_frame(lc3_packet, lc3_data_wr_size);
			}
			frame_count++;

			/* Check for stop command (non-blocking poll) */
			uint8_t cmd;
			int rc = uart_poll_in(uart_dev, &cmd);
			if (rc == 0) {
				if (cmd == 'e' || cmd == 'E') {
					fprintf(stderr, "\r[STOP]\n");
					streaming_active = false;
				} else if (cmd == 'q' || cmd == 'Q') {
					fprintf(stderr, "\r[QUIT]\n");
					streaming_active = false;
					running = false;
#ifdef CONFIG_SPEEXDSP
				} else if (cmd == 'p' || cmd == 'P') {
					speex_enabled = !speex_enabled;
					fprintf(stderr, "\r[SpeexDSP: %s]\n", speex_enabled ? "ON" : "OFF");
#endif
				}
			}
		}

		/* Stop DMIC */
		dmic_trigger(dmic, DMIC_TRIGGER_STOP);

		/* Close SD file if open */
		if (saving_to_sd) {
			sd_end_file();
		}

		/* Send end marker */
		printf(">>> DATA_END\n");
		fflush(stdout);

		fprintf(stderr, "Session ended. Frames: %d\n", frame_count);
		if (frame_count > 0) {
			fprintf(stderr, "LC3 encode time: min=%lld ms, max=%lld ms, avg=%lld ms\n",
				encode_time_min, encode_time_max,
				encode_time_total / frame_count);
			fprintf(stderr, "DSP process time: min=%lld ms, max=%lld ms, avg=%lld ms\n",
				dsp_time_min, dsp_time_max,
				dsp_time_total / frame_count);
		}

		/* Print BLE statistics if enabled */
		if (streaming_to_ble) {
			uint32_t ble_frames, ble_drops;
			uint64_t ble_bytes;
			int64_t ble_total, ble_min, ble_max;

			ble_get_stats(&ble_frames, &ble_bytes, &ble_drops,
				     &ble_total, &ble_min, &ble_max);

			float loss_rate = ble_get_packet_loss_rate();

			if (ble_frames > 0 || ble_drops > 0) {
				uint32_t avg_us = (ble_total * 1000) / ble_frames;
				uint32_t avg_ms = avg_us / 1000;
				uint32_t avg_frac_us = avg_us % 1000;
				fprintf(stderr, "[BLE: frames=%u, bytes=%llu, drops=%u, loss_rate=%.2f%%]\n",
					ble_frames, ble_bytes, ble_drops, loss_rate);
				fprintf(stderr, "[BLE: send time - min=%lldms, max=%lldms, avg=%u.%03ums/frame]\n",
					ble_min, ble_max, avg_ms, avg_frac_us);
			}
		}
	}

cleanup:
#ifdef CONFIG_SPEEXDSP
	speex_pp_cleanup();
#endif
	lc3_encoder_cleanup();
	mic_power_off();

	LOG_INF("Program exited.");

	return 0;
}
