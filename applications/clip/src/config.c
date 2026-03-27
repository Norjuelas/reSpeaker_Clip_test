/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/settings/settings.h>
#include <time.h>

#include "clip.h"
#include "config.h"

LOG_MODULE_REGISTER(config, CONFIG_CLIP_LOG_LEVEL);

/* Settings keys */
#define SETTING_BITRATE         "config/bitrate"
#define SETTING_COMPLEXITY      "config/complexity"
#define SETTING_MODE            "config/mode"
#define SETTING_NOISE_SUPPRESS  "config/noise_suppress"
#define SETTING_CHUNK_SIZE      "config/chunk_size"
#define SETTING_AUTODEL         "config/auto_delete_days"
#define SETTING_AGC_ENABLED     "config/agc_enabled"
#define SETTING_AGC_TARGET      "config/agc_target"
#define SETTING_DEREVERB        "config/dereverb_enabled"
#define SETTING_CONTRAST        "config/oled_contrast"
#define SETTING_TIME_UNIX       "time/unix_timestamp"

/* Config entry for settings handler */
struct config_entry {
    const char *name;
    size_t offset;
    size_t size;
};

/* Config entries table */
static const struct config_entry config_table[] = {
    { SETTING_BITRATE,        offsetof(struct clip_config, bitrate),        sizeof(uint16_t) },
    { SETTING_COMPLEXITY,     offsetof(struct clip_config, complexity),     sizeof(uint8_t) },
    { SETTING_MODE,           offsetof(struct clip_config, mode),           sizeof(uint8_t) },
    { SETTING_NOISE_SUPPRESS, offsetof(struct clip_config, noise_suppress), sizeof(uint8_t) },
    { SETTING_CHUNK_SIZE,     offsetof(struct clip_config, chunk_size),     sizeof(uint16_t) },
    { SETTING_AUTODEL,        offsetof(struct clip_config, auto_delete_days), sizeof(int8_t) },
    { SETTING_AGC_ENABLED,    offsetof(struct clip_config, agc_enabled),    sizeof(bool) },
    { SETTING_AGC_TARGET,     offsetof(struct clip_config, agc_target),     sizeof(uint16_t) },
    { SETTING_DEREVERB,       offsetof(struct clip_config, dereverb_enabled), sizeof(bool) },
    { SETTING_CONTRAST,       offsetof(struct clip_config, oled_contrast),  sizeof(uint8_t) },
};

#define CONFIG_TABLE_SIZE (sizeof(config_table) / sizeof(config_table[0]))

/* Settings handler for config */
static int config_settings_set(const char *name, size_t len,
                               settings_read_cb read_cb, void *cb_arg)
{
    struct clip_context *ctx = clip_get_context();

    if (!name) {
        return -ENOENT;
    }

    for (size_t i = 0; i < CONFIG_TABLE_SIZE; i++) {
        const struct config_entry *entry = &config_table[i];
        const char *key = entry->name + 7;  /* Skip "config/" */

        if (strcmp(name, key) == 0) {
            uint8_t buffer[16];
            int rc = read_cb(cb_arg, buffer, entry->size);
            if (rc < 0) {
                return rc;
            }
            memcpy((uint8_t *)&ctx->config + entry->offset, buffer, entry->size);
            return 0;
        }
    }

    return -ENOENT;
}

static struct settings_handler config_handler = {
    .name = "config",
    .h_set = config_settings_set,
};

/* Settings handler for time */
static int time_settings_set(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg)
{
    struct clip_context *ctx = clip_get_context();

    if (!name || strcmp(name, "unix_timestamp") != 0) {
        return -ENOENT;
    }

    int64_t saved_unix_time;
    int rc = read_cb(cb_arg, &saved_unix_time, sizeof(saved_unix_time));
    if (rc != sizeof(saved_unix_time)) {
        return -EINVAL;
    }

    /* Restore time from storage */
    time_t unix_time = (time_t)saved_unix_time;
    struct tm tm;
    gmtime_r(&unix_time, &tm);

    ctx->time.year = tm.tm_year + 1900;
    ctx->time.month = tm.tm_mon + 1;
    ctx->time.day = tm.tm_mday;
    ctx->time.hour = tm.tm_hour;
    ctx->time.min = tm.tm_min;
    ctx->time.sec = tm.tm_sec;
    ctx->time.base_uptime_ms = k_uptime_get();
    ctx->time.valid = true;

    LOG_INF("Time restored: %04d-%02d-%02d %02d:%02d:%02d",
            ctx->time.year, ctx->time.month, ctx->time.day,
            ctx->time.hour, ctx->time.min, ctx->time.sec);

    return 0;
}

