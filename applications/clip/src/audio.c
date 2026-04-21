/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <nrfx_pdm.h>
#include <cmsis_core.h>

#include <opus.h>
#include <opus_types.h>

#ifdef CONFIG_SPEEXDSP
#include <speex/speex_preprocess.h>
#endif

#include "audio.h"
#include "clip.h"
#include "config.h"
#include "storage.h"
#include "transfer.h"
#include "ble.h"
#include "display.h"

LOG_MODULE_REGISTER(audio, CONFIG_CLIP_LOG_LEVEL);

/* Memory slab for DMIC buffers */
K_MEM_SLAB_DEFINE_STATIC(audio_mem_slab, AUDIO_BLOCK_SIZE, 16, 4);

/* DMIC device */
static const struct device *const dmic_dev = DEVICE_DT_GET(DT_ALIAS(dmic0));
static const struct device *mic_regulator =
	DEVICE_DT_GET(DT_NODELABEL(mic_reg));

/* PCM stream configuration for DMIC */
static struct pcm_stream_cfg audio_stream = {
    .pcm_rate = AUDIO_SAMPLE_RATE,
    .pcm_width = AUDIO_SAMPLE_BITS,
    .block_size = AUDIO_BLOCK_SIZE,
    .mem_slab = &audio_mem_slab,
};

/* DMIC configuration */
static struct dmic_cfg audio_cfg = {
    .io = {
        .min_pdm_clk_freq = 1000000,
        .max_pdm_clk_freq = 3500000,
        .min_pdm_clk_dc = 40,
        .max_pdm_clk_dc = 60,
    },
    .streams = &audio_stream,
    .channel = {
        .req_num_streams = 1,
        .req_num_chan = AUDIO_CHANNELS,
    },
};

/* Buffer for processed audio */
static int16_t processed_buffer[AUDIO_OPUS_FRAME_SIZE];

/* Recording state */
static bool recording_active = false;
static enum audio_mode current_mode = AUDIO_MODE_MERGE;
static int opus_channels = 1;

/* Semaphore for blocking audio thread when not recording */
static K_SEM_DEFINE(audio_start_sem, 0, 1);

/* Mutex for protecting recording state */
static K_MUTEX_DEFINE(audio_state_mutex);

/* Stop request flag */
static volatile bool stop_requested = false;

/* Semaphore: audio thread signals when stop cleanup is done */
static struct k_sem stop_done_sem;

/* Pause state */
static volatile bool pause_requested = false;
static volatile bool is_paused = false;

/* Current session ID */
static char current_session_id[32] = {0};
static uint32_t recording_frame_count = 0;

/* Storage integration */
static struct storage_file current_storage_file = {0};
static uint32_t current_file_index = 1;
static uint32_t file_start_frame_count = 0;

/* Transfer state tracking for dynamic segment duration */
static bool was_transferring = false;

/* Audio energy level (0-10) for visualization */
static uint8_t audio_energy_level = 0;
static uint8_t audio_energy_history[13];  /* History for trend display */
static int audio_energy_history_index = 0;
static bool audio_energy_history_filled = false;
static K_MUTEX_DEFINE(audio_energy_mutex);

/* Statistics */
static struct audio_stats stats = {0};
static uint64_t encode_time_total_us = 0;
static uint64_t dsp_time_total_us = 0;

/* CPU cycle counter helpers (DWT->CYCCNT at 128 MHz) */
#define CPU_FREQ_HZ 128000000
static inline uint32_t cyc_to_us(uint32_t cycles)
{
	return (uint32_t)((uint64_t)cycles * 1000000ULL / CPU_FREQ_HZ);
}
static inline uint32_t cyc_start(void) { return DWT->CYCCNT; }
static inline uint32_t cyc_end(uint32_t start) { return DWT->CYCCNT - start; }

/* Data callback */
static audio_data_callback_t data_callback = NULL;
static void *data_callback_user_data = NULL;

/* Audio recording thread */
K_THREAD_STACK_DEFINE(audio_thread_stack, CONFIG_CLIP_AUDIO_STACK_SIZE);
static struct k_thread audio_thread_data;
static k_tid_t audio_thread_id;

/* Opus encoder state */
static OpusEncoder *opus_encoder = NULL;

#ifdef CONFIG_SPEEXDSP
/* SpeexDSP preprocessor state */
static SpeexPreprocessState *speex_pp = NULL;
static bool speex_enabled = false;

/* Cached SpeexDSP parameters for reinit detection */
static struct {
    uint8_t noise_suppress;
    bool dereverb_enabled;
    bool initialized;
} dsp_params = {0};
#endif

/* Cached encoder parameters for reinit detection */
static struct {
    uint32_t bitrate;
    uint8_t complexity;
    int channels;
    bool initialized;
} encoder_params = {0};

/* Forward declarations */
static int init_opus_encoder(void);
static void cleanup_opus_encoder(void);
static bool encoder_params_changed(void);
#ifdef CONFIG_SPEEXDSP
static int init_speex_preprocessor(void);
static void cleanup_speex_preprocessor(void);
static bool dsp_params_changed(void);
#endif
static int16_t *process_pcm_frame(int16_t *stereo_input, int frame_size);
static int mic_power_on(void);
static int mic_power_off(void);
static int audio_start_recording_internal(enum audio_mode mode);
static int audio_stop_recording_internal(void);
static void calculate_energy_level(int16_t *pcm_data, int frame_size);
static void dmic_flush_initial(void);

