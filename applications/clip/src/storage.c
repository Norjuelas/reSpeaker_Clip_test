/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "storage.h"

LOG_MODULE_REGISTER(storage, CONFIG_CLIP_LOG_LEVEL);

/* SD Card and File System */
static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = "/SD:",
};
static bool sd_mounted = false;

/* Base path for recordings */
#define STORAGE_BASE_PATH "/SD:/REC"

/* Write buffer for efficient SD card operations */
static uint8_t write_buffer[CONFIG_CLIP_STORAGE_CHUNK_SIZE];
static uint32_t buffer_pos = 0;
static struct fs_file_t *current_file_ptr = NULL;

/* Statistics */
static uint32_t total_chunks = 0;
static uint64_t total_bytes = 0;
static uint32_t free_space_mb = 0;
static uint64_t session_bytes_base = 0;  /* total_bytes at session start */

/* Current recording session */
static char current_session_id[STORAGE_SESSION_ID_LEN] = {0};

/* Track current writing file for transfer coordination */
static char writing_session[STORAGE_SESSION_ID_LEN] = {0};
static char writing_filename[STORAGE_FILENAME_MAX_LEN] = {0};

/* Semaphore to signal when a file is closed and ready for transfer */
static K_SEM_DEFINE(file_closed_sem, 0, 1);

/* Internal functions */
static int update_free_space(void);
static int create_marks_file(const char *path);
static int create_session_json(const char *path, const char *session_id,
                               uint8_t channels, uint32_t sample_rate,
                               const char *mode);
static int update_session_json(const char *session_id, uint32_t duration_sec,
                               uint32_t chunk_count, uint64_t session_bytes);
static int flush_write_buffer(void);

int storage_init(void)
{
    int rc;
    struct fs_dirent entry;

    LOG_INF("Initializing SD card storage");

    /* Initialize SD card */
    rc = disk_access_init("SD");
    if (rc != 0)
    {
        LOG_WRN("SD init failed: %d", rc);
        return rc;
    }

    /* Mount filesystem */
    rc = fs_mount(&mp);
    if (rc != 0)
    {
        LOG_WRN("SD mount failed: %d", rc);
        sd_mounted = false;
        return rc;
    }

    sd_mounted = true;
    LOG_INF("SD card mounted at /SD:");

    /* Create base REC directory if not exists */
    rc = fs_stat(STORAGE_BASE_PATH, &entry);
    if (rc != 0 || entry.type != FS_DIR_ENTRY_DIR)
    {
        rc = fs_mkdir(STORAGE_BASE_PATH);
        if (rc != 0 && rc != -EEXIST)
        {
            LOG_ERR("Failed to create REC directory: %d", rc);
            return rc;
        }
    }

    update_free_space();

    return 0;
}

void storage_cleanup(void)
{
    if (sd_mounted)
    {
        fs_unmount(&mp);
        sd_mounted = false;
    }
}

bool storage_is_mounted(void)
{
    return sd_mounted;
}

int storage_get_stats(struct storage_stats *stats)
{
    if (!stats)
    {
        return -EINVAL;
    }

    memset(stats, 0, sizeof(*stats));
    stats->is_mounted = sd_mounted;
    stats->total_chunks = total_chunks;
    stats->total_bytes = total_bytes;

    if (sd_mounted)
    {
        update_free_space();
        stats->free_space_mb = free_space_mb;
    }

    return 0;
}

int storage_create_session(const char *session_id, uint8_t channels,
                           uint32_t sample_rate, const char *mode)
{
    char dir_path[128];
    struct fs_dirent entry;
    int rc;

    if (!sd_mounted)
    {
        return -ENODEV;
    }

    if (!session_id)
    {
        return -EINVAL;
    }

    /* Create session directory */
    snprintf(dir_path, sizeof(dir_path), "%s/%s", STORAGE_BASE_PATH, session_id);
    LOG_INF("Creating session: %s", session_id);

    rc = fs_stat(dir_path, &entry);
    if (rc != 0 || entry.type != FS_DIR_ENTRY_DIR)
    {
        rc = fs_mkdir(dir_path);
        if (rc != 0 && rc != -EEXIST)
        {
            LOG_ERR("Failed to create session directory: %d", rc);
            return rc;
        }
    }

    /* Create marks.bin for bookmarks */
    create_marks_file(dir_path);

    /* Create session.json */
    create_session_json(dir_path, session_id, channels, sample_rate, mode);

    /* Store current session */
    strncpy(current_session_id, session_id, sizeof(current_session_id) - 1);
    current_session_id[sizeof(current_session_id) - 1] = '\0';
    session_bytes_base = total_bytes;

    return 0;
}

int storage_close_session(const char *session_id, uint32_t duration_sec,
                          uint32_t chunk_count)
{
    if (!session_id)
    {
        return -EINVAL;
    }

    uint64_t session_bytes = total_bytes - session_bytes_base;

    LOG_INF("Closing session: %s (chunks=%u, bytes=%llu, duration=%u sec)",
            session_id, chunk_count, session_bytes, duration_sec);

    /* Update session.json with final values */
    update_session_json(session_id, duration_sec, chunk_count, session_bytes);

    /* Clear current session if matching */
    if (strcmp(current_session_id, session_id) == 0)
    {
        current_session_id[0] = '\0';
    }

    return 0;
}