static struct settings_handler time_handler = {
    .name = "time",
    .h_set = time_settings_set,
};

/* LittleFS mount configuration */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_storage_cfg);

static struct fs_mount_t lfs_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_storage_cfg,
    .mnt_point = "/lfs",
    .storage_dev = (void *)FIXED_PARTITION_ID(lfs_storage),
};

static int mount_littlefs(void)
{
    int rc = fs_mount(&lfs_mnt);
    if (rc != 0 && rc != -EBUSY) {
        LOG_ERR("LittleFS mount failed: %d", rc);
        return rc;
    }
    LOG_INF("LittleFS mounted at %s", lfs_mnt.mnt_point);
    return 0;
}

/* Set default configuration values */
static void config_set_defaults(struct clip_context *ctx)
{
    ctx->config.bitrate = CONFIG_DEFAULT_BITRATE;
    ctx->config.complexity = CONFIG_DEFAULT_COMPLEXITY;
    ctx->config.mode = MODE_NORMAL;
    ctx->config.noise_suppress = CONFIG_DEFAULT_NOISE;
    ctx->config.chunk_size = CONFIG_DEFAULT_CHUNK_SIZE;
    ctx->config.auto_delete_days = -1;
    ctx->config.agc_target = CONFIG_DEFAULT_AGC_TARGET;
    ctx->config.agc_enabled = CONFIG_DEFAULT_AGC_ENABLED;
    ctx->config.dereverb_enabled = false;
    ctx->config.oled_contrast = CONFIG_DEFAULT_CONTRAST;
}

int config_init(void)
{
    struct clip_context *ctx = clip_get_context();
    int err;

    /* Set defaults first */
    config_set_defaults(ctx);

    /* Mount LittleFS */
    err = mount_littlefs();
    if (err) {
        LOG_WRN("LittleFS mount failed: %d", err);
        return 0;  /* Continue with defaults */
    }

    // /* TEMPORARY: Clear all settings to fix pairing issues */
    // LOG_WRN("=== CLEARING ALL SETTINGS ===");

    // /* Delete the settings file to clear all data */
    // int rc = fs_unlink(CONFIG_SETTINGS_FILE_PATH);
    // if (rc == 0) {
    //     LOG_INF("Settings file deleted - all data cleared");
    // } else {
    //     LOG_WRN("Settings file unlink failed: %d (file may not exist yet)", rc);
    // }

    /* Initialize settings subsystem */
    err = settings_subsys_init();
    if (err) {
        LOG_WRN("Settings init failed: %d", err);
        return 0;
    }

    /* Register handlers */
    err = settings_register(&config_handler);
    if (err && err != -EEXIST) {
        LOG_WRN("Config handler register failed: %d", err);
    }

    err = settings_register(&time_handler);
    if (err && err != -EEXIST) {
        LOG_WRN("Time handler register failed: %d", err);
    }

    /* Load config settings */
    err = settings_load_subtree("config");
    if (err) {
        LOG_WRN("Config load failed: %d, saving defaults", err);
        config_save();
    } else {
        LOG_INF("Config loaded: bitrate=%u, complexity=%u, mode=%d",
                ctx->config.bitrate, ctx->config.complexity, ctx->config.mode);
    }

    /* Load time settings (optional) */
    settings_load_subtree("time");

    LOG_INF("Configuration initialized");

    return 0;
}

int config_load(void)
{
    return settings_load();
}

int config_save(void)
{
    struct clip_context *ctx = clip_get_context();
    int err;

    for (size_t i = 0; i < CONFIG_TABLE_SIZE; i++) {
        const struct config_entry *entry = &config_table[i];
        err = settings_save_one(entry->name,
                               (const uint8_t *)&ctx->config + entry->offset,
                               entry->size);
        if (err) {
            LOG_ERR("Failed to save %s: %d", entry->name, err);
            return err;
        }
    }

    return 0;
}

int config_factory_reset(void)
{
    struct clip_context *ctx = clip_get_context();
    config_set_defaults(ctx);
    return config_save();
}