/* Static Opus packet buffer (shared across encoding operations) */
static uint8_t opus_packet[AUDIO_MAX_PACKET_SIZE];

int audio_init(void)
{
    int ret;

    if (!device_is_ready(dmic_dev)) {
        LOG_ERR("dmic not ready");
        return -ENODEV;
    }

    /* Initialize audio parameters from loaded config */
    struct clip_context *c = clip_get_context();
    current_mode = AUDIO_MODE_MERGE;  /* Both modes use merge (L+R → mono) */

    /* Set channels */
    if (current_mode == AUDIO_MODE_STEREO) {
        opus_channels = 2;
    } else {
        opus_channels = 1;
    }

    /* Configure channel map */
    audio_cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
    audio_cfg.channel.req_chan_map_lo |= dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);

    /* Configure DMIC */
    ret = dmic_configure(dmic_dev, &audio_cfg);
    if (ret < 0) {
        LOG_ERR("dmic_cfg: %d", ret);
        return ret;
    }

    /* Set microphone gain - maximum gain (+20dB, ~3x amplitude increase) */
#ifdef NRF_PDM0_S
    nrf_pdm_gain_set(NRF_PDM0_S, 0x50, 0x50);
#else
    nrf_pdm_gain_set(NRF_PDM0_NS, 0x50, 0x50);
#endif

    /* Initialize stop-done semaphore */
    k_sem_init(&stop_done_sem, 0, 1);

    /* Start audio recording thread */
    audio_thread_id = k_thread_create(&audio_thread_data,
                      audio_thread_stack,
                      CONFIG_CLIP_AUDIO_STACK_SIZE,
                      audio_recording_thread,
                      NULL, NULL, NULL,
                      CONFIG_CLIP_AUDIO_THREAD_PRIORITY, 0, K_NO_WAIT);
    if (!audio_thread_id) {
        LOG_ERR("Failed to create audio thread");
        return -ENOMEM;
    }
    k_thread_name_set(&audio_thread_data, "audio_rec");

    LOG_INF("Audio thread created: priority=%d, stack=%u",
        CONFIG_CLIP_AUDIO_THREAD_PRIORITY,
        CONFIG_CLIP_AUDIO_STACK_SIZE);

    return 0;
}

void audio_cleanup(void)
{
    audio_stop_recording();
}

int audio_start_recording(enum audio_mode mode)
{
    struct clip_context *c = clip_get_context();
    uint16_t year;
    uint8_t month, day, hour, min, sec;
    int ret;

    /* Lock to protect session_id and state variables */
    k_mutex_lock(&audio_state_mutex, K_FOREVER);

    if (recording_active) {
        k_mutex_unlock(&audio_state_mutex);
        LOG_WRN("Recording already active");
        return -EBUSY;
    }

    /* Store mode for audio thread */
    current_mode = mode;
    if (current_mode == AUDIO_MODE_STEREO) {
        opus_channels = 2;
    } else {
        opus_channels = 1;
    }

    /* Generate session ID: always 14 digits */
    if (clip_get_current_time(&year, &month, &day, &hour, &min, &sec)) {
        /* Time synchronized: YYYYMMDDHHMMSS (14 digits) */
        snprintf(current_session_id, sizeof(current_session_id),
            "%04d%02d%02d%02d%02d%02d",
            year, month, day, hour, min, sec);
    } else {
        /* Time not set: use 0 + uptime (14 digits total) */
        uint32_t uptime_sec = (uint32_t)(k_uptime_get() / 1000);
        snprintf(current_session_id, sizeof(current_session_id),
            "0%013u", uptime_sec);
        LOG_WRN("Time not synchronized, using uptime-based session ID");
    }

    /* Reset counters */
    recording_frame_count = 0;
    stop_requested = false;
    k_sem_reset(&stop_done_sem);

    /* Wake up audio thread by giving semaphore */
    k_sem_give(&audio_start_sem);
    k_mutex_unlock(&audio_state_mutex);

    LOG_INF("Recording start requested (mode=%d, session=%s)", mode, current_session_id);
    return 0;
}

int audio_stop_recording(void)
{
    k_mutex_lock(&audio_state_mutex, K_FOREVER);

    if (!recording_active) {
        /* Recording not yet active, but start may be pending (semaphore given
         * but audio thread hasn't processed it yet). Set stop_requested so
         * the audio thread skips the pending start.
         */
        stop_requested = true;
        k_mutex_unlock(&audio_state_mutex);
        return 0;
    }

    /* Set stop request flag - audio thread will handle cleanup.
     * Pause polling loop checks stop_requested, so no semaphore needed.
     */
    stop_requested = true;

    k_mutex_unlock(&audio_state_mutex);
    LOG_INF("Recording stop requested");

    /* Wait for audio thread to finish cleanup */
    k_sem_take(&stop_done_sem, K_MSEC(2000));

    return 0;
}

int audio_pause_recording(void)
{
    k_mutex_lock(&audio_state_mutex, K_FOREVER);

    if (!recording_active || is_paused) {
        k_mutex_unlock(&audio_state_mutex);
        return 0;
    }

    /* Set pause request flag */
    pause_requested = true;
    k_mutex_unlock(&audio_state_mutex);

    LOG_INF("Recording pause requested");
    return 0;
}

