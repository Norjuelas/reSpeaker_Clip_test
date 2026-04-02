/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "transfer.h"
#include "storage.h"
#include "transport.h"
#include "clip.h"
#include "ble.h"
#include "audio.h"

LOG_MODULE_REGISTER(transfer, CONFIG_CLIP_LOG_LEVEL);

/* Transfer thread stack - uses Kconfig value */
K_THREAD_STACK_DEFINE(transfer_thread_stack, CONFIG_CLIP_TRANSFER_STACK_SIZE);

/* Static chunk buffer - avoids stack allocation for large buffer */
static uint8_t chunk_buffer[CONFIG_CLIP_TRANSFER_CHUNK_SIZE];

/* Transfer thread state */
static struct k_thread transfer_thread_data;
static k_tid_t transfer_thread_id;
static K_SEM_DEFINE(transfer_trigger_sem, 0, 1);
static volatile bool transfer_thread_running = false;
static volatile bool transfer_thread_waiting = false;
static volatile bool transfer_thread_ready = false;

/* Transfer state */
static struct transfer_info current_transfer = {0};
static struct fs_file_t transfer_file;
static bool transfer_file_open = false;
static struct transport *current_transport = NULL;  /* Transport for current transfer */

/* Track last successfully transferred file */
static char last_transferred_file[32] = {0};

/* Transfer rate tracking */
static int64_t file_transfer_start_ms = 0;

/* Transfer control flags */
static volatile bool transfer_pause_requested = false;
static volatile bool transfer_cancel_requested = false;
static volatile bool transfer_complete_sent = false;

/* Forward declarations */
static void transfer_thread_main(void *, void *, void *);
static int transfer_next_file(void);
static int transfer_send_chunk(void);
static void transfer_cleanup(void);
static void send_file_ready_event(const char *session_id, const char *filename, uint64_t size);
static int send_file_complete_event(const char *filename);
static void send_transfer_complete_once(const char *session_id, int file_count);
static void generate_filename(uint32_t file_num, char *filename);

/**
 * @brief Generate filename from file number
 *
 * Files are numbered sequentially: 0001.opus, 0002.opus, etc.
 */
static void generate_filename(uint32_t file_num, char *filename)
{
    snprintf(filename, 16, "%04u.opus", file_num);
}

int transfer_init(void)
{
    memset(&current_transfer, 0, sizeof(current_transfer));
    memset(last_transferred_file, 0, sizeof(last_transferred_file));

    /* Set thread running flag BEFORE creating thread */
    transfer_thread_running = true;

    /* Create transfer thread */
    transfer_thread_id = k_thread_create(&transfer_thread_data,
                                         transfer_thread_stack,
                                         K_KERNEL_STACK_SIZEOF(transfer_thread_stack),
                                         transfer_thread_main,
                                         NULL, NULL, NULL,
                                         CONFIG_CLIP_TRANSFER_THREAD_PRIORITY,
                                         0, K_NO_WAIT);
    if (transfer_thread_id == NULL) {
        LOG_ERR("Failed to create transfer thread");
        transfer_thread_running = false;
        return -ENOMEM;
    }
    k_thread_name_set(&transfer_thread_data, "transfer");
    LOG_INF("Transfer thread created, waiting for ready flag...");

    /* Wait a bit for thread to initialize */
    k_sleep(K_MSEC(100));

    return 0;
}