static const char *key_to_setting(uint16_t key)
{
    switch (key) {
    case CONFIG_KEY_BITRATE:    return SETTING_BITRATE;
    case CONFIG_KEY_COMPLEXITY: return SETTING_COMPLEXITY;
    case CONFIG_KEY_MODE:       return SETTING_MODE;
    case CONFIG_KEY_NOISE:      return SETTING_NOISE_SUPPRESS;
    case CONFIG_KEY_CHUNK_SIZE: return SETTING_CHUNK_SIZE;
    case CONFIG_KEY_AUTODEL:    return SETTING_AUTODEL;
    case CONFIG_KEY_AGC_ENABLE: return SETTING_AGC_ENABLED;
    case CONFIG_KEY_AGC_TARGET: return SETTING_AGC_TARGET;
    case CONFIG_KEY_DEREVERB:   return SETTING_DEREVERB;
    case CONFIG_KEY_CONTRAST:   return SETTING_CONTRAST;
    default:                    return NULL;
    }
}

int config_set(uint16_t key, const void *value, size_t len)
{
    struct clip_context *ctx = clip_get_context();
    int ret = 0;

    /* Update in-memory config */
    switch (key) {
    case CONFIG_KEY_BITRATE:
        if (len == sizeof(uint16_t)) {
            ctx->config.bitrate = *(const uint16_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_COMPLEXITY:
        if (len == sizeof(uint8_t)) {
            ctx->config.complexity = *(const uint8_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_MODE:
        if (len == sizeof(uint8_t)) {
            ctx->config.mode = *(const uint8_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_NOISE:
        if (len == sizeof(uint8_t)) {
            ctx->config.noise_suppress = *(const uint8_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_CHUNK_SIZE:
        if (len == sizeof(uint16_t)) {
            ctx->config.chunk_size = *(const uint16_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_AUTODEL:
        if (len == sizeof(int8_t)) {
            ctx->config.auto_delete_days = *(const int8_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_AGC_ENABLE:
        if (len == sizeof(bool)) {
            ctx->config.agc_enabled = *(const bool *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_AGC_TARGET:
        if (len == sizeof(uint16_t)) {
            ctx->config.agc_target = *(const uint16_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_DEREVERB:
        if (len == sizeof(bool)) {
            ctx->config.dereverb_enabled = *(const bool *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_CONTRAST:
        if (len == sizeof(uint8_t)) {
            ctx->config.oled_contrast = *(const uint8_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    default:
        return -EINVAL;
    }

    if (ret) {
        return ret;
    }

    /* Save to storage */
    const char *setting = key_to_setting(key);
    if (setting) {
        return settings_save_one(setting, value, len);
    }

    return 0;
}

int config_get(uint16_t key, void *value, size_t len)
{
    struct clip_context *ctx = clip_get_context();

    switch (key) {
    case CONFIG_KEY_BITRATE:
        if (len == sizeof(uint16_t)) {
            *(uint16_t *)value = ctx->config.bitrate;
            return 0;
        }
        break;
    case CONFIG_KEY_COMPLEXITY:
        if (len == sizeof(uint8_t)) {
            *(uint8_t *)value = ctx->config.complexity;
            return 0;
        }
        break;
    case CONFIG_KEY_MODE:
        if (len == sizeof(uint8_t)) {
            *(uint8_t *)value = ctx->config.mode;
            return 0;
        }
        break;
    case CONFIG_KEY_NOISE:
        if (len == sizeof(uint8_t)) {
            *(uint8_t *)value = ctx->config.noise_suppress;
            return 0;
        }
        break;
    case CONFIG_KEY_CHUNK_SIZE:
        if (len == sizeof(uint16_t)) {
            *(uint16_t *)value = ctx->config.chunk_size;
            return 0;
        }
        break;
    case CONFIG_KEY_AUTODEL:
        if (len == sizeof(int8_t)) {
            *(int8_t *)value = ctx->config.auto_delete_days;
            return 0;
        }
        break;
    case CONFIG_KEY_AGC_ENABLE:
        if (len == sizeof(bool)) {
            *(bool *)value = ctx->config.agc_enabled;
            return 0;
        }
        break;
    case CONFIG_KEY_AGC_TARGET:
        if (len == sizeof(uint16_t)) {
            *(uint16_t *)value = ctx->config.agc_target;
            return 0;
        }
        break;
    case CONFIG_KEY_DEREVERB:
        if (len == sizeof(bool)) {
            *(bool *)value = ctx->config.dereverb_enabled;
            return 0;
        }
        break;
    case CONFIG_KEY_CONTRAST:
        if (len == sizeof(uint8_t)) {
            *(uint8_t *)value = ctx->config.oled_contrast;
            return 0;
        }
        break;
    default:
        return -EINVAL;
    }

    return -EINVAL;
}

int config_set_time(int64_t unix_time)
{
    struct clip_context *ctx = clip_get_context();

    /* Update in-memory time */
    time_t t = (time_t)unix_time;
    struct tm tm;
    gmtime_r(&t, &tm);

    ctx->time.year = tm.tm_year + 1900;
    ctx->time.month = tm.tm_mon + 1;
    ctx->time.day = tm.tm_mday;
    ctx->time.hour = tm.tm_hour;
    ctx->time.min = tm.tm_min;
    ctx->time.sec = tm.tm_sec;
    ctx->time.base_uptime_ms = k_uptime_get();
    ctx->time.valid = true;

    /* Save to storage */
    return settings_save_one(SETTING_TIME_UNIX, &unix_time, sizeof(unix_time));
}

int config_get_time(int64_t *unix_time)
{
    struct clip_context *ctx = clip_get_context();

    if (!unix_time) {
        return -EINVAL;
    }

    if (!ctx->time.valid) {
        return -ENODATA;
    }

    /* Calculate current Unix time */
    int64_t elapsed_ms = k_uptime_get() - ctx->time.base_uptime_ms;
    int64_t elapsed_sec = elapsed_ms / 1000;

    /* Convert to Unix timestamp */
    struct tm tm = {
        .tm_year = ctx->time.year - 1900,
        .tm_mon = ctx->time.month - 1,
        .tm_mday = ctx->time.day,
        .tm_hour = ctx->time.hour,
        .tm_min = ctx->time.min,
        .tm_sec = ctx->time.sec,
    };
    *unix_time = (int64_t)mktime(&tm) + elapsed_sec;

    return 0;
}

/* Convenience functions for individual config items */
int config_set_time_ymd(uint16_t year, uint8_t month, uint8_t day,
                        uint8_t hour, uint8_t min, uint8_t sec)
{
    struct clip_context *ctx = clip_get_context();

    /* Convert to Unix timestamp */
    struct tm tm = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = min,
        .tm_sec = sec,
    };
    time_t unix_time = mktime(&tm);
    if (unix_time == -1) {
        return -EINVAL;
    }

    ctx->time.year = year;
    ctx->time.month = month;
    ctx->time.day = day;
    ctx->time.hour = hour;
    ctx->time.min = min;
    ctx->time.sec = sec;
    ctx->time.base_uptime_ms = k_uptime_get();
    ctx->time.valid = true;

    return settings_save_one(SETTING_TIME_UNIX, &unix_time, sizeof(unix_time));
}

int config_set_bitrate(uint16_t bitrate)
{
    return config_set(CONFIG_KEY_BITRATE, &bitrate, sizeof(bitrate));
}

int config_set_complexity(uint8_t complexity)
{
    return config_set(CONFIG_KEY_COMPLEXITY, &complexity, sizeof(complexity));
}

int config_set_mode(enum recording_mode mode)
{
    return config_set(CONFIG_KEY_MODE, &mode, sizeof(mode));
}

int config_set_noise_suppress(uint8_t noise)
{
    return config_set(CONFIG_KEY_NOISE, &noise, sizeof(noise));
}

int config_set_chunk_size(uint16_t chunk_size)
{
    return config_set(CONFIG_KEY_CHUNK_SIZE, &chunk_size, sizeof(chunk_size));
}

int config_set_auto_delete_days(int8_t days)
{
    return config_set(CONFIG_KEY_AUTODEL, &days, sizeof(days));
}

int config_set_agc_enabled(bool enabled)
{
    return config_set(CONFIG_KEY_AGC_ENABLE, &enabled, sizeof(enabled));
}

int config_set_agc_target(uint16_t target)
{
    return config_set(CONFIG_KEY_AGC_TARGET, &target, sizeof(target));
}

int config_set_dereverb_enabled(bool enabled)
{
    return config_set(CONFIG_KEY_DEREVERB, &enabled, sizeof(enabled));
}

int config_set_oled_contrast(uint8_t contrast)
{
    return config_set(CONFIG_KEY_CONTRAST, &contrast, sizeof(contrast));
}