int audio_resume_recording(void)
{
    k_mutex_lock(&audio_state_mutex, K_FOREVER);

    if (!recording_active || !is_paused) {
        k_mutex_unlock(&audio_state_mutex);
        return 0;
    }

    /* Clear paused state - audio thread's polling loop will detect this */
    is_paused = false;
    k_mutex_unlock(&audio_state_mutex);

    LOG_INF("Recording resume requested");
    return 0;
}

bool audio_is_recording(void)
{
    k_mutex_lock(&audio_state_mutex, K_FOREVER);
    bool is_active = recording_active;
    k_mutex_unlock(&audio_state_mutex);

    return is_active;
}

int audio_add_bookmark(void)
{
    uint32_t recording_sec;
    struct audio_stats stats;

    /* Check if recording is active */
    if (!audio_is_recording()) {
        return -EINVAL;
    }

    /* Get current statistics */
    if (audio_get_stats(&stats) != 0) {
        return -EIO;
    }

    /* Calculate recording time in seconds */
    recording_sec = stats.recording_time_ms / 1000;

    /* Add bookmark via storage module */
    return storage_add_bookmark(current_session_id, recording_sec);
}

int audio_get_stats(struct audio_stats *stats_out)
{
    if (!stats_out) {
        return -EINVAL;
    }

    /* Calculate average if recording stopped */
    if (!recording_active && stats.frames_encoded > 0) {
        stats.encode_time_avg_us = (uint32_t)(encode_time_total_us / stats.frames_encoded);
    }

    memcpy(stats_out, &stats, sizeof(stats));
    return 0;
}

void audio_register_data_callback(audio_data_callback_t callback, void *user_data)
{
    data_callback = callback;
    data_callback_user_data = user_data;
}

const char *audio_get_session_id(void)
{
    k_mutex_lock(&audio_state_mutex, K_FOREVER);

    /* Return session_id if it's set, even if recording not fully started yet
     * This allows AT+START to return session_id immediately */
    if (current_session_id[0] == '\0') {
        k_mutex_unlock(&audio_state_mutex);
        return NULL;
    }

    /* Copy to static buffer for thread safety */
    static char session_id_copy[STORAGE_SESSION_ID_LEN];
    strncpy(session_id_copy, current_session_id, sizeof(session_id_copy) - 1);
    session_id_copy[sizeof(session_id_copy) - 1] = '\0';

    k_mutex_unlock(&audio_state_mutex);
    return session_id_copy;
}

bool audio_is_paused(void)
{
    k_mutex_lock(&audio_state_mutex, K_FOREVER);
    bool paused = is_paused;
    k_mutex_unlock(&audio_state_mutex);

    return paused;
}

/* Internal recording initialization */
static int audio_start_recording_internal(enum audio_mode mode)
{
    int ret;
    struct clip_context *c = clip_get_context();

    /* Ensure DWT cycle counter is enabled for timing measurement */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Reset statistics */
    memset(&stats, 0, sizeof(stats));
    stats.encode_time_min_us = UINT32_MAX;
    encode_time_total_us = 0;
    dsp_time_total_us = 0;

    /* Reset file index */
    current_file_index = 1;
    file_start_frame_count = 0;

    /* Create storage session */
    uint8_t channels = (mode == AUDIO_MODE_STEREO) ? 2 : 1;
    const char *mode_str = (mode == AUDIO_MODE_STEREO) ? "stereo" : "mono";
    ret = storage_create_session(current_session_id, channels,
                                 AUDIO_SAMPLE_RATE, mode_str);
    if (ret != 0 && ret != -EEXIST) {
        LOG_WRN("Failed to create storage session: %d", ret);
        /* Continue anyway - storage is optional */
    }

    /* Create first file */
    ret = storage_create_file(&current_storage_file, current_session_id, current_file_index);
    if (ret != 0) {
        LOG_WRN("Failed to create storage file: %d", ret);
        /* Continue anyway - storage is optional */
    } else {
        file_start_frame_count = 0;
        LOG_DBG("Recording to: %s", current_storage_file.filename);
    }

    /* Boost CPU to 128MHz for encoding, before mic power-on.
     * The 10ms mic stabilization delay also serves as CPU frequency
     * settling time.
     */
    LOG_INF("Boosting CPU for recording");
    clip_cpu_boost_acquire();

    /* Power on microphone with delay for stabilization */
    ret = mic_power_on();
    if (ret < 0) {
        LOG_ERR("Failed to power on microphone: %d", ret);
        clip_cpu_boost_release();
        return ret;
    }

    /* Initialize Opus encoder (only if not initialized or params changed) */
    ret = init_opus_encoder();
    if (ret < 0) {
        LOG_ERR("Failed to init Opus encoder: %d", ret);
        mic_power_off();
        clip_cpu_boost_release();
        return ret;
    }

#ifdef CONFIG_SPEEXDSP
    /* Initialize/Reinitialize SpeexDSP - enhanced mode only */
    if (c->config.mode == MODE_ENHANCED && c->config.noise_suppress > 0) {
        speex_enabled = true;
        ret = init_speex_preprocessor();
        if (ret < 0) {
            LOG_WRN("SpeexDSP init failed: %d", ret);
            speex_enabled = false;
        }
    } else {
        /* STEREO mode or no noise suppression - cleanup DSP if exists */
        if (speex_enabled || speex_pp) {
            cleanup_speex_preprocessor();
            speex_enabled = false;
        }
    }
#endif

    /* Start DMIC */
    ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
    if (ret < 0) {
        LOG_ERR("Failed to start DMIC: %d", ret);
        mic_power_off();
        clip_cpu_boost_release();
        return ret;
    }

    /* Discard initial frames to eliminate mic power-up pop */
    dmic_flush_initial();

    recording_active = true;
    is_paused = false;

    /* Calculate actual bitrate for logging */
    uint32_t actual_bitrate = (current_mode == AUDIO_MODE_STEREO) ?
        CONFIG_CLIP_NORMAL_BITRATE * 2 : CONFIG_CLIP_ENHANCED_BITRATE;
    LOG_INF("Recording started: %s mode, %u kbps, session=%s",
        (current_mode == AUDIO_MODE_STEREO) ? "stereo" : "mono",
        actual_bitrate/1000, current_session_id);
    return 0;
}

