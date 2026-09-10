/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_CONFIG_H
#define CLIP_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include "clip.h"

/* Configuration keys */
#define CONFIG_KEY_MODE          0x03
#define CONFIG_KEY_NOISE         0x04
#define CONFIG_KEY_AUTODEL       0x06
#define CONFIG_KEY_DEREVERB      0x09
#define CONFIG_KEY_BRIGHTNESS    0x0A
#define CONFIG_KEY_WIFI_PASSWORD 0x0B
#define CONFIG_KEY_DEVICE_NAME   0x0C
#define CONFIG_KEY_WIFI_CHANNEL  0x0D
#define CONFIG_KEY_WIFI_REG_DOMAIN 0x0E
#define CONFIG_KEY_STA_SSID      0x0F
#define CONFIG_KEY_STA_PSK       0x10
#define CONFIG_KEY_UPLOAD_HOST   0x11
#define CONFIG_KEY_UPLOAD_PORT   0x12

/**
 * @brief Por que se apago o reinicio el aparato la vez anterior.
 *
 * `reset_cause` del latido dice COMO (power-on, software, watchdog) pero no
 * POR QUE, y ahi se confunden tres cosas muy distintas: alguien lo apago, se
 * quedo sin bateria, o el firmware se reinicio solo. Esto las separa.
 */
enum clip_boot_reason {
	CLIP_BOOT_UNKNOWN = 0,  /* corte de corriente, bateria, AT+REBOOT, cuelgue */
	CLIP_BOOT_CLEAN   = 1,  /* apagado deliberado: se entro en ship mode */
	CLIP_BOOT_WEDGE   = 2,  /* el detector de radio colgada reinicio en frio */
};

/**
 * @brief Lo que el arranque anterior quiso dejar dicho.
 *
 * Existe porque los contadores de diagnostico (`miss`, `miss_stage`, `tdfail`)
 * viven en RAM, y el reinicio del detector de radio colgada -- justo el caso que
 * mas interesa explicar -- se los lleva. Sin esto, un aparato de tienda llega al
 * servidor con todo a cero y `reset: software`: dice que se rindio, no por que.
 *
 * Se escribe antes de un reinicio deliberado y se lee una vez al arrancar. El
 * log de la tarjeta cubre el resto, pero ese exige tener el aparato en la mano;
 * esto viaja en el latido.
 */
struct clip_boot_note {
	uint8_t  version;      /* 1 */
	uint8_t  reason;       /* enum clip_boot_reason */
	uint8_t  miss_stage;   /* enum clip_win_stage de la ultima ventana mala */
	uint8_t  reserved;
	uint16_t miss;         /* ventanas seguidas sin exito al morir */
	uint16_t tdfail;       /* net_if_down() fallidos acumulados */
	uint32_t boots;        /* arranques desde el aprovisionamiento */
	uint32_t uptime_s;     /* cuanto llevaba encendido */
};

#define CLIP_BOOT_NOTE_VERSION 1

/**
 * @brief Guardar la nota de arranque. Se llama antes de reiniciar o apagar.
 */
int config_save_boot_note(const struct clip_boot_note *note);

/**
 * @brief Leer la nota que dejo el arranque anterior.
 *
 * @retval 0        habia nota y es de una version conocida
 * @retval -ENOENT  no hay nota (aparato nuevo, o el arranque anterior murio sin avisar)
 */
int config_load_boot_note(struct clip_boot_note *note);


/**
 * @brief Initialize configuration system
 *
 * @return 0 on success, negative error code on failure
 */
int config_init(void);

/**
 * @brief Load configuration from storage
 *
 * @return 0 on success, negative error code on failure
 */
int config_load(void);

/**
 * @brief Save configuration to storage
 *
 * @return 0 on success, negative error code on failure
 */
int config_save(void);

/**
 * @brief Reset configuration to factory defaults
 *
 * @return 0 on success, negative error code on failure
 */
int config_factory_reset(void);

/**
 * @brief Set configuration value
 *
 * @param key   Configuration key
 * @param value Pointer to value
 * @param len   Length of value
 * @return 0 on success, negative error code on failure
 */
int config_set(uint16_t key, const void *value, size_t len);

/**
 * @brief Get configuration value
 *
 * @param key    Configuration key
 * @param value  Output buffer
 * @param len    Length of buffer
 * @return 0 on success, negative error code on failure
 */
int config_get(uint16_t key, void *value, size_t len);

/**
 * @brief Save Unix timestamp to storage
 *
 * @param unix_time Unix timestamp
 * @return 0 on success, negative error code on failure
 */
int config_set_time(int64_t unix_time);

/**
 * @brief Get Unix timestamp from storage
 *
 * @param unix_time Output buffer for Unix timestamp
 * @return 0 on success, negative error code if time not set
 */
int config_get_time(int64_t *unix_time);

/**
 * @brief Set time values (YMDHMS)
 *
 * @param year Year
 * @param month Month (1-12)
 * @param day Day (1-31)
 * @param hour Hour (0-23)
 * @param min Minute (0-59)
 * @param sec Second (0-59)
 * @return 0 on success, negative error code on failure
 */
int config_set_time_ymd(uint16_t year, uint8_t month, uint8_t day,
                        uint8_t hour, uint8_t min, uint8_t sec);

/**
 * @brief Sync time baseline using current compensated time
 *
 * Recalculates current time from uptime compensation, then writes
 * it back as the new baseline. Call periodically (e.g. on recording
 * stop) to prevent long-term drift in the uptime-based clock.
 */
void config_sync_time(void);

/**
 * @brief Set recording mode
 *
 * @param mode Recording mode
 * @return 0 on success, negative error code on failure
 */
