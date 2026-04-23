/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

/* Storage configuration */
#define STORAGE_SESSION_ID_LEN 32
#define STORAGE_FILENAME_MAX_LEN 64

/**
 * @brief Storage statistics
 */
struct storage_stats {
    uint32_t total_chunks;      /* Total chunk files */
    uint64_t total_bytes;       /* Total bytes stored */
    uint32_t free_space_mb;     /* Free space in MB */
    bool is_mounted;            /* SD card mounted */
};

/**
 * @brief Chunk file handle (each Opus packet is a separate file)
 */
struct storage_chunk {
    char filename[32];          /* e.g., "000001.opus" */
    uint32_t chunk_index;       /* Chunk number */
    uint32_t bytes_written;     /* Bytes in this chunk */
    bool is_open;
};

/**
 * @brief Session information (matches clip format)
 */
struct storage_session_info {
    char session_id[STORAGE_SESSION_ID_LEN];
    uint32_t file_count;       /* Number of files in session (was chunk_count) */
    uint64_t total_bytes;      /* Total bytes in session */
    uint32_t synced_files;     /* Number of files successfully synced */
    uint32_t duration_sec;     /* Duration in seconds */
    uint8_t channels;          /* 1=mono, 2=stereo */
    uint8_t sample_rate_khz;   /* Sample rate in kHz (e.g., 16 for 16000Hz) */
    char mode[16];             /* "normal" or "enhanced" */
};

/**
 * @brief Initialize storage subsystem
 *
 * @return 0 on success, negative error code on failure
 */
int storage_init(void);

/**
 * @brief Cleanup storage subsystem
 */
void storage_cleanup(void);

/**
 * @brief Remount SD card after MSC disable
 *
 * Re-initializes disk and mounts FATFS.
 *
 * @return 0 on success, negative error code on failure
 */
int storage_remount(void);

/**
 * @brief Check if SD card is mounted
 *
 * @return true if mounted, false otherwise
 */
bool storage_is_mounted(void);

/**
 * @brief Get storage statistics
 *
 * @param stats Output statistics structure
 * @return 0 on success, negative error code on failure
 */
int storage_get_stats(struct storage_stats *stats);

/**
 * @brief Create a new recording session
 *
 * Creates /SD:/REC/<session_id>/ directory with:
 * - session.json (metadata)
 * - marks.bin (empty bookmark file)
 *
 * @param session_id Session ID (14 digits: YYYYMMDDHHMMSS)
 * @param channels Audio channels (1=mono, 2=stereo)
 * @param sample_rate Sample rate in Hz (e.g., 16000)
 * @param mode Recording mode ("normal" or "enhanced")
 * @return 0 on success, negative error code on failure
 */
int storage_create_session(const char *session_id, uint8_t channels,
                          uint32_t sample_rate, const char *mode);

/**
 * @brief Close a recording session
 *
 * Updates session.json with final duration and chunk count.
 *
 * @param session_id Session ID
 * @param duration_sec Recording duration in seconds
 * @param chunk_count Number of chunk files
 * @return 0 on success, negative error code on failure
 */
int storage_close_session(const char *session_id, uint32_t duration_sec,
                         uint32_t chunk_count);

/**
 * @brief Write a chunk (Opus packet) as a separate file
 *
 * Each Opus packet is saved as a separate file: 000001.opus, 000002.opus, ...
 *
 * @param session_id Session ID
 * @param chunk_index Chunk number (1-based)
 * @param data Opus encoded data
 * @param len Length of data
 * @return 0 on success, negative error code on failure
 */
int storage_write_chunk(const char *session_id, uint32_t chunk_index,
                       const uint8_t *data, uint32_t len);

/**
 * @brief Storage file handle for continuous writing
 */
struct storage_file {
    char filename[STORAGE_FILENAME_MAX_LEN];
    char session_id[STORAGE_SESSION_ID_LEN];
    uint32_t chunk_index;
    uint32_t bytes_written;
    uint32_t frames_written;
    bool is_open;
    struct fs_file_t internal_file;  /* Internal file handle */
};

/**
 * @brief Create a new storage file
 *
 * @param file Storage file handle
 * @param session_id Session ID
 * @param chunk_index Chunk number (1-based)
 * @return 0 on success, negative error code on failure
 */
int storage_create_file(struct storage_file *file, const char *session_id, uint32_t chunk_index);

/**
 * @brief Write an Opus frame to storage file (with 2-byte length header)
 *
 * Format: [2-byte little-endian length][Opus data]
 *
 * @param file Storage file handle
 * @param data Opus encoded data
 * @param len Length of data (max 65535)
 * @return 0 on success, negative error code on failure
 */
int storage_write_frame(struct storage_file *file, const uint8_t *data, uint32_t len);