/* Internal stop function */
static int audio_stop_recording_internal(void)
{
    int ret;
    uint32_t duration_sec;

    if (!recording_active) {
        return 0;
    }

    recording_active = false;
    is_paused = false;

    /* Stop DMIC */
    ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
    if (ret < 0) {
        LOG_ERR("Failed to stop DMIC: %d", ret);
    }

    /* Close storage file if open */
    if (current_storage_file.is_open) {
        ret = storage_close_file(&current_storage_file);
        if (ret != 0) {
            LOG_WRN("Failed to close storage file: %d", ret);
        }
    }

    /* Calculate average encode time */
    if (stats.frames_encoded > 0) {
        stats.encode_time_avg_us = (uint32_t)(encode_time_total_us / stats.frames_encoded);
    }

    /* Close storage session */
    duration_sec = stats.recording_time_ms / 1000;
    ret = storage_close_session(current_session_id, duration_sec, current_file_index);
    if (ret != 0) {
        LOG_WRN("Failed to close storage session: %d", ret);
    }

    /* Power off microphone to save power */
    mic_power_off();

    LOG_INF("Recording stopped, releasing CPU boost");
    clip_cpu_boost_release();

    /* Note: Keep encoder and DSP initialized for next recording
     * They will be reinitialized only if parameters change */

    return 0;
}