int transfer_start(const char *session_id, const char *filename, struct transport *tp)
{
    int err;

    /* Use active transport if none specified */
    if (!tp) {
        tp = transport_get_active();
        if (!tp) {
            LOG_ERR("No transport available for transfer");
            return -ENOTCONN;
        }
    }

    /* Save the transport for this transfer session */
    current_transport = tp;
    LOG_INF("Transfer using transport type %d", tp->type);

    clip_cpu_boost_acquire();
    /* Check if transfer is already active */
    if (transfer_is_active() && transfer_file_open) {
        LOG_WRN("Transfer already active");
        return -EBUSY;
    }

    /* Wait for any previous transfer to complete */
    int retry_count = 0;
    while (current_transfer.state == TRANSFER_STATE_TRANSMITTING) {
        if (transfer_file_open) {
            LOG_WRN("Transfer in progress with file open");
            return -EBUSY;
        }

        if (++retry_count > 20) {
            LOG_WRN("Transfer cleanup timeout - forcing cleanup");
            transfer_cleanup();
            break;
        }
        k_sleep(K_MSEC(100));
    }

    /* Wait for transfer thread to be ready */
    if (!transfer_thread_ready) {
        int retry = 0;
        while (!transfer_thread_ready && retry < 100) {
            k_sleep(K_MSEC(10));
            retry++;
        }
        if (!transfer_thread_ready) {
            LOG_ERR("Transfer thread not ready after timeout");
            return -ETIMEDOUT;
        }
    }

    if (!storage_is_mounted()) {
        LOG_ERR("SD card not mounted");
        return -ENODEV;
    }

    /* Check if session exists */
    struct storage_session_info session_info;
    if (storage_get_session_info(session_id, &session_info) != 0) {
        LOG_ERR("Session not found: %s", session_id);
        return -ENOENT;
    }

    transfer_complete_sent = false;
    transfer_pause_requested = false;

    /* Initialize transfer state */
    memset(&current_transfer, 0, sizeof(current_transfer));
    memset(last_transferred_file, 0, sizeof(last_transferred_file));
    current_transfer.state = TRANSFER_STATE_TRANSMITTING;
    current_transfer.direction = TRANSFER_DIR_UPLOAD;

    strncpy(current_transfer.session_id, session_id, sizeof(current_transfer.session_id) - 1);

    if (filename) {
        /* Transfer single file */
        char filepath[128];
        struct fs_dirent entry;

        snprintf(filepath, sizeof(filepath), "/SD:/REC/%s/%s", session_id, filename);
        if (fs_stat(filepath, &entry) != 0) {
            LOG_ERR("File not found: %s", filepath);
            return -ENOENT;
        }

        strncpy(current_transfer.current_file, filename, sizeof(current_transfer.current_file) - 1);
        current_transfer.total_files = 1;
        current_transfer.total_bytes = entry.size;
        current_transfer.continuous = false;  /* Single file - not continuous */
        LOG_INF("Transfer: %s (%u KB)", filename, (uint32_t)entry.size/1024);
    } else {
        /* Transfer entire session */
        struct storage_session_info session_info;
        err = storage_get_session_info(session_id, &session_info);
        if (err < 0) {
            LOG_ERR("Failed to get session info: %d", err);
            return err;
        }
        current_transfer.total_files = session_info.file_count;
        current_transfer.total_bytes = session_info.total_bytes;
        current_transfer.file_index = 0;
        current_transfer.first_file_num = 1;
        current_transfer.last_file_num = session_info.file_count;

        /* Check if this session is currently being recorded (continuous mode) */
        const char *recording_session = audio_get_session_id();
        current_transfer.continuous = (recording_session != NULL &&
                                       strcmp(recording_session, session_id) == 0);

        LOG_INF("Transfer: %s (%u files, %s)",
                session_id, current_transfer.total_files,
                current_transfer.continuous ? "continuous" : "normal");
    }

    /* Start transfer thread */
    transfer_thread_running = true;
    k_sem_give(&transfer_trigger_sem);

    return 0;
}