int storage_write_chunk(const char *session_id, uint32_t chunk_index,
                        const uint8_t *data, uint32_t len)
{
    char filepath[128];
    struct fs_file_t file;
    int rc;
    ssize_t written;

    if (!sd_mounted || !session_id || !data)
    {
        return -EINVAL;
    }

    if (len == 0)
    {
        return 0;
    }

    /* Generate chunk filename: 0001.opus, 0002.opus, ... */
    snprintf(filepath, sizeof(filepath), "%s/%s/%04u.opus",
             STORAGE_BASE_PATH, session_id, chunk_index);

    /* Open file for writing */
    fs_file_t_init(&file);
    rc = fs_open(&file, filepath, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0)
    {
        LOG_ERR("Failed to create chunk file %s: %d", filepath, rc);
        return rc;
    }

    /* Write data */
    written = fs_write(&file, data, len);
    fs_close(&file);

    if (written != len)
    {
        LOG_ERR("Write incomplete: %zd != %u", written, len);
        return -EIO;
    }

    /* Update statistics */
    total_chunks++;
    total_bytes += len;

    return 0;
}

/* Flush write buffer to SD card */
static int flush_write_buffer(void)
{
    int rc;
    ssize_t written;

    if (buffer_pos == 0 || !current_file_ptr)
    {
        return 0;
    }

    written = fs_write(current_file_ptr, write_buffer, buffer_pos);
    if (written != buffer_pos)
    {
        LOG_ERR("Buffer flush incomplete: %zd != %u", written, buffer_pos);
        return -EIO;
    }

    buffer_pos = 0;
    return 0;
}

int storage_create_file(struct storage_file *file, const char *session_id, uint32_t chunk_index)
{
    char filepath[128];
    char filename[32]; /* Just the filename, not full path */
    int rc;

    if (!sd_mounted || !file || !session_id)
    {
        return -EINVAL;
    }

    /* Flush any existing buffer */
    if (current_file_ptr)
    {
        rc = flush_write_buffer();
        if (rc != 0)
        {
            return rc;
        }
        fs_close(current_file_ptr);
        current_file_ptr = NULL;
    }

    /* Initialize file structure */
    memset(file, 0, sizeof(*file));
    strncpy(file->session_id, session_id, sizeof(file->session_id) - 1);
    file->chunk_index = chunk_index;

    /* Generate filename: 0001.opus (4-digit format) */
    snprintf(filename, sizeof(filename), "%04u.opus", chunk_index);
    snprintf(filepath, sizeof(filepath), "%s/%s/%s",
             STORAGE_BASE_PATH, session_id, filename);
    strncpy(file->filename, filename, sizeof(file->filename) - 1);

    /* Open file */
    fs_file_t_init(&file->internal_file);
    LOG_INF("Opening file: %s...", filepath);
    rc = fs_open(&file->internal_file, filepath, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0)
    {
        LOG_ERR("Failed to create file %s: %d", filepath, rc);
        return rc;
    }

    file->is_open = true;
    current_file_ptr = &file->internal_file;
    buffer_pos = 0;

    /* Mark this file as being written for transfer coordination */
    storage_set_writing_file(session_id, filename);

    LOG_INF("File opened: %s (chunk %u)", filepath, chunk_index);
    return 0;
}

int storage_write_frame(struct storage_file *file, const uint8_t *data, uint32_t len)
{
    int rc;
    uint8_t frame_len[2];

    if (!sd_mounted || !file || !file->is_open)
    {
        return -EINVAL;
    }

    if (len > 65535)
    {
        LOG_ERR("Frame too large: %u", len);
        return -EINVAL;
    }

    /* Write frame length as 2-byte little-endian */
    frame_len[0] = len & 0xFF;
    frame_len[1] = (len >> 8) & 0xFF;

    /* Write length to buffer */
    if (buffer_pos + 2 > CONFIG_CLIP_STORAGE_CHUNK_SIZE)
    {
        rc = flush_write_buffer();
        if (rc != 0)
        {
            return rc;
        }
    }

    write_buffer[buffer_pos++] = frame_len[0];
    write_buffer[buffer_pos++] = frame_len[1];

    /* Write frame data */
    uint32_t remaining = len;
    uint32_t offset = 0;

    while (remaining > 0)
    {
        uint32_t space = CONFIG_CLIP_STORAGE_CHUNK_SIZE - buffer_pos;
        uint32_t to_copy = (remaining < space) ? remaining : space;

        memcpy(&write_buffer[buffer_pos], &data[offset], to_copy);
        buffer_pos += to_copy;
        offset += to_copy;
        remaining -= to_copy;

        /* Flush buffer when full */
        if (buffer_pos >= CONFIG_CLIP_STORAGE_CHUNK_SIZE)
        {
            rc = flush_write_buffer();
            if (rc != 0)
            {
                return rc;
            }
        }
    }

    file->bytes_written += len + 2; /* +2 for length header */
    file->frames_written++;

    return 0;
}

int storage_close_file(struct storage_file *file)
{
    int rc;

    if (!file || !file->is_open)
    {
        return -EINVAL;
    }

    /* Flush any remaining data in buffer */
    if (buffer_pos > 0)
    {
        rc = flush_write_buffer();
        if (rc != 0)
        {
            LOG_ERR("Failed to flush buffer: %d", rc);
        }
    }

    /* Sync file to ensure data is written to disk
     * Important for transfer-while-recording: files must be available immediately */
    if (current_file_ptr)
    {
        rc = fs_sync(current_file_ptr);
        if (rc != 0)
        {
            LOG_WRN("File sync failed: %d", rc);
        }

        fs_close(current_file_ptr);
        current_file_ptr = NULL;
    }

    /* Small delay to ensure file system has flushed data */
    k_sleep(K_MSEC(50));

    /* Clear writing file status - file is now ready for transfer */
    storage_set_writing_file(NULL, NULL);

    /* Signal that a file has been closed and is ready for transfer */
    k_sem_give(&file_closed_sem);

    LOG_DBG("File closed: %s (%u bytes, %u frames)",
            file->filename, file->bytes_written, file->frames_written);

    file->is_open = false;
    return 0;
}