int config_set_mode(enum recording_mode mode);

/**
 * @brief Set noise suppression level
 *
 * @param noise Noise suppression in dB
 * @return 0 on success, negative error code on failure
 */
int config_set_noise_suppress(uint8_t noise);

/**
 * @brief Set auto-delete days
 *
 * @param days Days (0-30) or -1 for off
 * @return 0 on success, negative error code on failure
 */
int config_set_auto_delete_days(int8_t days);

/**
 * @brief Set dereverb enabled
 *
 * @param enabled true to enable, false to disable
 * @return 0 on success, negative error code on failure
 */
int config_set_dereverb_enabled(bool enabled);

/**
 * @brief Set OLED brightness
 *
 * @param brightness Brightness (0-255)
 * @return 0 on success, negative error code on failure
 */
int config_set_oled_brightness(uint8_t brightness);

/**
 * @brief Generate a random WiFi AP password and save to config
 *
 * Generates 8-char alphanumeric password (excluding 0/O/l/1/I).
 *
 * @return 0 on success, negative error code on failure
 */
int config_generate_wifi_password(void);

/**
 * @brief Set WiFi AP password
 *
 * @param password Password string (up to 8 chars)
 * @return 0 on success, negative error code on failure
 */
int config_set_wifi_password(const char *password);

/**
 * @brief Get WiFi AP password
 *
 * @return Password string, or "12345678" if not set
 */
const char *config_get_wifi_password(void);

/**
 * @brief Set device name
 *
 * @param name Device name string (1-32 chars, printable UTF-8)
 * @return 0 on success, negative error code on failure
 */
int config_set_device_name(const char *name);

/**
 * @brief Get device name
 *
 * @return Device name string, or empty string if not set
 */
const char *config_get_device_name(void);

/**
 * @brief Set WiFi AP channel
 *
 * @param channel Channel number (5GHz: 36-165)
 * @return 0 on success, negative error code on failure
 */
int config_set_wifi_channel(uint8_t channel);

/**
 * @brief Get WiFi AP channel
 *
 * @return Channel number
 */
uint8_t config_get_wifi_channel(void);

/**
 * @brief Set WiFi regulatory domain
 *
 * @param reg_domain 2-letter country code
 * @return 0 on success, negative error code on failure
 */
int config_set_wifi_reg_domain(const char *reg_domain);

/**
 * @brief Get WiFi regulatory domain
 *
 * @return 2-letter country code string
 */
const char *config_get_wifi_reg_domain(void);

/**
 * @brief Store the credentials of the network to join in station mode
 *
 * Persisted to LittleFS, so they survive a reboot and an OTA.
 *
 * @param ssid Network name, 1-32 chars. Required.
 * @param psk  WPA2 passphrase (8-63 chars) or 64-char hex PSK. NULL or empty
 *             for an open network.
 * @return 0 on success, negative error code on failure
 */
int config_set_sta_credentials(const char *ssid, const char *psk);

/**
 * @brief Get the configured station-mode SSID
 *
 * @return SSID string, empty if never configured
 */
const char *config_get_sta_ssid(void);

/**
 * @brief Get the configured station-mode passphrase
 *
 * @return Passphrase string, empty for an open network
 */
const char *config_get_sta_psk(void);

/**
 * @brief Whether station-mode credentials have been configured
 *
 * @return true if an SSID is stored
 */
bool config_has_sta_credentials(void);

/**
 * @brief Store the address of the upload service
 *
 * Where recordings are pushed once the device is on the network. Persisted to
 * LittleFS.
 *
 * @param host Service IPv4, dotted quad
 * @param port Service UDP port
 * @return 0 on success, -EINVAL if host or port is not usable
 */
int config_set_upload_endpoint(const char *host, uint16_t port);

/**
 * @brief Store the AES-128 audio-at-rest key (Doc 09)
 *
 * Rejects the all-zero key (it is the "not provisioned" marker). Persisted
 * in LittleFS settings; never readable back over any AT command.
 *
 * @param key 16-byte AES-128 key
 * @return 0 on success, -EINVAL for the all-zero key
 */
int config_set_audio_key(const uint8_t key[16]);

/**
 * @brief Whether an audio-at-rest key has been provisioned
 */
bool config_has_audio_key(void);

/**
 * @brief Get the upload service address
 *
 * @return Dotted-quad string, empty if never configured
 */
const char *config_get_upload_host(void);

/**
 * @brief Destruye los secretos del device sin tocar el resto de la config.
 *
 * Borra la clave de audio, las credenciales WiFi y el endpoint. El audio ya
 * grabado en la tarjeta queda como ciphertext sin llave: el borrado es
 * instantaneo e irreversible aunque la tarjeta este llena, porque no borra
 * ficheros — tira la llave.
 *
 * Se invoca al entrar en recuperacion (CLIP_SECURITY_WIPE_ON_DFU) y desde
 * AT+WIPE.
 *
 * @return 0, o el primer error de guardado.
 */
int config_wipe_secrets(void);

/**
 * @brief Get the upload service port
 *
 * @return Port number, 0 if never configured
 */
uint16_t config_get_upload_port(void);

/**
 * @brief Arm/disarm the settings_load watchdog
 *
 * settings_load can block for ~40s on a corrupt settings file and cannot
 * be interrupted. Arm a watchdog (on the system workqueue thread, which
 * runs independently of the main thread where settings_load executes)
 * before a settings_load_subtree() call; if it hasn't returned within
 * SETTINGS_LOAD_TIMEOUT_MS, the watchdog wipes the file and reboots.
 */
void settings_load_watchdog_arm(void);
void settings_load_watchdog_disarm(void);

#endif /* CLIP_CONFIG_H */