int transfer_resume_from(const char *session_id, const char *start_file, struct transport *tp)
{
    /* Use active transport if none specified */
    if (!tp) {
        tp = transport_get_active();
        if (!tp) {
            LOG_ERR("No transport available for transfer");
            return -ENOTCONN;
        }
    }

    /* Wait for any previous transfer to complete */
    int retry_count = 0;
    while (current_transfer.state == TRANSFER_STATE_TRANSMITTING) {
        if (transfer_file_open) {
            return -EBUSY;
        }

        if (++retry_count > 50) {
            LOG_WRN("Transfer cleanup timeout");
            transfer_cleanup();
            break;
        }
        k_sleep(K_MSEC(100));
    }

    if (transfer_is_active()) {
        return -EBUSY;
    }

    /* Wait for transfer thread to be ready */
    if (!transfer_thread_ready) {
        int retry = 0;
        while (!transfer_thread_ready && retry < 100) {
            k_sleep(K_MSEC(10));
            retry++;
        }
        if (!transfer_thread_ready) {
            LOG_ERR("Transfer thread not ready after timeout");
            return -ETIMEDOUT;
        }
    }

    if (!storage_is_mounted()) {
        LOG_ERR("SD card not mounted");
        return -ENODEV;
    }

    if (!session_id || !start_file) {
        return -EINVAL;
    }

    struct storage_session_info tmp_info;
    if (storage_get_session_info(session_id, &tmp_info) != 0) {
        LOG_ERR("Session not found: %s", session_id);
        return -ENOENT;
    }

    transfer_complete_sent = false;
    transfer_pause_requested = false;

    /* Save the transport for this transfer session */
    current_transport = tp;
    LOG_INF("Transfer using transport type %d", tp->type);

    clip_cpu_boost_acquire();

    /* Initialize transfer state */
    memset(&current_transfer, 0, sizeof(current_transfer));
    current_transfer.state = TRANSFER_STATE_TRANSMITTING;
    current_transfer.direction = TRANSFER_DIR_UPLOAD;
    strncpy(current_transfer.session_id, session_id, sizeof(current_transfer.session_id) - 1);

    /* Get total file count */
    struct storage_session_info session_info;
    int err = storage_get_session_info(session_id, &session_info);
    if (err < 0) {
        LOG_ERR("Failed to get session info: %d", err);
        return err;
    }

    current_transfer.total_files = session_info.file_count;
    current_transfer.total_bytes = session_info.total_bytes;
    current_transfer.first_file_num = 1;
    current_transfer.last_file_num = session_info.file_count;

    /* Check if this session is currently being recorded (continuous mode) */
    const char *recording_session = audio_get_session_id();
    current_transfer.continuous = (recording_session != NULL &&
                                   strcmp(recording_session, session_id) == 0);

    /* Extract number from filename (e.g., "0023.opus" -> 23) */
    int start_num = atoi(start_file);
    if (start_num > 0 && start_num <= (int)current_transfer.total_files) {
        current_transfer.file_index = start_num - 1;
        LOG_DBG("Starting from file %s (index=%u)", start_file, current_transfer.file_index);
    } else if (start_num > (int)current_transfer.total_files) {
        current_transfer.file_index = current_transfer.total_files - 1;
        LOG_DBG("Start file %s doesn't exist yet, will wait for new files", start_file);
        if (current_transfer.total_files > 0) {
            generate_filename(current_transfer.total_files, last_transferred_file);
        }
    } else {
        LOG_WRN("Invalid start file %s, starting from beginning", start_file);
        current_transfer.file_index = 0;
    }

    /* Start transfer thread */
    transfer_thread_running = true;
    k_sem_give(&transfer_trigger_sem);

    return 0;
}

int transfer_cancel(void)
{
    if (!transfer_is_active()) {
        return -EINVAL;
    }

    LOG_INF("Transfer canceled");
    transfer_cancel_requested = true;
    transfer_pause_requested = false;

    return 0;
}

int transfer_get_progress(struct transfer_info *info)
{
    if (!info) {
        return -EINVAL;
    }

    memcpy(info, &current_transfer, sizeof(*info));

    return 0;
}

bool transfer_is_active(void)
{
    return (current_transfer.state == TRANSFER_STATE_TRANSMITTING ||
            current_transfer.state == TRANSFER_STATE_PAUSED);
}

enum transfer_state transfer_get_state(void)
{
    return current_transfer.state;
}

int transfer_get_current_session(char *session_id, size_t len, char *filename, size_t filename_len)
{
    if (!transfer_is_active()) {
        return -EINVAL;
    }

    if (session_id && len > 0) {
        strncpy(session_id, current_transfer.session_id, len - 1);
        session_id[len - 1] = '\0';
    }

    if (filename && filename_len > 0) {
        if (current_transfer.current_file[0] != '\0') {
            strncpy(filename, current_transfer.current_file, filename_len - 1);
            filename[filename_len - 1] = '\0';
        } else {
            filename[0] = '\0';
        }
    }

    return 0;
}

uint32_t transfer_get_total_files(void)
{
    if (!transfer_is_active()) {
        return 0;
    }

    return current_transfer.total_files;
}