int storage_read_chunk(const char *session_id, uint32_t chunk_index,
                       uint8_t *data, uint32_t len)
{
    char filepath[128];
    struct fs_file_t file;
    int rc;
    ssize_t bytes_read;

    if (!sd_mounted || !session_id || !data)
    {
        return -EINVAL;
    }

    /* Generate chunk filename */
    snprintf(filepath, sizeof(filepath), "%s/%s/%04u.opus",
             STORAGE_BASE_PATH, session_id, chunk_index);

    /* Open file for reading */
    fs_file_t_init(&file);
    rc = fs_open(&file, filepath, FS_O_READ);
    if (rc != 0)
    {
        LOG_ERR("Failed to open chunk file %s: %d", filepath, rc);
        return rc;
    }

    /* Read data */
    bytes_read = fs_read(&file, data, len);
    fs_close(&file);

    if (bytes_read < 0)
    {
        LOG_ERR("Read error: %zd", bytes_read);
        return (int)bytes_read;
    }

    return (int)bytes_read;
}

int storage_delete_chunk(const char *session_id, uint32_t chunk_index)
{
    char filepath[128];
    int rc;

    if (!sd_mounted || !session_id)
    {
        return -EINVAL;
    }

    /* Generate chunk filename */
    snprintf(filepath, sizeof(filepath), "%s/%s/%04u.opus",
             STORAGE_BASE_PATH, session_id, chunk_index);

    rc = fs_unlink(filepath);
    if (rc != 0)
    {
        LOG_WRN("Failed to delete chunk %s: %d", filepath, rc);
    }

    return rc;
}

int storage_list_sessions(struct storage_session_info *sessions, int max_sessions)
{
    struct fs_dir_t dirp;
    struct fs_dirent entry;
    int count = 0;
    int rc;

    if (!sd_mounted || !sessions)
    {
        return -EINVAL;
    }

    LOG_DBG("Listing sessions (max=%d)", max_sessions);

    /* Open REC directory */
    fs_dir_t_init(&dirp);
    rc = fs_opendir(&dirp, STORAGE_BASE_PATH);
    if (rc != 0)
    {
        LOG_ERR("Failed to open REC directory: %d", rc);
        return rc;
    }

    /* Read directory entries */
    while (count < max_sessions)
    {
        rc = fs_readdir(&dirp, &entry);
        if (rc != 0 || entry.name[0] == '\0')
        {
            break;
        }

        /* Skip non-directories */
        if (entry.type != FS_DIR_ENTRY_DIR)
        {
            continue;
        }

        /* Validate session_id format: should be 14 digits (YYYYMMDDHHMMSS) */
        size_t len = strlen(entry.name);
        if (len != 14)
        {
            LOG_DBG("Skipping invalid session dir (wrong length): %s (len=%u)",
                    entry.name, (unsigned int)len);
            continue;
        }

        /* Check if all digits */
        bool valid = true;
        for (size_t i = 0; i < len; i++)
        {
            if (entry.name[i] < '0' || entry.name[i] > '9')
            {
                valid = false;
                break;
            }
        }
        if (!valid)
        {
            LOG_DBG("Skipping invalid session dir (not all digits): %s", entry.name);
            continue;
        }

        LOG_DBG("Found session dir: %s", entry.name);

        /* Get session info (skip counting chunks for speed) */
        rc = storage_get_session_info(entry.name, &sessions[count]);
        if (rc == 0)
        {
            count++;
            LOG_DBG("Added session %d: %s", count, entry.name);
        }
        else
        {
            LOG_WRN("Failed to get info for %s: %d", entry.name, rc);
        }

        /* Yield to prevent blocking other operations */
        if (count % 5 == 0)
        {
            k_yield();
        }
    }

    fs_closedir(&dirp);

    /* Sort by session_id descending (newest first) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(sessions[i].session_id, sessions[j].session_id) < 0) {
                struct storage_session_info tmp = sessions[i];
                sessions[i] = sessions[j];
                sessions[j] = tmp;
            }
        }
    }

    LOG_DBG("Listed %d sessions", count);
    return count;
}

int storage_count_sessions(void)
{
    struct fs_dir_t dirp;
    struct fs_dirent entry;
    int count = 0;
    int rc;

    if (!sd_mounted)
    {
        return -EINVAL;
    }

    /* Open REC directory */
    fs_dir_t_init(&dirp);
    rc = fs_opendir(&dirp, STORAGE_BASE_PATH);
    if (rc != 0)
    {
        LOG_ERR("Failed to open REC directory: %d", rc);
        return rc;
    }

    /* Count valid session directories */
    while (true)
    {
        rc = fs_readdir(&dirp, &entry);
        if (rc != 0 || entry.name[0] == '\0')
        {
            break;
        }

        /* Skip non-directories */
        if (entry.type != FS_DIR_ENTRY_DIR)
        {
            continue;
        }

        /* Validate session_id format: should be 14 digits */
        size_t len = strlen(entry.name);
        if (len != 14)
        {
            continue;
        }

        /* Check if all digits */
        bool valid = true;
        for (size_t i = 0; i < len; i++)
        {
            if (entry.name[i] < '0' || entry.name[i] > '9')
            {
                valid = false;
                break;
            }
        }
        if (valid)
        {
            count++;
        }
    }

    fs_closedir(&dirp);

    return count;
}

