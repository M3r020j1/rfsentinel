#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/* RFSentinel — configuration persistante (NVS)
 *
 * Le projet ne depend plus d'aucune carte SD : configuration en NVS,
 * journaux de scan en LittleFS sur la partition interne `storage`.
 */

#define CFG_SSID_MAX  33
#define CFG_PASS_MAX  65
#define CFG_USER_MAX  32
#define CFG_WIGLE_USER_MAX 129
#define CFG_TOKEN_MAX 129
#define CFG_AP_SSID   "RFSentinel-Setup"
#define CFG_AP_PASS   "012345678"
#define CFG_BROKER_MAX 64
#define CFG_MQTT_USER_MAX 32
#define CFG_MQTT_PASS_MAX 65
#define CFG_MQTT_TOPIC_MAX 64
#define CFG_MQTT_ID_MAX 32
#define CFG_NTP_MAX   32
#define CFG_TZ_MAX    32
#define CFG_DEVID_MAX 24

typedef struct {
    /* Wi-Fi client (STA) */
    char wifi_ssid[CFG_SSID_MAX];
    char wifi_pass[CFG_PASS_MAX];

    /* Identifiants administrateur WebUI */
    char web_user[CFG_USER_MAX];
    char web_pass[CFG_PASS_MAX];

    /* WiGLE (upload manuel) */
    char wigle_user[CFG_WIGLE_USER_MAX];
    char wigle_token[CFG_TOKEN_MAX];

    /* Access point de secours */
    char ap_ssid[CFG_SSID_MAX];
    char ap_password[CFG_PASS_MAX];

    /* Localisation configuree (pas de GPS) */
    float lat;
    float lon;

    /* Seuils RSSI independants (dBm, valeurs negatives) */
    int8_t min_rssi;       /* enregistrement des observations */
    int8_t mqtt_min_rssi;  /* declenchement des alertes MQTT */

    /* Duree d'absence (minutes) avant qu'un equipement
     * soit de nouveau considere comme "nouvellement present" */
    uint16_t rediscover_min;

    /* Options de scan */
    uint8_t scan_wifi;
    uint8_t scan_ble;
    uint8_t ble_address_filter;
    uint8_t scan_autostart;

    /* Langue UI : 0 = FR, 1 = EN */
    uint8_t ui_lang;

    /* MQTT */
    uint8_t mqtt_enabled;
    char mqtt_broker[CFG_BROKER_MAX];
    uint16_t mqtt_port;
    char mqtt_user[CFG_MQTT_USER_MAX];
    char mqtt_pass[CFG_MQTT_PASS_MAX];
    char mqtt_topic[CFG_MQTT_TOPIC_MAX];
    char mqtt_client_id[CFG_MQTT_ID_MAX];
    uint8_t mqtt_tls;

    /* NTP / fuseau */
    char ntp_server[CFG_NTP_MAX];
    char tz[CFG_TZ_MAX];

    /* Identifiant unique RFSentinel */
    char device_id[CFG_DEVID_MAX];
} app_config_t;

/* Initialise la config (NVS) + valeurs par defaut si absente. */
esp_err_t config_init(void);

/* Retourne la config en RAM (partagee, en lecture). */
const app_config_t *config_get(void);

/* Marqueurs de presence de secrets (sans les exposer). */
bool config_has_password(void);
bool config_has_token(void);
bool config_has_ap_password(void);

/* Force la saisie du mot de passe si identifiants par defaut (admin/admin). */
bool config_needs_password_change(void);

/** Verifie les identifiants de connexion WebUI. Retourne true si valides. */
bool config_check_credentials(const char *user, const char *pass);

/* Mutateurs valides (validation interne, ecritures NVS limitees). */
esp_err_t config_set_wifi(const char *ssid, const char *pass);
esp_err_t config_set_ap(const char *ssid, const char *pass);
esp_err_t config_set_scan_autostart(uint8_t enabled);
esp_err_t config_set_ble_address_filter(uint8_t mode);
esp_err_t config_set_credentials(const char *user, const char *pass);
esp_err_t config_set_token(const char *token);
esp_err_t config_set_location(float lat, float lon);
esp_err_t config_set_options(int8_t min_rssi, uint8_t scan_wifi,
                             uint8_t scan_ble, uint8_t lang);
esp_err_t config_set_options2(int8_t min_rssi, int8_t mqtt_min_rssi,
                              uint16_t rediscover_min, uint8_t scan_wifi,
                              uint8_t scan_ble, uint8_t lang);
esp_err_t config_set_rssi_threshold(bool mqtt, int8_t threshold);
esp_err_t config_set_web_auth(const char *user, const char *pass);
esp_err_t config_set_mqtt(uint8_t enabled, const char *broker, uint16_t port,
                          const char *user, const char *pass,
                          const char *topic, const char *client_id,
                          uint8_t tls);
esp_err_t config_set_time(const char *ntp_server, const char *tz);
esp_err_t config_set_device_id(const char *device_id);

/* Enregistre la derniere reference temporelle valide (anti-usure :
 * au plus une ecrite par periode). */
esp_err_t config_save_time_ref(time_t ts);

/* Lit la derniere reference temporelle valide (0 si aucune). */
time_t config_load_time_ref(void);