int transfer_set_synced_files(const char *session_id, uint32_t count)
{
    /* This updates the synced field in session.json */
    char filepath[128];
    struct fs_file_t file;
    char json_buf[512];
    int ret;

    if (!session_id) {
        return -EINVAL;
    }

    snprintf(filepath, sizeof(filepath), "/SD:/REC/%s/session.json", session_id);

    /* Read existing session.json */
    fs_file_t_init(&file);
    ret = fs_open(&file, filepath, FS_O_READ);
    if (ret != 0) {
        LOG_ERR("Failed to open session.json: %d", ret);
        return ret;
    }

    ssize_t bytes_read = fs_read(&file, json_buf, sizeof(json_buf) - 1);
    fs_close(&file);

    if (bytes_read < 0) {
        LOG_ERR("Failed to read session.json: %zd", bytes_read);
        return bytes_read;
    }
    json_buf[bytes_read] = '\0';

    /* Update synced field */
    char *p = strstr(json_buf, "\"synced\"");
    if (!p) {
        /* Add synced field after "files" field */
        p = strstr(json_buf, "\"files\"");
        if (p) {
            p = strstr(p, ",");
            if (p) {
                char new_synced[32];
                snprintf(new_synced, sizeof(new_synced), ",\n  \"synced\": %u", count);
                /* Insert after comma */
                memmove(p + strlen(new_synced) + 1, p + 1, strlen(p + 1));
                memcpy(p + 1, new_synced, strlen(new_synced));
            }
        }
    } else {
        /* Update existing synced field */
        p = strstr(p, ":");
        if (p) {
            p++; /* Skip ':' */
            while (*p == ' ') p++; /* Skip whitespace */
            char new_count[16];
            snprintf(new_count, sizeof(new_count), "%u", count);
            /* Find end of number */
            char *end = p;
            while (*end >= '0' && *end <= '9') end++;
            /* Replace number */
            size_t old_len = end - p;
            size_t new_len = strlen(new_count);
            if (new_len <= old_len) {
                memcpy(p, new_count, new_len);
                /* Pad with spaces if needed */
                for (size_t i = new_len; i < old_len; i++) {
                    p[i] = ' ';
                }
            } else {
                /* New value is longer — shift remaining content */
                size_t tail_len = strlen(end);
                memmove(p + new_len, end, tail_len + 1);
                memcpy(p, new_count, new_len);
            }
        }
    }

    /* Write back to file */
    fs_file_t_init(&file);
    ret = fs_open(&file, filepath, FS_O_WRITE);
    if (ret != 0) {
        LOG_ERR("Failed to open session.json for writing: %d", ret);
        return ret;
    }

    ssize_t bytes_written = fs_write(&file, json_buf, strlen(json_buf));
    fs_close(&file);

    if (bytes_written < 0) {
        LOG_ERR("Failed to write session.json: %zd", bytes_written);
        return bytes_written;
    }

    LOG_DBG("Updated synced count: %u for session %s", count, session_id);
    return 0;
}