/* Audio recording thread */
void audio_recording_thread(void *p1, void *p2, void *p3)
{
    int ret;
    void *buffer = NULL;
    uint32_t size;
    uint16_t year;
    uint8_t month, day, hour, min, sec;
    int dmic_timeout_count = 0;  /* For DMIC recovery */

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (true) {
        /* Wait for start signal (blocks here) */
        k_sem_take(&audio_start_sem, K_FOREVER);

        /* Check if stop was requested before we started */
        if (stop_requested) {
            stop_requested = false;
            continue;
        }

        /* Perform initialization in audio thread context */
        ret = audio_start_recording_internal(current_mode);
        if (ret < 0) {
            LOG_ERR("Failed to start recording: %d", ret);
            continue;
        }

        /* Recording loop - process audio until stop requested */
        while (!stop_requested) {
            /* Check for pause request */
            if (pause_requested) {
                pause_requested = false;

                /* Close current file */
                if (current_storage_file.is_open) {
                    ret = storage_close_file(&current_storage_file);
                    if (ret != 0) {
                        LOG_WRN("Failed to close file on pause: %d", ret);
                    }
                }

                /* Stop DMIC and mic power to save power */
                dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
                mic_power_off();

                is_paused = true;
                LOG_INF("Recording paused at %llu sec", stats.recording_time_ms / 1000);

                /* Wait for resume signal */
                while (is_paused && !stop_requested) {
                    k_sleep(K_MSEC(100));
                }

                /* Check if stop was requested while paused */
                if (stop_requested) {
                    break;
                }

                /* Resume: Create new file with incremented index */
                current_file_index++;
                ret = storage_create_file(&current_storage_file, current_session_id, current_file_index);
                if (ret != 0) {
                    LOG_ERR("Failed to create new file for resume: %d", ret);
                    current_file_index--;
                    break;
                }

                file_start_frame_count = recording_frame_count;

                /* Power on mic and restart DMIC */
                mic_power_on();
                ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
                if (ret < 0) {
                    LOG_ERR("Failed to restart DMIC: %d", ret);
                    break;
                }

                /* Discard initial frames for DMIC settling */
                dmic_flush_initial();

                LOG_INF("Recording resumed: new file #%u", current_file_index);
                continue;
            }

            /* Read one audio block from DMIC */
            ret = dmic_read(dmic_dev, 0, &buffer, &size, AUDIO_FRAME_MS*2);
            if (ret < 0) {
                if (ret == -EAGAIN) {
                    /* Timeout - track consecutive timeouts for recovery */
                    dmic_timeout_count++;

                    if (dmic_timeout_count >= 5) {
                        LOG_WRN("DMIC timeout recovery: retriggering DMIC");
                        dmic_timeout_count = 0;

                        /* Try to recover DMIC */
                        dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
                        k_sleep(K_MSEC(10));
                        ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
                        if (ret < 0) {
                            LOG_ERR("DMIC recovery failed: %d", ret);
                        } else {
                            dmic_flush_initial();
                            LOG_INF("DMIC recovered");
                        }
                    }
                    continue;
                }
                LOG_ERR("DMIC read error: %d", ret);
                stats.dropped_frames++;
                break;
            }

            /* Reset timeout counter on successful read */
            dmic_timeout_count = 0;

            /* Validate block size */
            if (size != AUDIO_BLOCK_SIZE) {
                LOG_WRN("Invalid block size: %u (expected %u)", size, AUDIO_BLOCK_SIZE);
                k_mem_slab_free(&audio_mem_slab, buffer);
                buffer = NULL;
                stats.dropped_frames++;
                continue;
            }

            /* Process PCM data according to mode (DSP: merge, denoise, dereverb) */
            uint32_t dsp_cyc = cyc_start();
            int16_t *pcm_data = process_pcm_frame((int16_t *)buffer, AUDIO_OPUS_FRAME_SIZE);
            uint32_t dsp_us = cyc_to_us(cyc_end(dsp_cyc));
            dsp_time_total_us += dsp_us;

            /* Check encoder state */
            if (!opus_encoder) {
                LOG_ERR("Opus encoder is NULL!");
                k_mem_slab_free(&audio_mem_slab, buffer);
                stats.dropped_frames++;
                continue;
            }

            /* Measure encode time with DWT cycle counter */
            uint32_t enc_cyc = cyc_start();

            /* Encode audio */
            opus_int32 encoded_bytes = opus_encode(
                opus_encoder,
                pcm_data,
                AUDIO_OPUS_FRAME_SIZE,
                opus_packet,
                AUDIO_MAX_PACKET_SIZE
            );

            /* Calculate encode time in microseconds */
            uint32_t encode_us = cyc_to_us(cyc_end(enc_cyc));

            /* Free buffer */
            k_mem_slab_free(&audio_mem_slab, buffer);
            buffer = NULL;

            if (encoded_bytes < 0) {
                LOG_ERR("Opus encode error: %d", encoded_bytes);
                stats.dropped_frames++;
                continue;
            }

            /* Update statistics */
            stats.frames_encoded++;
            stats.recording_time_ms = stats.frames_encoded * AUDIO_FRAME_MS;
            stats.total_bytes += encoded_bytes;
            recording_frame_count++;

            /* Update encode time statistics */
            encode_time_total_us += encode_us;
            if (encode_us < stats.encode_time_min_us) {
                stats.encode_time_min_us = encode_us;
            }
            if (encode_us > stats.encode_time_max_us) {
                stats.encode_time_max_us = encode_us;
            }

            /* Print encode stats every 10 seconds (500 frames at 20ms/frame) */
            if (stats.frames_encoded % 500 == 0) {
                uint32_t avg_enc = (uint32_t)(encode_time_total_us / stats.frames_encoded);
                uint32_t avg_dsp = (uint32_t)(dsp_time_total_us / stats.frames_encoded);
                LOG_INF("Encode: avg=%u us, min=%u, max=%u | DSP: avg=%u us (%u frames)",
                        avg_enc, stats.encode_time_min_us, stats.encode_time_max_us,
                        avg_dsp, stats.frames_encoded);
            }

            /* Calculate energy level for visualization.
             * Skip when BLE vis not subscribed AND display is in REC_DOT
             * (no display animation needs the data). Always calculate when
             * BLE client is subscribed or display shows REC_WAVE.
             */
            if (ble_is_audio_vis_subscribed() ||
                display_get_state() == UI_STATE_REC_WAVE) {
                calculate_energy_level(pcm_data, AUDIO_OPUS_FRAME_SIZE);
            }

            /* Write encoded data to storage (with 2-byte length header) */
            if (current_storage_file.is_open) {
                ret = storage_write_frame(&current_storage_file, opus_packet, encoded_bytes);
                if (ret != 0) {
                    LOG_ERR("Storage write error: %d", ret);
                    /* Close file on error */
                    storage_close_file(&current_storage_file);
                    memset(&current_storage_file, 0, sizeof(current_storage_file));
                }
            }

            /* Check if we need to create a new segment file
             * Use different segment durations based on transfer state:
             * - When transferring: shorter segments (CLIP_AUDIO_SEGMENT_DURATION_SYNC)
             * - When not transferring: longer segments (CLIP_AUDIO_SEGMENT_DURATION_NO_SYNC)
             */
            bool is_transferring = transfer_is_active();
            uint32_t segment_duration_sec;

            if (is_transferring) {
                segment_duration_sec = CLIP_AUDIO_SEGMENT_DURATION_SYNC;
            } else {
                segment_duration_sec = CLIP_AUDIO_SEGMENT_DURATION_NO_SYNC;
            }

            /* Calculate frames per file based on current segment duration */
            uint32_t frames_per_file = segment_duration_sec * (1000 / AUDIO_FRAME_MS);

            /* Check for state transition: not transferring -> transferring
             * If current file duration exceeds sync segment duration, slice immediately
             */
            if (was_transferring == false && is_transferring == true && recording_frame_count > 1) {
                uint32_t current_file_frames = recording_frame_count - file_start_frame_count;
                uint32_t sync_frames_per_file = CLIP_AUDIO_SEGMENT_DURATION_SYNC * (1000 / AUDIO_FRAME_MS);

                if (current_file_frames >= sync_frames_per_file) {
                    /* File exceeds sync duration, slice immediately */
                    goto create_new_segment;
                }
            }

            /* Update transfer state tracking */
            was_transferring = is_transferring;

            /* Regular segment check based on current segment duration */
            if (recording_frame_count > 1 && (recording_frame_count - file_start_frame_count) >= frames_per_file) {
create_new_segment:
                /* Close current file */
                if (current_storage_file.is_open) {
                    uint32_t segment_frames = recording_frame_count - file_start_frame_count;
                    uint32_t segment_bytes = current_storage_file.bytes_written;

                    ret = storage_close_file(&current_storage_file);
                    if (ret != 0) {
                        LOG_WRN("Failed to close segment file: %d", ret);
                    }

                    LOG_INF("Segment: file #%u (%u frames, %u bytes, %us)",
                            current_file_index, segment_frames, segment_bytes,
                            segment_duration_sec);
                }

                /* Create new file */
                current_file_index++;
                ret = storage_create_file(&current_storage_file, current_session_id, current_file_index);
                if (ret != 0) {
                    LOG_ERR("Failed to create new segment file: %d", ret);
                    current_file_index--;
                    /* Continue recording without storage */
                } else {
                    file_start_frame_count = recording_frame_count;
                }
            }

            k_yield();
        }

        /* Perform cleanup in audio thread context */
        audio_stop_recording_internal();
        stop_requested = false;
        pause_requested = false;

        /* Signal that stop is complete */
        k_sem_give(&stop_done_sem);

        LOG_INF("Recording stopped: %u frames, %llu sec, %u KB",
            stats.frames_encoded,
            stats.recording_time_ms / 1000,
            stats.total_bytes / 1024);
    }
}