int storage_list_sessions_paginated(struct storage_session_info *sessions,
                                    int offset, int limit)
{
    struct fs_dir_t dirp;
    struct fs_dirent entry;
    int count = 0;
    int skipped = 0;
    int rc;

    if (!sd_mounted || !sessions)
    {
        return -EINVAL;
    }

    /* Open REC directory */
    fs_dir_t_init(&dirp);
    rc = fs_opendir(&dirp, STORAGE_BASE_PATH);
    if (rc != 0)
    {
        LOG_ERR("Failed to open REC directory: %d", rc);
        return rc;
    }

    /* Read directory entries, skip first 'offset' entries */
    while (count < limit)
    {
        rc = fs_readdir(&dirp, &entry);
        if (rc != 0 || entry.name[0] == '\0')
        {
            break;
        }

        /* Skip non-directories */
        if (entry.type != FS_DIR_ENTRY_DIR)
        {
            continue;
        }

        /* Validate session_id format: should be 14 digits */
        size_t len = strlen(entry.name);
        if (len != 14)
        {
            continue;
        }

        /* Check if all digits */
        bool valid = true;
        for (size_t i = 0; i < len; i++)
        {
            if (entry.name[i] < '0' || entry.name[i] > '9')
            {
                valid = false;
                break;
            }
        }
        if (!valid)
        {
            continue;
        }

        /* Skip first 'offset' valid entries */
        if (skipped < offset)
        {
            skipped++;
            continue;
        }

        /* Get session info */
        rc = storage_get_session_info(entry.name, &sessions[count]);
        if (rc == 0)
        {
            count++;
        }

        /* Yield periodically */
        if (count % 5 == 0)
        {
            k_yield();
        }
    }

    fs_closedir(&dirp);

    /* Sort by session_id descending (newest first) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(sessions[i].session_id, sessions[j].session_id) < 0) {
                struct storage_session_info tmp = sessions[i];
                sessions[i] = sessions[j];
                sessions[j] = tmp;
            }
        }
    }

    return count;
}

int storage_get_session_info(const char *session_id, struct storage_session_info *info)
{
    char filepath[128];
    struct fs_file_t file;
    char json_buf[512];
    int rc;
    ssize_t bytes_read;

    if (!info || !session_id)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));
    strncpy(info->session_id, session_id, sizeof(info->session_id) - 1);
    info->session_id[sizeof(info->session_id) - 1] = '\0';

    /* Try to read session.json */
    snprintf(filepath, sizeof(filepath), "%s/%s/session.json",
             STORAGE_BASE_PATH, session_id);

    fs_file_t_init(&file);
    rc = fs_open(&file, filepath, FS_O_READ);
    if (rc == 0)
    {
        bytes_read = fs_read(&file, json_buf, sizeof(json_buf) - 1);
        fs_close(&file);

        if (bytes_read > 0)
        {
            json_buf[bytes_read] = '\0';

            /* Simple JSON parsing */
            char *p;

            /* Parse duration */
            p = strstr(json_buf, "\"duration\":");
            if (p)
            {
                info->duration_sec = atoi(p + 11);
            }

            /* Parse files (chunk_count) */
            p = strstr(json_buf, "\"files\":");
            if (p)
            {
                info->file_count = atoi(p + 8);
            }

            /* Parse synced files */
            p = strstr(json_buf, "\"synced\":");
            if (p)
            {
                info->synced_files = atoi(p + 9);
            }
            else
            {
                /* For backward compatibility, if no synced field, check recording flag */
                p = strstr(json_buf, "\"recording\":");
                if (p && strncmp(p + 12, "true", 4) == 0)
                {
                    info->synced_files = 0; /* Still recording, nothing synced yet */
                }
                else
                {
                    /* Recording completed, all files are synced */
                    info->synced_files = info->file_count;
                }
            }

            /* Parse size (total bytes) */
            p = strstr(json_buf, "\"size\":");
            if (p)
            {
                info->total_bytes = (uint64_t)strtoull(p + 7, NULL, 10);
            }

            /* Parse channels */
            p = strstr(json_buf, "\"channels\":");
            if (p)
            {
                info->channels = atoi(p + 11);
            }

            /* Parse sample_rate and convert to kHz */
            p = strstr(json_buf, "\"sample_rate\":");
            if (p)
            {
                uint32_t sample_rate_hz = atoi(p + 14);
                info->sample_rate_khz = (uint8_t)(sample_rate_hz / 1000);
            }

            /* Parse mode */
            p = strstr(json_buf, "\"mode\": \"");
            if (p)
            {
                char mode_start[16];
                strncpy(mode_start, p + 9, sizeof(mode_start) - 1);
                mode_start[sizeof(mode_start) - 1] = '\0';
                char *end = strchr(mode_start, '"');
                if (end)
                {
                    *end = '\0';
                    strncpy(info->mode, mode_start, sizeof(info->mode) - 1);
                }
            }

            /* Parse recording flag - if still recording, reset synced */
            p = strstr(json_buf, "\"recording\":");
            if (p && strncmp(p + 12, "true", 4) == 0)
            {
                info->synced_files = 0; /* Still recording, nothing synced yet */
            }
            /* Note: do NOT override synced_files when recording is done.
             * The synced field in session.json is the authoritative value. */
        }
    }

    /* Fallback: if session.json has no file info (empty/corrupt),
     * count files from directory */
    if (info->file_count == 0 && info->total_bytes == 0 &&
        info->channels == 0 && info->sample_rate_khz == 0)
    {
        char dir_path[128];
        struct fs_dir_t dirp;
        struct fs_dirent entry;

        snprintf(dir_path, sizeof(dir_path), "%s/%s",
                 STORAGE_BASE_PATH, session_id);
        fs_dir_t_init(&dirp);
        if (fs_opendir(&dirp, dir_path) == 0)
        {
            uint32_t file_count = 0;
            while (fs_readdir(&dirp, &entry) == 0 && entry.name[0] != '\0')
            {
                size_t len = strlen(entry.name);
                if ((len == 9 && strcmp(entry.name + 4, ".opus") == 0) ||
                    (len == 9 && strcmp(entry.name + 4, ".ogg") == 0))
                {
                    file_count++;
                }
            }
            fs_closedir(&dirp);
            if (file_count > 0)
            {
                info->file_count = file_count;
                LOG_WRN("session.json missing/empty for %s, "
                        "counted %u files from directory",
                        session_id, file_count);
            }
        }
    }

    return 0;
}