/* Internal functions */
static void transfer_thread_main(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Wait for initial transfer start signal */
    transfer_thread_waiting = true;
    transfer_thread_ready = true;
    k_sem_take(&transfer_trigger_sem, K_FOREVER);
    transfer_thread_waiting = false;

    while (transfer_thread_running) {
        /* Check if paused */
        if (current_transfer.state == TRANSFER_STATE_PAUSED) {
            transfer_thread_waiting = true;
            k_sem_take(&transfer_trigger_sem, K_FOREVER);
            transfer_thread_waiting = false;
            continue;
        }

        /* Process transfer */
        int ret;
        static int consecutive_file_errors = 0;

process_next_file:
        /* Handle cancel: send TRANSFER_DONE then cleanup */
        if (transfer_cancel_requested) {
            LOG_INF("Transfer thread handling cancel");
            if (transfer_file_open) {
                fs_close(&transfer_file);
                transfer_file_open = false;
            }
            if (current_transfer.session_id[0] != '\0') {
                send_transfer_complete_once(current_transfer.session_id,
                                             (int)current_transfer.synced_files);
            }
            transfer_cancel_requested = false;
            transfer_cleanup();
            transfer_thread_waiting = true;
            k_sem_take(&transfer_trigger_sem, K_FOREVER);
            transfer_thread_waiting = false;
            goto process_next_file;
        }

        /* Check if we should be processing transfers */
        if (current_transfer.state != TRANSFER_STATE_TRANSMITTING) {
            /* Not in transmitting state, wait for next transfer */
            if (current_transfer.state == TRANSFER_STATE_IDLE) {
                transfer_thread_waiting = true;
                k_sem_take(&transfer_trigger_sem, K_FOREVER);
                transfer_thread_waiting = false;
                goto process_next_file;
            }
            /* COMPLETED or ERROR state - wait and recheck */
            k_sleep(K_MSEC(100));
            continue;
        }

        /* Check transport connection */
        if (!transport_is_connected()) {
            LOG_INF("Transport disconnected, stopping transfer");
            current_transfer.state = TRANSFER_STATE_IDLE;
            transfer_cleanup();
            transfer_thread_waiting = true;
            k_sem_take(&transfer_trigger_sem, K_FOREVER);
            transfer_thread_waiting = false;
            goto process_next_file;
        }

        /* Open first file if not already open */
        if (!transfer_file_open) {
            ret = transfer_next_file();
            if (ret == 0) {
                consecutive_file_errors = 0;
            }
            if (ret != 0) {
                if (ret == -ENOENT) {
                    /* No more files */
                    LOG_INF("Transfer completed: %u files", current_transfer.synced_files);
                    current_transfer.state = TRANSFER_STATE_COMPLETED;
                    send_transfer_complete_once(current_transfer.session_id,
                                                 (int)current_transfer.synced_files);
                    transfer_cleanup();
                    transfer_thread_waiting = true;
                    k_sem_take(&transfer_trigger_sem, K_FOREVER);
                    transfer_thread_waiting = false;
                    goto process_next_file;
                } else if (ret == -EAGAIN) {
                    /* Missing file skipped — try next immediately */
                    consecutive_file_errors = 0;
                    goto process_next_file;
                } else {
                    /* Non-ENOENT error */
                    consecutive_file_errors++;
                    LOG_ERR("Transfer error: %d (consecutive errors: %d)", ret, consecutive_file_errors);

                    if (consecutive_file_errors > 10) {
                        LOG_ERR("Too many consecutive file errors, aborting transfer");
                        current_transfer.state = TRANSFER_STATE_ERROR;
                        send_transfer_complete_once(current_transfer.session_id,
                                                     (int)current_transfer.synced_files);
                        transfer_cleanup();
                        transfer_thread_waiting = true;
                        k_sem_take(&transfer_trigger_sem, K_FOREVER);
                        transfer_thread_waiting = false;
                        consecutive_file_errors = 0;
                        goto process_next_file;
                    }

                    k_sleep(K_MSEC(100));
                    goto process_next_file;
                }
            }
        }

        /* Send data chunks */
        while (transfer_thread_running &&
               current_transfer.state == TRANSFER_STATE_TRANSMITTING &&
               !transfer_cancel_requested) {

            /* Check if transport is connected before sending */
            if (!transport_is_connected()) {
                LOG_WRN("Transport disconnected during send");
                current_transfer.state = TRANSFER_STATE_IDLE;
                break;
            }

            ret = transfer_send_chunk();
            if (ret != 0) {
                if (ret == -EOF) {
                    /* File complete — send FILE_END and wait for FILE_ACK */
                    int64_t elapsed_ms = k_uptime_get() - file_transfer_start_ms;
                    uint32_t rate_kbps = 0;
                    if (elapsed_ms > 0) {
                        rate_kbps = (uint32_t)((current_transfer.bytes_transferred * 8) / elapsed_ms);
                    }
                    LOG_INF("Sent: %s (%u KB, %u kbps)",
                            current_transfer.current_file,
                            (uint32_t)(current_transfer.bytes_transferred/1024),
                            rate_kbps);

                    strncpy(last_transferred_file, current_transfer.current_file,
                           sizeof(last_transferred_file) - 1);
                    last_transferred_file[sizeof(last_transferred_file) - 1] = '\0';

                    /* Send file_end and wait for FILE_ACK */
                    int file_ret = send_file_complete_event(last_transferred_file);
                    if (file_ret == -EAGAIN) {
                        /* CRC mismatch — retransmit entire file */
                        int file_retry = 0;
                        bool file_ok = false;

                        while (!file_ok && file_retry < TRANSFER_MAX_FILE_RETRIES &&
                               transfer_thread_running &&
                               current_transfer.state == TRANSFER_STATE_TRANSMITTING) {
                            file_retry++;
                            LOG_WRN("File NACK, retransmitting (%d/%d)", file_retry, TRANSFER_MAX_FILE_RETRIES);

                            /* Backoff: let WiFi channel settle before retransmit */
                            k_msleep(50 * file_retry);

                            /* Seek back to file start */
                            fs_seek(&transfer_file, 0, FS_SEEK_SET);
                            current_transfer.bytes_transferred = 0;

                            /* Resend FILE_START (will reset transport file state) */
                            send_file_ready_event(current_transfer.session_id,
                                                   last_transferred_file,
                                                   current_transfer.total_bytes);

                            /* Resend all chunks */
                            bool chunk_error = false;
                            while (transfer_thread_running &&
                                   current_transfer.state == TRANSFER_STATE_TRANSMITTING) {
                                ret = transfer_send_chunk();
                                if (ret == -EOF) {
                                    break;
                                } else if (ret < 0) {
                                    chunk_error = true;
                                    break;
                                }
                            }

                            if (chunk_error) {
                                break;
                            }

                            /* Resend FILE_END */
                            file_ret = send_file_complete_event(last_transferred_file);
                            if (file_ret == 0) {
                                file_ok = true;
                            } else if (file_ret != -EAGAIN) {
                                /* -ETIMEDOUT or other fatal error */
                                break;
                            }
                        }

                        if (!file_ok) {
                            LOG_ERR("File retransmit failed after %d retries", file_retry);
                            fs_close(&transfer_file);
                            transfer_file_open = false;
                            current_transfer.state = TRANSFER_STATE_ERROR;
                            send_transfer_complete_once(current_transfer.session_id,
                                                         (int)current_transfer.synced_files);
                            transfer_cleanup();
                            break;
                        }

                        LOG_INF("File retransmit OK on attempt %d", file_retry + 1);
                    } else if (file_ret < 0) {
                        /* -ETIMEDOUT or other error */
                        LOG_ERR("FILE_END failed: %d", file_ret);
                        fs_close(&transfer_file);
                        transfer_file_open = false;
                        current_transfer.state = TRANSFER_STATE_ERROR;
                        send_transfer_complete_once(current_transfer.session_id,
                                                     (int)current_transfer.synced_files);
                        transfer_cleanup();
                        break;
                    }

                    /* File ACKed OK — file_index is already 1-based */
                    current_transfer.synced_files = current_transfer.file_index;

                    /* Close file */
                    fs_close(&transfer_file);
                    transfer_file_open = false;
                    memset(&current_transfer.current_file, 0, sizeof(current_transfer.current_file));
                    k_sem_give(&transfer_trigger_sem);
                    break;
                } else if (ret == -ENOTCONN || ret == -EIO) {
                    /* Connection error, cancel transfer */
                    LOG_INF("Connection error during send: %d", ret);
                    /* Close file if open */
                    if (transfer_file_open) {
                        fs_close(&transfer_file);
                        transfer_file_open = false;
                    }
                    /* Do NOT increment synced_files - file was not completed */
                    current_transfer.state = TRANSFER_STATE_ERROR;
                    send_transfer_complete_once(current_transfer.session_id,
                                                 (int)current_transfer.synced_files);
                    transfer_cleanup();
                    break;
                } else {
                    /* Any other error (e.g. -ETIMEDOUT): close file and exit transfer */
                    LOG_ERR("Transfer send error: %d, aborting", ret);
                    if (transfer_file_open) {
                        fs_close(&transfer_file);
                        transfer_file_open = false;
                    }
                    current_transfer.state = TRANSFER_STATE_ERROR;
                    send_transfer_complete_once(current_transfer.session_id,
                                                 (int)current_transfer.synced_files);
                    transfer_cleanup();
                    break;
                }
            }

            /* Update progress */
            if (current_transfer.total_bytes > 0) {
                current_transfer.progress_percent =
                    (uint8_t)((current_transfer.bytes_transferred * 100) / current_transfer.total_bytes);
            }
        }
    }

    LOG_DBG("Transfer thread exiting");
}