/* Internal functions */
static bool encoder_params_changed(void)
{
    /* Calculate current parameters based on mode */
    uint32_t actual_bitrate = (current_mode == AUDIO_MODE_STEREO) ?
        CONFIG_CLIP_NORMAL_BITRATE * 2 : CONFIG_CLIP_ENHANCED_BITRATE;
    uint8_t effective_complexity = (current_mode == AUDIO_MODE_STEREO) ?
        CONFIG_CLIP_NORMAL_COMPLEXITY : CONFIG_CLIP_ENHANCED_COMPLEXITY;

    /* Check if parameters changed */
    if (!encoder_params.initialized) {
        return true;
    }

    if (encoder_params.bitrate != actual_bitrate ||
        encoder_params.complexity != effective_complexity ||
        encoder_params.channels != opus_channels) {
        return true;
    }

    return false;
}

static int init_opus_encoder(void)
{
    int err;
    struct clip_context *c = clip_get_context();

    /* Calculate current parameters based on mode */
    uint32_t actual_bitrate = (current_mode == AUDIO_MODE_STEREO) ?
        CONFIG_CLIP_NORMAL_BITRATE * 2 : CONFIG_CLIP_ENHANCED_BITRATE;
    uint8_t effective_complexity = (current_mode == AUDIO_MODE_STEREO) ?
        CONFIG_CLIP_NORMAL_COMPLEXITY : CONFIG_CLIP_ENHANCED_COMPLEXITY;

    /* Check if reinitialization is needed */
    if (opus_encoder && !encoder_params_changed()) {
        /* Parameters unchanged, no need to reinit */
        return 0;
    }

    /* Destroy old encoder if exists */
    if (opus_encoder) {
        cleanup_opus_encoder();
    }

    /* Create Opus encoder */
    opus_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, opus_channels,
                       OPUS_APPLICATION_VOIP, &err);
    if (!opus_encoder) {
        LOG_ERR("Failed to create Opus encoder: %d", err);
        return err;
    }

    /* Set bitrate */
    opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE((opus_int32)actual_bitrate));

    /* Enable VBR with unconstrained quality */
    opus_encoder_ctl(opus_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(opus_encoder, OPUS_SET_VBR_CONSTRAINT(0));

    /* Set complexity from Kconfig (mode-specific) */
    err = opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(effective_complexity));
    if (err != OPUS_OK) {
        LOG_ERR("Failed to set complexity: %d", err);
        return err;
    }

    /* Voice-optimized signal */
    opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    /* Input sample depth */
    opus_encoder_ctl(opus_encoder, OPUS_SET_LSB_DEPTH(16));

    /* Disable DTX, FEC, packet loss */
    opus_encoder_ctl(opus_encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(opus_encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(opus_encoder, OPUS_SET_PACKET_LOSS_PERC(0));
    

    /* Update cached parameters */
    encoder_params.bitrate = actual_bitrate;
    encoder_params.complexity = effective_complexity;
    encoder_params.channels = opus_channels;
    encoder_params.initialized = true;

    LOG_INF("Opus encoder ready: %d ch, %u kbps, complexity=%u, %s",
        opus_channels, actual_bitrate / 1000, effective_complexity,
        (current_mode == AUDIO_MODE_STEREO) ? "stereo" : "mono");

    return 0;
}

static void cleanup_opus_encoder(void)
{
    if (opus_encoder) {
        opus_encoder_destroy(opus_encoder);
        opus_encoder = NULL;
    }
}

#ifdef CONFIG_SPEEXDSP
static bool dsp_params_changed(void)
{
    struct clip_context *c = clip_get_context();

    if (!dsp_params.initialized) {
        return true;
    }

    if (dsp_params.noise_suppress != c->config.noise_suppress ||
        dsp_params.dereverb_enabled != c->config.dereverb_enabled) {
        return true;
    }

    return false;
}

static int init_speex_preprocessor(void)
{
    struct clip_context *c = clip_get_context();

    /* Check if reinitialization is needed */
    if (speex_pp && !dsp_params_changed()) {
        /* Parameters unchanged, no need to reinit */
        return 0;
    }

    /* Destroy old preprocessor if exists */
    if (speex_pp) {
        cleanup_speex_preprocessor();
    }

    /* Create preprocessor state */
    speex_pp = speex_preprocess_state_init(AUDIO_OPUS_FRAME_SIZE, AUDIO_SAMPLE_RATE);
    if (!speex_pp) {
        LOG_ERR("Failed to create SpeexDSP preprocessor");
        return -ENOMEM;
    }

    /* Noise suppression (Kconfig) */
    int denoise = CONFIG_CLIP_DEFAULT_NOISE;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &denoise);

    /* Dereverberation (Kconfig) */
    int dereverb = IS_ENABLED(CONFIG_CLIP_DEFAULT_DEREVERB) ? 1 : 0;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_DEREVERB, &dereverb);

    int dereverb_level = 40;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_DEREVERB_LEVEL, &dereverb_level);

    int dereverb_decay = 20;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_DEREVERB_DECAY, &dereverb_decay);

    /* AGC for far-field pickup (Kconfig) */
    int agc = 1;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_AGC, &agc);
    float agc_level = 8000.0f;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_level);
    int agc_max_gain = CONFIG_CLIP_AGC_MAX_GAIN;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &agc_max_gain);
    int agc_target = CONFIG_CLIP_AGC_TARGET;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_AGC_TARGET, &agc_target);
    int agc_increment = CONFIG_CLIP_AGC_INCREMENT;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_AGC_INCREMENT, &agc_increment);
    int agc_decrement = CONFIG_CLIP_AGC_DECREMENT;
    speex_preprocess_ctl(speex_pp, SPEEX_PREPROCESS_SET_AGC_DECREMENT, &agc_decrement);

    dsp_params.initialized = true;

    LOG_INF("SpeexDSP ready: noise=%d dB, dereverb=%d, agc=1, max_gain=%d",
            CONFIG_CLIP_DEFAULT_NOISE,
            IS_ENABLED(CONFIG_CLIP_DEFAULT_DEREVERB) ? 1 : 0,
            CONFIG_CLIP_AGC_MAX_GAIN);

    return 0;
}