int storage_list_chunks(const char *session_id, uint32_t *chunks, int max_chunks, int skip)
{
    char dir_path[128];
    struct fs_dir_t dirp;
    struct fs_dirent entry;
    int count = 0;
    int rc;
    int skipped = 0;

    if (!sd_mounted || !session_id || !chunks)
    {
        return -EINVAL;
    }

    /* Open session directory */
    snprintf(dir_path, sizeof(dir_path), "%s/%s", STORAGE_BASE_PATH, session_id);
    LOG_DBG("Listing chunks in: %s (max=%d, skip=%d)", dir_path, max_chunks, skip);

    fs_dir_t_init(&dirp);
    rc = fs_opendir(&dirp, dir_path);
    if (rc != 0)
    {
        LOG_ERR("Failed to open session directory %s: %d", dir_path, rc);
        return rc;
    }

    /* Read directory entries */
    while (count < max_chunks)
    {
        rc = fs_readdir(&dirp, &entry);
        if (rc != 0 || entry.name[0] == '\0')
        {
            break;
        }

        /* Check if it's a .opus file with 4-digit prefix (0001.opus) */
        size_t len = strlen(entry.name);
        if (len == 9 && strcmp(entry.name + 4, ".opus") == 0)
        {
            /* Skip files before the requested offset */
            if (skipped < skip)
            {
                skipped++;
                continue;
            }
            /* Extract chunk number from filename (0001.opus -> 1) */
            chunks[count] = (uint32_t)atoi(entry.name);
            count++;
            LOG_DBG("Found chunk: %s -> %u", entry.name, chunks[count - 1]);
        }
    }

    fs_closedir(&dirp);

    LOG_DBG("Listed %d chunks for session %s", count, session_id);
    return count;
}

int storage_delete_session(const char *session_id)
{
    char dir_path[128];
    struct fs_dir_t dirp;
    struct fs_dirent entry;
    int rc;

    if (!sd_mounted || !session_id)
    {
        return -EINVAL;
    }

    /* Don't delete current recording session */
    if (strcmp(current_session_id, session_id) == 0)
    {
        return -EBUSY;
    }

    /* Open session directory */
    snprintf(dir_path, sizeof(dir_path), "%s/%s", STORAGE_BASE_PATH, session_id);
    fs_dir_t_init(&dirp);
    rc = fs_opendir(&dirp, dir_path);
    if (rc != 0)
    {
        return rc;
    }

    /* Delete all files in directory */
    while (true)
    {
        rc = fs_readdir(&dirp, &entry);
        if (rc != 0 || entry.name[0] == '\0')
        {
            break;
        }

        char filepath[192];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, entry.name);
        fs_unlink(filepath);
    }

    fs_closedir(&dirp);

    /* Remove directory */
    rc = fs_unlink(dir_path);
    if (rc != 0)
    {
        LOG_WRN("Failed to remove session directory: %d", rc);
    }

    return 0;
}

int storage_format_card(void)
{
    int rc;

    if (!sd_mounted)
    {
        return -ENODEV;
    }

    /* 1. Unmount */
    fs_unmount(&mp);
    sd_mounted = false;

    /* 2. Format */
    static uint8_t workbuf[4096];

    MKFS_PARM opt = {
        .fmt = FM_FAT32,
        .n_fat = 1,
        .align = 0,
        .n_root = 0,
        .au_size = 0};

    rc = f_mkfs("SD:", &opt, workbuf, sizeof(workbuf));

    if (rc != FR_OK)
    {
        LOG_ERR("f_mkfs failed: %d", rc);
        return -EIO;
    }

    /* 3. Remount */
    rc = fs_mount(&mp);
    if (rc != 0)
    {
        LOG_ERR("Remount failed after format: %d", rc);
        return rc;
    }

    sd_mounted = true;

    /* 4. Recreate REC directory */
    fs_mkdir(STORAGE_BASE_PATH);

    LOG_INF("SD card formatted and remounted");
    return 0;
}

/* Internal functions */

static int update_free_space(void)
{
    uint32_t free_sectors;
    FATFS *fat_fs_ptr;
    int rc;

    if (!sd_mounted)
    {
        return -ENODEV;
    }

    /* Get free space from FatFS - use FatFS native path */
    rc = f_getfree("0:", &free_sectors, &fat_fs_ptr);
    if (rc != 0)
    {
        LOG_WRN("Failed to get free space: %d", rc);
        free_space_mb = 0;
        return rc;
    }

    /* Calculate free space in MB (sector size is typically 512 bytes) */
    uint64_t free_bytes = (uint64_t)free_sectors * fat_fs_ptr->csize * 512;
    free_space_mb = (uint32_t)(free_bytes / (1024 * 1024));

    return 0;
}