static int transfer_next_file(void)
{
    char filepath[128];
    struct fs_dirent entry;
    int ret;

    /* If specific filename is set, use it directly */
    if (current_transfer.current_file[0] != '\0') {
        snprintf(filepath, sizeof(filepath), "/SD:/REC/%s/%s",
                 current_transfer.session_id, current_transfer.current_file);
        goto open_file;
    }

    /* Generate next filename from file number */
    uint32_t file_num = current_transfer.file_index + 1;

    if (current_transfer.continuous) {
        /* ========================================
         * CONTINUOUS MODE: Session is being recorded
         * ========================================
         * - Wait for files to be written
         * - Wait for new files to appear
         * - Detect recording stop to end transfer
         */
        generate_filename(file_num, current_transfer.current_file);
        snprintf(filepath, sizeof(filepath), "/SD:/REC/%s/%s",
                 current_transfer.session_id, current_transfer.current_file);

        /* Check if file exists */
        if (fs_stat(filepath, &entry) != 0) {
            /* File doesn't exist yet - wait for it (recording in progress) */
            int wait_count = 0;
            while (wait_count < 600) {  /* Wait up to 5 minutes (600 * 500ms) */
                /* Check if file appeared */
                if (fs_stat(filepath, &entry) == 0) {
                    LOG_INF("Found new file: %s", current_transfer.current_file);
                    goto wait_for_write;
                }

                /* Check if recording stopped - if so, no more files coming */
                if (!audio_is_recording() && wait_count > 4) {
                    /* Check if next file exists (skip missing files) */
                    uint32_t next_num = file_num + 1;
                    char next_filepath[128];
                    generate_filename(next_num, next_filepath);
                    snprintf(next_filepath, sizeof(next_filepath), "/SD:/REC/%s/%s",
                             current_transfer.session_id, next_filepath);
                    if (fs_stat(next_filepath, &entry) == 0) {
                        LOG_WRN("File missing: %s, skipping", current_transfer.current_file);
                        current_transfer.file_index = file_num;
                        current_transfer.current_file[0] = '\0';
                        return -EAGAIN;
                    }
                    LOG_INF("Recording stopped, ending transfer");
                    return -ENOENT;
                }

                /* Check if still connected */
                if (!transport_is_connected()) {
                    LOG_INF("Transport disconnected");
                    return -ENOTCONN;
                }

                /* Check if transfer was canceled */
                if (current_transfer.state != TRANSFER_STATE_TRANSMITTING) {
                    LOG_INF("Transfer canceled");
                    return -ECANCELED;
                }

                /* Wait for file closed signal with timeout */
                struct k_sem *file_closed_sem = storage_get_file_closed_sem();
                if (file_closed_sem &&
                    k_sem_take(file_closed_sem, K_MSEC(500)) == 0) {
                    /* File closed signal received, check if it's our file */
                    if (fs_stat(filepath, &entry) == 0) {
                        LOG_INF("File ready: %s", current_transfer.current_file);
                        goto wait_for_write;
                    }
                }
                wait_count++;
            }

            LOG_WRN("Timeout waiting for file");
            return -ENOENT;
        }

wait_for_write:
        /* Wait for file to finish writing using semaphore */
        while (storage_file_is_writing(current_transfer.session_id,
                                        current_transfer.current_file)) {
            if (!transport_is_connected()) {
                LOG_INF("Transport disconnected while waiting for write");
                current_transfer.current_file[0] = '\0';
                return -ENOTCONN;
            }
            /* Use semaphore with timeout instead of busy polling */
            struct k_sem *file_closed_sem = storage_get_file_closed_sem();
            if (file_closed_sem) {
                k_sem_take(file_closed_sem, K_MSEC(100));
            } else {
                k_sleep(K_MSEC(100));
            }
        }

    } else {
        /* ========================================
         * NORMAL MODE: Session is complete
         * ========================================
         * - Just read files in sequence
         * - No waiting for writes
         */
        generate_filename(file_num, current_transfer.current_file);
        snprintf(filepath, sizeof(filepath), "/SD:/REC/%s/%s",
                 current_transfer.session_id, current_transfer.current_file);

        /* Check if file exists */
        if (fs_stat(filepath, &entry) != 0) {
            /* Check if we've passed the expected file count */
            if (current_transfer.total_files > 0 &&
                file_num > current_transfer.total_files) {
                LOG_INF("All files transferred");
                return -ENOENT;
            }

            /* File missing in sequence — skip and try next */
            LOG_WRN("File missing: %s, skipping", current_transfer.current_file);
            current_transfer.file_index = file_num;  /* Advance past missing file */
            current_transfer.current_file[0] = '\0';  /* Clear so next call generates new filename */
            return -EAGAIN;
        }

        /* Check if we've transferred all files */
        if (current_transfer.total_files > 0 &&
            file_num > current_transfer.total_files) {
            LOG_INF("All files transferred");
            return -ENOENT;
        }
    }

open_file:
    /* Verify file exists and get size */
    if (fs_stat(filepath, &entry) != 0) {
        LOG_WRN("File not found: %s", filepath);
        current_transfer.current_file[0] = '\0';
        return -ENOENT;
    }

    /* Skip very small files (might still be flushing) */
    int retry_count = 0;
    while (entry.size <= 100 && retry_count < 10) {
        k_sleep(K_MSEC(100));
        if (fs_stat(filepath, &entry) != 0) {
            return -ENOENT;
        }
        retry_count++;
    }

    /* Open file */
    fs_file_t_init(&transfer_file);
    ret = fs_open(&transfer_file, filepath, FS_O_READ);
    if (ret != 0) {
        LOG_ERR("File open failed: %s (%d)", filepath, ret);
        current_transfer.current_file[0] = '\0';
        return ret;
    }

    /* Skip empty files */
    if (entry.size == 0) {
        LOG_WRN("Empty file: %s", current_transfer.current_file);
        fs_close(&transfer_file);
        fs_unlink(filepath);
        current_transfer.current_file[0] = '\0';
        return -ENOENT;
    }

    current_transfer.file_index++;
    transfer_file_open = true;
    current_transfer.total_bytes = entry.size;
    current_transfer.bytes_transferred = 0;  /* Reset for accurate rate calculation */
    file_transfer_start_ms = k_uptime_get();

    LOG_INF("Sending: %s (%u KB)", current_transfer.current_file, (uint32_t)entry.size/1024);
    send_file_ready_event(current_transfer.session_id, current_transfer.current_file, entry.size);

    return 0;
}

