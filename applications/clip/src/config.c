/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
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
#define SETTING_MODE            "config/mode"
#define SETTING_NOISE_SUPPRESS  "config/noise_suppress"
#define SETTING_AUTODEL         "config/auto_delete_days"
#define SETTING_DEREVERB        "config/dereverb_enabled"
#define SETTING_BRIGHTNESS      "config/oled_brightness"
#define SETTING_WIFI_PASSWORD   "config/wifi_password"
#define SETTING_TIME_UNIX       "time/unix_timestamp"

/* Config entry for settings handler */
struct config_entry {
    const char *name;
    size_t offset;
    size_t size;
};

/* Config entries table */
static const struct config_entry config_table[] = {
    { SETTING_MODE,           offsetof(struct clip_config, mode),           sizeof(uint8_t) },
    { SETTING_NOISE_SUPPRESS, offsetof(struct clip_config, noise_suppress), sizeof(uint8_t) },
    { SETTING_AUTODEL,        offsetof(struct clip_config, auto_delete_days), sizeof(int8_t) },
    { SETTING_DEREVERB,       offsetof(struct clip_config, dereverb_enabled), sizeof(bool) },
    { SETTING_BRIGHTNESS,       offsetof(struct clip_config, oled_brightness),  sizeof(uint8_t) },
    { SETTING_WIFI_PASSWORD,    offsetof(struct clip_config, wifi_password),    sizeof(char[9]) },
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
            uint8_t buffer[32];
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
    ctx->config.mode = MODE_NORMAL;
    ctx->config.noise_suppress = CONFIG_CLIP_DEFAULT_NOISE;
    ctx->config.auto_delete_days = CONFIG_CLIP_DEFAULT_AUTODEL;
    ctx->config.dereverb_enabled = IS_ENABLED(CONFIG_CLIP_DEFAULT_DEREVERB);
    ctx->config.oled_brightness = CONFIG_CLIP_DEFAULT_BRIGHTNESS;
    ctx->config.wifi_password[0] = '\0';
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
        LOG_INF("Config loaded: mode=%d", ctx->config.mode);
    }

    /* Load time settings (optional) */
    settings_load_subtree("time");

    LOG_INF("Configuration initialized");

    /* Generate WiFi password if not set */
    if (ctx->config.wifi_password[0] == '\0') {
        config_generate_wifi_password();
    }

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
    case CONFIG_KEY_MODE:       return SETTING_MODE;
    case CONFIG_KEY_NOISE:      return SETTING_NOISE_SUPPRESS;
    case CONFIG_KEY_AUTODEL:    return SETTING_AUTODEL;
    case CONFIG_KEY_DEREVERB:   return SETTING_DEREVERB;
    case CONFIG_KEY_BRIGHTNESS: return SETTING_BRIGHTNESS;
    case CONFIG_KEY_WIFI_PASSWORD: return SETTING_WIFI_PASSWORD;
    default:                    return NULL;
    }
}

int config_set(uint16_t key, const void *value, size_t len)
{
    struct clip_context *ctx = clip_get_context();
    int ret = 0;

    /* Update in-memory config */
    switch (key) {
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
    case CONFIG_KEY_AUTODEL:
        if (len == sizeof(int8_t)) {
            ctx->config.auto_delete_days = *(const int8_t *)value;
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
    case CONFIG_KEY_BRIGHTNESS:
        if (len == sizeof(uint8_t)) {
            ctx->config.oled_brightness = *(const uint8_t *)value;
        } else {
            ret = -EINVAL;
        }
        break;
    case CONFIG_KEY_WIFI_PASSWORD:
        if (len <= 8) {
            strncpy(ctx->config.wifi_password, value, len);
            ctx->config.wifi_password[len] = '\0';
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
    case CONFIG_KEY_AUTODEL:
        if (len == sizeof(int8_t)) {
            *(int8_t *)value = ctx->config.auto_delete_days;
            return 0;
        }
        break;
    case CONFIG_KEY_DEREVERB:
        if (len == sizeof(bool)) {
            *(bool *)value = ctx->config.dereverb_enabled;
            return 0;
        }
        break;
    case CONFIG_KEY_BRIGHTNESS:
        if (len == sizeof(uint8_t)) {
            *(uint8_t *)value = ctx->config.oled_brightness;
            return 0;
        }
        break;
    case CONFIG_KEY_WIFI_PASSWORD:
        if (len >= 9) {
            strncpy(value, ctx->config.wifi_password, 9);
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

int config_set_mode(enum recording_mode mode)
{
    return config_set(CONFIG_KEY_MODE, &mode, sizeof(mode));
}

int config_set_noise_suppress(uint8_t noise)
{
    return config_set(CONFIG_KEY_NOISE, &noise, sizeof(noise));
}

int config_set_auto_delete_days(int8_t days)
{
    return config_set(CONFIG_KEY_AUTODEL, &days, sizeof(days));
}

int config_set_dereverb_enabled(bool enabled)
{
    return config_set(CONFIG_KEY_DEREVERB, &enabled, sizeof(enabled));
}

int config_set_oled_brightness(uint8_t brightness)
{
    return config_set(CONFIG_KEY_BRIGHTNESS, &brightness, sizeof(brightness));
}

/* ========================================================================== */
/* WiFi Password Management                                                     */
/* ========================================================================== */

/* Character set: alphanumeric, excluding easily confused 0/O/l/1/I */
static const char password_chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#define PASSWORD_CHAR_COUNT (sizeof(password_chars) - 1)

int config_generate_wifi_password(void)
{
    struct clip_context *ctx = clip_get_context();
    uint8_t rand_bytes[8];

    sys_rand_get(rand_bytes, sizeof(rand_bytes));

    for (int i = 0; i < 8; i++) {
        ctx->config.wifi_password[i] = password_chars[rand_bytes[i] % PASSWORD_CHAR_COUNT];
    }
    ctx->config.wifi_password[8] = '\0';

    int err = settings_save_one(SETTING_WIFI_PASSWORD,
                            (const uint8_t *)ctx->config.wifi_password, 9);
    if (err) {
        LOG_ERR("Failed to save wifi_password: %d", err);
        return err;
    }

    LOG_INF("WiFi password generated: %s", ctx->config.wifi_password);
    return 0;
}

int config_set_wifi_password(const char *password)
{
    return config_set(CONFIG_KEY_WIFI_PASSWORD, password, strlen(password));
}

const char *config_get_wifi_password(void)
{
    struct clip_context *ctx = clip_get_context();

    if (ctx->config.wifi_password[0] == '\0') {
        return "12345678";  /* Fallback if not generated yet */
    }

    return ctx->config.wifi_password;
}