static void cleanup_speex_preprocessor(void)
{
    if (speex_pp) {
        speex_preprocess_state_destroy(speex_pp);
        speex_pp = NULL;
    }
}
#endif

static int16_t *process_pcm_frame(int16_t *stereo_input, int frame_size)
{
    /* stereo_input layout: L0, R0, L1, R1, L2, R2, ... */

    if (current_mode == AUDIO_MODE_STEREO) {
        /* STEREO mode: return original stereo data (no DSP) */
        return stereo_input;

    } else {  /* AUDIO_MODE_MERGE */
        /* Enhanced mode: Mix left and right: (L + R) / 2, then apply DSP */
        for (int i = 0; i < frame_size; i++) {
            int32_t left = stereo_input[i * 2];
            int32_t right = stereo_input[i * 2 + 1];
            int32_t mixed = (left + right) / 2;
            /* Clamp to int16 range */
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            processed_buffer[i] = (int16_t)mixed;
        }

#ifdef CONFIG_SPEEXDSP
        /* Apply SpeexDSP processing (noise suppression, AGC, dereverb) */
        if (speex_enabled && speex_pp) {
            speex_preprocess_run(speex_pp, processed_buffer);
        }
#endif

        return processed_buffer;
    }
}

static int mic_power_on(void)
{
    if (mic_regulator) {
        int ret = regulator_enable(mic_regulator);
        if (ret) {
            LOG_WRN("Mic regulator enable failed: %d", ret);
            return ret;
        }
    }
    return 0;
}

static int mic_power_off(void)
{
    if (mic_regulator) {
        int ret = regulator_disable(mic_regulator);
        if (ret) {
            LOG_WRN("Mic regulator disable failed: %d", ret);
            return ret;
        }
    }
    return 0;
}

/* Flush initial DMIC frames to discard mic power-up transients */
#define DMIC_FLUSH_FRAMES CONFIG_CLIP_AUDIO_DMIC_FLUSH_FRAMES

static void dmic_flush_initial(void)
{
    void *buf;
    uint32_t sz;

    for (int i = 0; i < DMIC_FLUSH_FRAMES; i++) {
        if (dmic_read(dmic_dev, 0, &buf, &sz, AUDIO_FRAME_MS * 2) == 0) {
            k_mem_slab_free(&audio_mem_slab, buf);
        }
    }
}

/* ========================================
 * Audio Visualization
 * ======================================== */

/**
 * @brief Calculate energy level from PCM data
 *
 * Calculates the RMS energy of the entire audio frame and maps it to 0-10 range.
 * Sends audio visualization data via BLE every ~200ms.
 *
 * @param pcm_data PCM audio data (mono or stereo interleaved)
 * @param frame_size Number of samples in pcm_data
 */