static int transfer_send_chunk(void)
{
    ssize_t bytes_read;
    int ret;
    int64_t t0, t_read, t_send;

    if (!transfer_file_open) {
        LOG_ERR("File not open!");
        return -EIO;
    }

    if (!current_transport || !current_transport->ops || !current_transport->ops->send_file_data) {
        LOG_ERR("No transport available for sending");
        return -ENOTCONN;
    }

    /* Read chunk from file into static buffer */
    t0 = k_uptime_get();
    bytes_read = fs_read(&transfer_file, chunk_buffer, CONFIG_CLIP_TRANSFER_CHUNK_SIZE);
    t_read = k_uptime_get() - t0;

    if (bytes_read < 0) {
        LOG_ERR("File read error: %zd", bytes_read);
        return bytes_read;
    }

    if (bytes_read == 0) {
        /* End of file */
        LOG_DBG("End of file: %llu bytes", current_transfer.bytes_transferred);
        return -EOF;
    }

    /* Send via the transport that initiated this transfer */
    t0 = k_uptime_get();
    ret = current_transport->ops->send_file_data(chunk_buffer, bytes_read);
    t_send = k_uptime_get() - t0;

    if (ret < 0) {
        LOG_ERR("Transport send error: %d", ret);
        return ret;
    }

    /* Log timing for first few chunks and every 64th chunk */
    static int chunk_count;
    if (chunk_count < 3 || chunk_count % 64 == 0) {
        LOG_INF("chunk %d: read=%dms, send=%dms, size=%d",
                chunk_count, (int)t_read, (int)t_send, (int)bytes_read);
    }
    chunk_count++;

    /* Only increment bytes_transferred if send actually succeeded */
    current_transfer.bytes_transferred += bytes_read;

    return 0;
}