static int create_marks_file(const char *dir_path)
{
    char marks_path[128];
    struct fs_file_t file;
    int rc;
    ssize_t written;

    snprintf(marks_path, sizeof(marks_path), "%s/marks.bin", dir_path);

    fs_file_t_init(&file);
    rc = fs_open(&file, marks_path, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0)
    {
        LOG_ERR("Failed to create marks.bin: %d", rc);
        return rc;
    }

    /* Write empty bookmark header */
    uint8_t header[6] = {'B', 'M', 'R', 'K', 0, 0};
    written = fs_write(&file, header, 6);
    if (written != 6)
    {
        LOG_ERR("Failed to write marks.bin header: %zd", written);
        fs_close(&file);
        return -EIO;
    }

    fs_close(&file);
    LOG_DBG("Created marks.bin");

    return 0;
}

static int create_session_json(const char *dir_path, const char *session_id,
                               uint8_t channels, uint32_t sample_rate,
                               const char *mode)
{
    char json_path[128];
    struct fs_file_t file;
    char json_buf[512];
    int len;
    int rc;
    ssize_t written;

    snprintf(json_path, sizeof(json_path), "%s/session.json", dir_path);

    fs_file_t_init(&file);
    rc = fs_open(&file, json_path, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0)
    {
        LOG_ERR("Failed to create session.json: %d", rc);
        return rc;
    }

    len = snprintf(json_buf, sizeof(json_buf),
                   "{\n"
                   "  \"id\": \"%s\",\n"
                   "  \"duration\": 0,\n"
                   "  \"files\": 0,\n"
                   "  \"synced\": 0,\n"
                   "  \"channels\": %u,\n"
                   "  \"sample_rate\": %u,\n"
                   "  \"mode\": \"%s\",\n"
                   "  \"recording\": true\n"
                   "}\n",
                   session_id, channels, sample_rate, mode ? mode : "normal");

    if (len < 0 || len >= (int)sizeof(json_buf))
    {
        LOG_ERR("Failed to format session.json");
        fs_close(&file);
        return -ENOMEM;
    }

    written = fs_write(&file, json_buf, len);
    if (written != len)
    {
        LOG_ERR("Failed to write session.json: %zd != %d", written, len);
        fs_close(&file);
        return -EIO;
    }

    fs_close(&file);
    LOG_DBG("Created session.json");

    return 0;
}

static int update_session_json(const char *session_id, uint32_t duration_sec,
                               uint32_t chunk_count, uint64_t session_bytes)
{
    char json_path[128];
    struct fs_file_t file;
    char json_buf[512];
    char channels_str[16] = "2";
    char sample_rate_str[16] = "16000";
    char mode_str[16] = "normal";
    int len;
    int rc;
    ssize_t bytes_read, written;

    snprintf(json_path, sizeof(json_path), "%s/%s/session.json",
             STORAGE_BASE_PATH, session_id);

    /* Read existing JSON to preserve other fields including synced */
    fs_file_t_init(&file);
    char synced_str[16] = "0"; /* Default: no files synced */
    rc = fs_open(&file, json_path, FS_O_READ);
    if (rc == 0)
    {
        bytes_read = fs_read(&file, json_buf, sizeof(json_buf) - 1);
        fs_close(&file);

        if (bytes_read > 0)
        {
            json_buf[bytes_read] = '\0';

            /* Parse and preserve channels, sample_rate, mode, synced */
            char *p;
            p = strstr(json_buf, "\"channels\":");
            if (p)
            {
                unsigned int val = atoi(p + 11);
                snprintf(channels_str, sizeof(channels_str), "%u", val);
            }
            p = strstr(json_buf, "\"sample_rate\":");
            if (p)
            {
                unsigned int val = atoi(p + 14);
                snprintf(sample_rate_str, sizeof(sample_rate_str), "%u", val);
            }
            p = strstr(json_buf, "\"mode\": \"");
            if (p)
            {
                char temp[16];
                strncpy(temp, p + 9, sizeof(temp) - 1);
                temp[sizeof(temp) - 1] = '\0';
                char *end = strchr(temp, '"');
                if (end)
                {
                    *end = '\0';
                    strncpy(mode_str, temp, sizeof(mode_str) - 1);
                }
            }
            /* Preserve synced count - don't assume all files are synced */
            p = strstr(json_buf, "\"synced\":");
            if (p)
            {
                unsigned int val = atoi(p + 9);
                snprintf(synced_str, sizeof(synced_str), "%u", val);
            }
        }
    }

    /* Write updated JSON with all fields, set recording to false
     * Keep synced count unchanged - only transfer should update it.
     * Use FS_O_CREATE | FS_O_WRITE without FS_O_TRUNC to avoid race:
     * concurrent readers may see an empty file if truncate happens before write.
     * The new content is written at offset 0, overwriting the old data.
     */
    fs_file_t_init(&file);
    rc = fs_open(&file, json_path, FS_O_CREATE | FS_O_WRITE);
    if (rc != 0)
    {
        LOG_ERR("Failed to open session.json for update: %d", rc);
        return rc;
    }

    len = snprintf(json_buf, sizeof(json_buf),
                   "{\n"
                   "  \"id\": \"%s\",\n"
                   "  \"duration\": %u,\n"
                   "  \"files\": %u,\n"
                   "  \"size\": %llu,\n"
                   "  \"synced\": %s,\n"
                   "  \"channels\": %s,\n"
                   "  \"sample_rate\": %s,\n"
                   "  \"mode\": \"%s\",\n"
                   "  \"recording\": false\n"
                   "}\n",
                   session_id, duration_sec, chunk_count,
                   (unsigned long long)session_bytes,
                   synced_str, channels_str, sample_rate_str, mode_str);

    if (len < 0 || len >= (int)sizeof(json_buf))
    {
        LOG_ERR("Failed to format session.json");
        fs_close(&file);
        return -ENOMEM;
    }

    written = fs_write(&file, json_buf, len);
    if (written != len)
    {
        LOG_ERR("Failed to write session.json: %zd != %d", written, len);
        fs_close(&file);
        return -EIO;
    }

    fs_close(&file);
    LOG_DBG("Updated session.json");

    return 0;
}