static void calculate_energy_level(int16_t *pcm_data, int frame_size)
{
    int64_t sum_squares = 0;

    /* Calculate RMS energy for the entire frame */
    for (int i = 0; i < frame_size; i++) {
        int16_t sample;

        /* Handle stereo vs mono */
        if (current_mode == AUDIO_MODE_STEREO) {
            /* Average of L+R channels */
            int16_t left = pcm_data[i * 2];
            int16_t right = pcm_data[i * 2 + 1];
            sample = (left + right) / 2;
        } else {
            /* Mono or merged - already processed */
            sample = pcm_data[i];
        }

        sum_squares += (int64_t)sample * sample;
    }

    /* Calculate RMS */
    int32_t rms = (int32_t)sqrt((float)sum_squares / frame_size);

    /* Map RMS to 0-10 range using logarithmic scale
     * Adjusted for microphone noise floor (~300-400 RMS)
     * - rms < 256: level 0 (below noise floor)
     * - rms 256-511: level 1 (noise floor)
     * - rms 512-1023: level 2 (quiet speech)
     * - rms 1024-2047: level 3 (normal speech)
     * - rms 2048-4095: level 4 (loud speech)
     * - rms 4096-8191: level 5 (very loud)
     * - rms 8192-16383: level 6
     * - rms 16384-32767: level 7
     * - rms 32768-65535: level 8
     * - rms 65535-131071: level 9
     * - rms >= 131072: level 10
     */
    uint8_t new_level;
    if (rms < 256) {
        new_level = 0;
    } else if (rms < 512) {
        new_level = 1;
    } else if (rms < 1024) {
        new_level = 2;
    } else if (rms < 2048) {
        new_level = 3;
    } else if (rms < 4096) {
        new_level = 4;
    } else if (rms < 8192) {
        new_level = 5;
    } else if (rms < 16384) {
        new_level = 6;
    } else if (rms < 32768) {
        new_level = 7;
    } else if (rms < 65536) {
        new_level = 8;
    } else if (rms < 131072) {
        new_level = 9;
    } else {
        new_level = 10;
    }

    /* Update shared data with mutex protection */
    k_mutex_lock(&audio_energy_mutex, K_FOREVER);
    audio_energy_level = new_level;

    /* Update energy history for trend display */
    audio_energy_history[audio_energy_history_index] = new_level;
    audio_energy_history_index = (audio_energy_history_index + 1) % 13;
    if (audio_energy_history_index == 0) {
        audio_energy_history_filled = true;
    }

    k_mutex_unlock(&audio_energy_mutex);

    /* Send audio visualization data via BLE (every ~200ms)
     * Pack 13 values (4 bits each) into 7 bytes (oldest to newest)
     * Byte format: [val0:val1] [val2:val3] [val4:val5] [val6:val7]
     *              [val8:val9] [val10:val11] [val12:0x0] */
    static int64_t last_vis_send = 0;
    static int vis_count = 0;
    static int vis_ok = 0;
    static int vis_enotconn = 0;
    static int vis_err_other = 0;
    int64_t now = k_uptime_get();
    if (now - last_vis_send >= 200) {
        uint8_t history_copy[13];
        uint8_t packed[7];

        /* Copy history in order: oldest to newest */
        k_mutex_lock(&audio_energy_mutex, K_FOREVER);
        if (audio_energy_history_filled) {
            /* History array has wrapped, copy from current index */
            int idx = 0;
            for (int i = 0; i < 13; i++) {
                history_copy[idx++] = audio_energy_history[(audio_energy_history_index + i) % 13];
            }
        } else {
            /* Not filled yet, copy from start */
            memcpy(history_copy, audio_energy_history, audio_energy_history_index);
            /* Fill remaining with current value */
            for (int i = audio_energy_history_index; i < 13; i++) {
                history_copy[i] = new_level;
            }
        }
        k_mutex_unlock(&audio_energy_mutex);

        /* Pack 13 values (4 bits each) into 7 bytes */
        for (int i = 0; i < 6; i++) {
            packed[i] = (history_copy[i * 2] & 0x0F) | ((history_copy[i * 2 + 1] & 0x0F) << 4);
        }
        packed[6] = history_copy[12] & 0x0F;  /* Last value in low nibble */

        extern int ble_send_audio_vis(const uint8_t *data, uint16_t len);
        int vis_err = ble_send_audio_vis(packed, 7);
        vis_count++;
        if (vis_err == 0) {
            vis_ok++;
        } else if (vis_err == -ENOTCONN) {
            vis_enotconn++;
        } else {
            vis_err_other++;
        }
        last_vis_send = now;

        /* Periodic diagnostic report every ~5 seconds (25 sends) */
        if (vis_count % 25 == 0) {
            LOG_INF("AudioVis[%d]: ok=%d enotconn=%d err=%d sub=%d lvl=%d",
                    vis_count, vis_ok, vis_enotconn, vis_err_other,
                    ble_is_audio_vis_subscribed(), new_level);
            vis_ok = 0;
            vis_enotconn = 0;
            vis_err_other = 0;
        }
    }
}

int audio_get_energy_level(void)
{
    int level;
    k_mutex_lock(&audio_energy_mutex, K_FOREVER);
    level = audio_energy_level;
    k_mutex_unlock(&audio_energy_mutex);
    return level;
}