static void send_file_ready_event(const char *session_id, const char *filename, uint64_t size)
{
    if (!session_id || session_id[0] == '\0') {
        LOG_WRN("Cannot send file_ready: empty session_id");
        return;
    }

    if (!filename || filename[0] == '\0') {
        LOG_WRN("Cannot send file_ready: empty filename");
        return;
    }

    if (!current_transport || !current_transport->ops) {
        LOG_WRN("No transport for file_ready event");
        return;
    }

    if (current_transport->ops->send_file_start) {
        current_transport->ops->send_file_start(session_id, filename, (uint32_t)size);
    }
}

static int send_file_complete_event(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        LOG_WRN("Cannot send file_complete: empty filename");
        return -EINVAL;
    }

    if (!current_transport || !current_transport->ops) {
        LOG_WRN("No transport for file_complete event");
        return -ENOTCONN;
    }

    if (current_transport->ops->send_file_end) {
        return current_transport->ops->send_file_end(filename);
    }

    return 0;
}


static void send_transfer_complete_once(const char *session_id, int file_count)
{
    if (transfer_complete_sent) {
        LOG_DBG("transfer_complete already sent, skipping duplicate");
        return;
    }

    if (!session_id || session_id[0] == '\0') {
        LOG_WRN("Cannot send transfer_complete: empty session_id");
        return;
    }

    /* Save synced count to session.json */
    if (current_transfer.synced_files > 0) {
        transfer_set_synced_files(session_id, current_transfer.synced_files);
    }

    if (current_transport && current_transport->ops && current_transport->ops->send_transfer_done) {
        current_transport->ops->send_transfer_done(session_id, file_count);
    }

    LOG_INF("Transfer complete: session=%s, files=%d", session_id, file_count);
    transfer_complete_sent = true;
}

static void transfer_cleanup(void)
{
    clip_cpu_boost_release();

    if (transfer_file_open) {
        fs_close(&transfer_file);
        transfer_file_open = false;
    }

    /* Save synced count to session.json if any files were transferred */
    if (current_transfer.synced_files > 0 && current_transfer.session_id[0] != '\0') {
        transfer_set_synced_files(current_transfer.session_id, current_transfer.synced_files);
        LOG_DBG("Saved synced count: %u for session %s",
                current_transfer.synced_files, current_transfer.session_id);
    }

    /* Reset state */
    current_transfer.state = TRANSFER_STATE_IDLE;
    memset(&current_transfer.current_file, 0, sizeof(current_transfer.current_file));
    memset(current_transfer.session_id, 0, sizeof(current_transfer.session_id));
    current_transfer.file_index = 0;
    current_transfer.total_files = 0;
    current_transfer.synced_files = 0;
    current_transfer.bytes_transferred = 0;
    current_transfer.total_bytes = 0;
    current_transfer.progress_percent = 0;
    transfer_pause_requested = false;
    transfer_cancel_requested = false;
    transfer_complete_sent = false;
}
  