/* Bookmark support */

#define BOOKMARK_MAGIC "BMRK"
#define BOOKMARK_FILE_NAME "marks.bin"

static int get_bookmark_filepath(const char *session_id, char *filepath, size_t size)
{
    if (!session_id || !filepath)
    {
        return -EINVAL;
    }

    snprintf(filepath, size, "%s/%s/%s", STORAGE_BASE_PATH, session_id, BOOKMARK_FILE_NAME);

    return 0;
}

int storage_add_bookmark(const char *session_id, uint32_t offset_sec)
{
    char filepath[128];
    struct fs_file_t file;
    int rc;
    ssize_t ret;

    if (!sd_mounted || !session_id)
    {
        return -EINVAL;
    }

    /* Get file path */
    rc = get_bookmark_filepath(session_id, filepath, sizeof(filepath));
    if (rc != 0)
    {
        return rc;
    }

    /* Check if file exists */
    struct fs_dirent entry;
    bool file_exists = (fs_stat(filepath, &entry) == 0);

    if (file_exists)
    {
        /* Read current header and count */
        fs_file_t_init(&file);
        rc = fs_open(&file, filepath, FS_O_READ | FS_O_WRITE);
        if (rc != 0)
        {
            LOG_ERR("Failed to open bookmarks file: %d", rc);
            return rc;
        }

        /* Read header */
        uint8_t header[6];
        ret = fs_read(&file, header, sizeof(header));
        if (ret != sizeof(header))
        {
            LOG_ERR("Failed to read bookmark header: %zd", ret);
            fs_close(&file);
            return -EIO;
        }

        /* Verify magic */
        if (memcmp(header, BOOKMARK_MAGIC, 4) != 0)
        {
            LOG_ERR("Invalid bookmark magic");
            fs_close(&file);
            return -EIO;
        }

        /* Get and increment count */
        uint16_t count;
        memcpy(&count, &header[4], 2);
        count++;

        /* Seek back to count position */
        rc = fs_seek(&file, 4, FS_SEEK_SET);
        if (rc != 0)
        {
            LOG_ERR("Failed to seek to count position: %d", rc);
            fs_close(&file);
            return rc;
        }

        /* Write updated count */
        ret = fs_write(&file, &count, 2);
        if (ret != 2)
        {
            LOG_ERR("Failed to write bookmark count: %zd", ret);
            fs_close(&file);
            return -EIO;
        }

        /* Seek to end to append bookmark */
        rc = fs_seek(&file, 0, FS_SEEK_END);
        if (rc != 0)
        {
            LOG_ERR("Failed to seek to end: %d", rc);
            fs_close(&file);
            return rc;
        }

        /* Write bookmark */
        ret = fs_write(&file, &offset_sec, sizeof(uint32_t));
        if (ret != sizeof(uint32_t))
        {
            LOG_ERR("Failed to write bookmark: %zd", ret);
            fs_close(&file);
            return -EIO;
        }

        fs_close(&file);

        LOG_DBG("Added bookmark %u at %u seconds", count, offset_sec);
    }
    else
    {
        /* Create new file with header */
        fs_file_t_init(&file);
        rc = fs_open(&file, filepath, FS_O_CREATE | FS_O_WRITE);
        if (rc != 0)
        {
            LOG_ERR("Failed to create bookmarks file: %d", rc);
            return rc;
        }

        /* Write header */
        uint8_t header[6];
        memcpy(header, BOOKMARK_MAGIC, 4);
        uint16_t count = 1;
        memcpy(&header[4], &count, 2);
        ret = fs_write(&file, header, sizeof(header));
        if (ret != sizeof(header))
        {
            LOG_ERR("Failed to write bookmark header: %zd", ret);
            fs_close(&file);
            return -EIO;
        }

        /* Write first bookmark */
        ret = fs_write(&file, &offset_sec, sizeof(uint32_t));
        if (ret != sizeof(uint32_t))
        {
            LOG_ERR("Failed to write first bookmark: %zd", ret);
            fs_close(&file);
            return -EIO;
        }

        fs_close(&file);

        LOG_DBG("Created bookmarks file with first bookmark at %u seconds", offset_sec);
    }

    return 0;
}

int storage_get_bookmarks(const char *session_id, int page, int per_page,
                          struct storage_bookmark *bookmarks, int max_count)
{
    char filepath[128];
    struct fs_file_t file;
    uint8_t header[6];
    ssize_t ret;
    uint16_t count;
    int start_index, end_index;
    int rc;

    if (!sd_mounted || !session_id || !bookmarks || max_count <= 0)
    {
        return -EINVAL;
    }

    if (page < 1 || per_page < 1)
    {
        return -EINVAL;
    }