/**
 * @brief Close storage file
 *
 * @param file Storage file handle
 * @return 0 on success, negative error code on failure
 */
int storage_close_file(struct storage_file *file);

/**
 * @brief Read a chunk file
 *
 * @param session_id Session ID
 * @param chunk_index Chunk number (1-based)
 * @param data Buffer to read data into
 * @param len Buffer size
 * @return Number of bytes read, or negative error code on failure
 */
int storage_read_chunk(const char *session_id, uint32_t chunk_index,
                      uint8_t *data, uint32_t len);

/**
 * @brief Delete a chunk file
 *
 * @param session_id Session ID
 * @param chunk_index Chunk number (1-based)
 * @return 0 on success, negative error code on failure
 */
int storage_delete_chunk(const char *session_id, uint32_t chunk_index);

/**
 * @brief List all sessions
 *
 * @param sessions Output array for session info
 * @param max_sessions Maximum sessions to return
 * @return Number of sessions found, or negative error code
 */
int storage_list_sessions(struct storage_session_info *sessions, int max_sessions);

/**
 * @brief Count total number of sessions (fast, doesn't read session details)
 *
 * @return Total session count, or negative error code
 */
int storage_count_sessions(void);

/**
 * @brief List session IDs only (fast, no session.json reads)
 *
 * Scans REC directory for valid session directories and returns
 * sorted session IDs. Does not read session.json or any other files.
 * Empty sessions (no .opus files) are automatically deleted.
 *
 * @param ids Output array for session IDs (each at least 16 bytes)
 * @param max_ids Maximum number of IDs to return
 * @return Number of IDs found, or negative error code
 */
int storage_list_session_ids(char ids[][16], int max_ids);

/**
 * @brief List sessions with pagination support
 *
 * @param sessions Output array for session info
 * @param offset Starting offset (0-based)
 * @param limit Maximum number of sessions to return
 * @return Number of sessions returned, or negative error code
 */
int storage_list_sessions_paginated(struct storage_session_info *sessions,
                                   int offset, int limit);

/**
 * @brief Get session information
 *
 * @param session_id Session ID
 * @param info Output session info structure
 * @return 0 on success, negative error code on failure
 */
int storage_get_session_info(const char *session_id, struct storage_session_info *info);

/**
 * @brief List chunk files in a session
 *
 * @param session_id Session ID
 * @param chunks Output array for chunk indices
 * @param max_chunks Maximum chunks to return
 * @return Number of chunks found, or negative error code
 */
int storage_list_chunks(const char *session_id, uint32_t *chunks, int max_chunks, int skip);

/**
 * @brief Delete a session and all its chunks
 *
 * @param session_id Session ID to delete
 * @return 0 on success, negative error code on failure
 */
int storage_delete_session(const char *session_id);

/**
 * @brief Format SD card
 *
 * @return 0 on success, negative error code on failure
 */
int storage_format_card(void);

/* Bookmark support */
/**
 * @brief Bookmark entry
 */
struct storage_bookmark {
    uint32_t offset_sec;    /* Seconds from session start */
};

/**
 * @brief Add a bookmark to a session
 *
 * @param session_id Session ID
 * @param offset_sec Seconds from session start
 * @return 0 on success, negative error code on failure
 */
int storage_add_bookmark(const char *session_id, uint32_t offset_sec);

/**
 * @brief Get bookmarks for a session (with pagination)
 *
 * File format: [4 bytes magic "BMRK"][2 bytes count][N * 4 byte offsets]
 *
 * @param session_id Session ID
 * @param page Page number (1-based)
 * @param per_page Items per page
 * @param bookmarks Output array for bookmarks
 * @param max_count Maximum number of bookmarks to return
 * @return Number of bookmarks found, or negative error code
 */
int storage_get_bookmarks(const char *session_id, int page, int per_page,
                          struct storage_bookmark *bookmarks, int max_count);

/**
 * @brief Get bookmark count for a session
 *
 * @param session_id Session ID
 * @return Number of bookmarks, or negative error code
 */
int storage_count_bookmarks(const char *session_id);

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
void storage_set_writing_file(const char *session_id, const char *filename);

/**
 * @brief Check if a file is currently being written
 *
 * @param session_id Session ID
 * @param filename Filename
 * @return true if file is being written, false otherwise
 */
bool storage_file_is_writing(const char *session_id, const char *filename);

/**
 * @brief Get the file closed semaphore
 *
 * This semaphore is given when a file is closed and ready for transfer.
 * Transfer thread can wait on this instead of polling storage_file_is_writing().
 *
 * @return Pointer to the semaphore, or NULL if storage not initialized
 */
struct k_sem *storage_get_file_closed_sem(void);

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
                              size_t session_size, size_t filename_size);

#endif /* STORAGE_H */