    /* Get file path */
    rc = get_bookmark_filepath(session_id, filepath, sizeof(filepath));
    if (rc != 0)
    {
        return rc;
    }

    /* Open file */
    fs_file_t_init(&file);
    rc = fs_open(&file, filepath, FS_O_READ);
    if (rc != 0)
    {
        /* File doesn't exist yet - no bookmarks */
        return 0;
    }

    /* Read header */
    ret = fs_read(&file, header, sizeof(header));
    if (ret != sizeof(header))
    {
        LOG_ERR("Invalid bookmark file");
        fs_close(&file);
        return -EIO;
    }

    /* Verify magic */
    if (memcmp(header, BOOKMARK_MAGIC, 4) != 0)
    {
        LOG_ERR("Invalid bookmark magic");
        fs_close(&file);
        return -EIO;
    }

    /* Get count */
    memcpy(&count, &header[4], 2);
    if (count > 10000)
    { /* Sanity check */
        LOG_WRN("Bookmark count too large: %u", count);
        count = 0;
    }

    /* Calculate pagination bounds */
    start_index = (page - 1) * per_page;
    if (start_index >= count)
    {
        /* Page beyond available data */
        fs_close(&file);
        return 0;
    }

    end_index = start_index + per_page;
    if (end_index > (int)count)
    {
        end_index = count;
    }

    /* Adjust max_count */
    if (max_count < (end_index - start_index))
    {
        end_index = start_index + max_count;
    }

    /* Seek to first bookmark in page */
    off_t seek_pos = sizeof(header) + (start_index * sizeof(uint32_t));
    ret = fs_seek(&file, seek_pos, FS_SEEK_SET);
    if (ret < 0)
    {
        LOG_ERR("Failed to seek to bookmark position");
        fs_close(&file);
        return ret;
    }

    /* Read bookmarks */
    int read_count = 0;
    for (int i = start_index; i < end_index; i++)
    {
        uint32_t offset_sec;
        ret = fs_read(&file, (uint8_t *)&offset_sec, sizeof(uint32_t));
        if (ret != sizeof(uint32_t))
        {
            break;
        }
        bookmarks[read_count].offset_sec = offset_sec;
        read_count++;
    }

    fs_close(&file);

    return read_count;
}

int storage_count_bookmarks(const char *session_id)
{
    char filepath[128];
    struct fs_file_t file;
    uint8_t header[6];
    ssize_t ret;
    uint16_t count = 0;
    int rc;

    if (!sd_mounted || !session_id)
    {
        return -EINVAL;
    }

    /* Get file path */
    rc = get_bookmark_filepath(session_id, filepath, sizeof(filepath));
    if (rc != 0)
    {
        return rc;
    }

    /* Open file */
    fs_file_t_init(&file);
    rc = fs_open(&file, filepath, FS_O_READ);
    if (rc != 0)
    {
        /* File doesn't exist yet - no bookmarks */
        return 0;
    }

    /* Read header */
    ret = fs_read(&file, header, sizeof(header));
    if (ret == sizeof(header))
    {
        /* Verify magic */
        if (memcmp(header, BOOKMARK_MAGIC, 4) == 0)
        {
            /* Get count */
            memcpy(&count, &header[4], 2);
        }
    }

    fs_close(&file);

    return count;
}

/* File writing state tracking for transfer coordination */

/**
 * @brief Set the current writing file
 *
 * Called by storage_create_file() to mark a file as being written.
 * Transfer thread waits for files to finish writing before transferring.
 *
 * @param session_id Session ID (NULL to clear)
 * @param filename Filename (NULL to clear)
 */
void storage_set_writing_file(const char *session_id, const char *filename)
{
    if (session_id && filename)
    {
        strncpy(writing_session, session_id, sizeof(writing_session) - 1);
        writing_session[sizeof(writing_session) - 1] = '\0';
        strncpy(writing_filename, filename, sizeof(writing_filename) - 1);
        writing_filename[sizeof(writing_filename) - 1] = '\0';
        LOG_DBG("Writing: %s/%s", writing_session, writing_filename);
    }
    else
    {
        /* Clear writing file info */
        writing_session[0] = '\0';
        writing_filename[0] = '\0';
        LOG_DBG("Writing cleared");
    }
}

/**
 * @brief Check if a file is currently being written
 *
 * @param session_id Session ID
 * @param filename Filename
 * @return true if file is being written, false otherwise
 */
bool storage_file_is_writing(const char *session_id, const char *filename)
{
    if (!session_id || !filename)
    {
        return false;
    }

    /* Check if session and filename match current writing file */
    return (strcmp(writing_session, session_id) == 0 &&
            strcmp(writing_filename, filename) == 0);
}

struct k_sem *storage_get_file_closed_sem(void)
{
    return &file_closed_sem;
}

/**
 * @brief Get the current writing file info
 *
 * @param out_session Output buffer for session ID (can be NULL)
 * @param out_filename Output buffer for filename (can be NULL)
 * @param session_size Size of session buffer
 * @param filename_size Size of filename buffer
 * @return true if a file is being written, false otherwise
 */
bool storage_get_writing_file(char *out_session, char *out_filename,
                              size_t session_size, size_t filename_size)
{
    bool is_writing = (writing_session[0] != '\0');

    if (is_writing)
    {
        if (out_session && session_size > 0)
        {
            strncpy(out_session, writing_session, session_size - 1);
            out_session[session_size - 1] = '\0';
        }
        if (out_filename && filename_size > 0)
        {
            strncpy(out_filename, writing_filename, filename_size - 1);
            out_filename[filename_size - 1] = '\0';
        }
    }

    return is_writing;
}
