#include "config.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "ble_address.h"

static const char *TAG = "rfs.cfg";

#define NVS_NS "rfsentinel"

#define CFG_DEFAULT_USER "admin"
#define CFG_DEFAULT_PASS "admin"

/* NVS keys (15 chars max) */
#define K_WIFI_SSID  "wifi_ssid"
#define K_WIFI_PASS  "wifi_pass"
#define K_HOSTNAME   "hostname"
#define K_WEB_USER   "web_user"
#define K_WEB_PASS   "web_pass"
#define K_WIGLE_USER "wigle_user"
#define K_WIGLE_TOK  "wigle_tok"
#define K_AP_SSID    "ap_ssid"
#define K_AP_PASS    "ap_pass"
#define K_LAT        "lat"
#define K_LON        "lon"
#define K_MIN_RSSI   "min_rssi"
#define K_MQTT_RSSI  "mq_rssi"
#define K_RDISC      "rdisc"
#define K_SCAN_WIFI  "scan_wifi"
#define K_SCAN_BLE   "scan_ble"
#define K_BLE_FILTER "ble_filter"
#define K_SCAN_AUTO  "scan_auto"
#define K_UI_LANG    "ui_lang"
#define K_MQ_EN      "mq_en"
#define K_MQ_BROKER  "mq_broker"
#define K_MQ_PORT    "mq_port"
#define K_MQ_USER    "mq_user"
#define K_MQ_PASS    "mq_pass"
#define K_MQ_TOPIC   "mq_topic"
#define K_MQ_ID      "mq_id"
#define K_MQ_TLS     "mq_tls"
#define K_NTP        "ntp"
#define K_TZ         "tz"
#define K_DEVID      "devid"
#define K_TIME_REF   "time_ref"

static app_config_t s_cfg;
static bool s_cfg_loaded = false;

/* Copie borndee + nettoyage des caracteres de controle.
 * Les valeurs saisies ne sont jamais logpees. */
static void copy_str(char *dst, size_t n, const char *src) {
    size_t i = 0;
    if (src) {
        for (; i + 1 < n && src[i]; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[i] = (c < 0x20 || c == 0x7f) ? ' ' : src[i];
        }
    }
    dst[i] = '\0';
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t n, const char *def) {
    size_t sz = n;
    esp_err_t err = nvs_get_str(h, key, dst, &sz);
    if (err == ESP_OK) {
        /* nettoyage des caracteres de controle */
        size_t len = strnlen(dst, n);
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)dst[i];
            if (c < 0x20 || c == 0x7f) dst[i] = ' ';
        }
        dst[n - 1] = '\0';
    } else {
        strlcpy(dst, def, n);
    }
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst, uint8_t def) {
    esp_err_t err = nvs_get_u8(h, key, dst);
    if (err != ESP_OK) *dst = def;
}

static void load_i8(nvs_handle_t h, const char *key, int8_t *dst, int8_t def) {
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *dst = (int8_t)v;
    } else {
        *dst = def;
    }
}

static void load_u16(nvs_handle_t h, const char *key, uint16_t *dst, uint16_t def) {
    esp_err_t err = nvs_get_u16(h, key, dst);
    if (err != ESP_OK) *dst = def;
}

static void load_f32(nvs_handle_t h, const char *key, float *dst, float def) {
    int32_t v = 0;
    esp_err_t err = nvs_get_i32(h, key, &v);
    *dst = (err == ESP_OK) ? (float)v / 1000000.0f : def;
}

static void load_u64(nvs_handle_t h, const char *key, uint64_t *dst, uint64_t def) {
    esp_err_t err = nvs_get_u64(h, key, dst);
    if (err != ESP_OK) *dst = def;
}

static void save_str(nvs_handle_t h, const char *key, const char *val) {
    nvs_set_str(h, key, val);
}

static void save_u8(nvs_handle_t h, const char *key, uint8_t val) {
    nvs_set_u8(h, key, val);
}

static void save_i8(nvs_handle_t h, const char *key, int8_t val) {
    nvs_set_u8(h, key, (uint8_t)val);
}

static void save_u16(nvs_handle_t h, const char *key, uint16_t val) {
    nvs_set_u16(h, key, val);
}

static void save_f32(nvs_handle_t h, const char *key, float val) {
    nvs_set_i32(h, key, (int32_t)(val * 1000000.0f));
}

static void save_u64(nvs_handle_t h, const char *key, uint64_t val) {
    nvs_set_u64(h, key, val);
}

void config_defaults(app_config_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->wifi_ssid, "", CFG_SSID_MAX);
    strlcpy(c->wifi_pass, "", CFG_PASS_MAX);
    strlcpy(c->hostname, CFG_HOSTNAME_DEFAULT, sizeof(c->hostname));
    strlcpy(c->web_user, CFG_DEFAULT_USER, CFG_USER_MAX);
    strlcpy(c->web_pass, CFG_DEFAULT_PASS, CFG_PASS_MAX);
    strlcpy(c->wigle_user, "", CFG_WIGLE_USER_MAX);
    strlcpy(c->wigle_token, "", CFG_TOKEN_MAX);
    strlcpy(c->ap_ssid, CFG_AP_SSID, CFG_SSID_MAX);
    strlcpy(c->ap_password, CFG_AP_PASS, CFG_PASS_MAX);
    c->lat = 0.0f;
    c->lon = 0.0f;
    c->min_rssi = -95;
    c->mqtt_min_rssi = -60;
    c->rediscover_min = 60;
    c->scan_wifi = 1;
    c->scan_ble = 1;
    c->ble_address_filter = RFS_BLE_FILTER_STABLE;
    c->scan_autostart = 1;
    c->ui_lang = 0;
    c->mqtt_enabled = 0;
    strlcpy(c->mqtt_broker, "", CFG_BROKER_MAX);
    c->mqtt_port = 1883;
    strlcpy(c->mqtt_user, "", CFG_MQTT_USER_MAX);
    strlcpy(c->mqtt_pass, "", CFG_MQTT_PASS_MAX);
    strlcpy(c->mqtt_topic, "/rfsentinel/alert", CFG_MQTT_TOPIC_MAX);
    strlcpy(c->mqtt_client_id, "rfsentinel", CFG_MQTT_ID_MAX);
    c->mqtt_tls = 0;
    strlcpy(c->ntp_server, "pool.ntp.org", CFG_NTP_MAX);
    strlcpy(c->tz, "CET-1CEST,M3.5.0,M10.5.0/3", CFG_TZ_MAX);
    strlcpy(c->device_id, "rfsentinel-01", CFG_DEVID_MAX);
}

esp_err_t config_init(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    load_str(h, K_WIFI_SSID, s_cfg.wifi_ssid, CFG_SSID_MAX, "");
    load_str(h, K_WIFI_PASS, s_cfg.wifi_pass, CFG_PASS_MAX, "");
    load_str(h, K_HOSTNAME, s_cfg.hostname, sizeof(s_cfg.hostname), CFG_HOSTNAME_DEFAULT);
    if (!config_hostname_valid(s_cfg.hostname)) strlcpy(s_cfg.hostname, CFG_HOSTNAME_DEFAULT, sizeof(s_cfg.hostname));
    load_str(h, K_WEB_USER, s_cfg.web_user, CFG_USER_MAX, CFG_DEFAULT_USER);
    load_str(h, K_WEB_PASS, s_cfg.web_pass, CFG_PASS_MAX, CFG_DEFAULT_PASS);
    load_str(h, K_WIGLE_USER, s_cfg.wigle_user, CFG_WIGLE_USER_MAX, "");
    load_str(h, K_WIGLE_TOK, s_cfg.wigle_token, CFG_TOKEN_MAX, "");
    load_str(h, K_AP_SSID, s_cfg.ap_ssid, CFG_SSID_MAX, CFG_AP_SSID);
    load_str(h, K_AP_PASS, s_cfg.ap_password, CFG_PASS_MAX, CFG_AP_PASS);
    /* One-time migration requested for the installed device; later user changes persist. */
    uint8_t ap_password_migrated = 0;
    if (nvs_get_u8(h, "ap_pass_v2", &ap_password_migrated) != ESP_OK || !ap_password_migrated) {
        err = nvs_set_str(h, K_AP_PASS, CFG_AP_PASS);
        if (err == ESP_OK) err = nvs_set_u8(h, "ap_pass_v2", 1);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK) { nvs_close(h); return err; }
        strlcpy(s_cfg.ap_password, CFG_AP_PASS, sizeof(s_cfg.ap_password));
    }

    {
        int32_t v = 0;
        esp_err_t e = nvs_get_i32(h, K_LAT, &v);
        s_cfg.lat = (e == ESP_OK) ? (float)v / 1000000.0f : 0.0f;
        e = nvs_get_i32(h, K_LON, &v);
        s_cfg.lon = (e == ESP_OK) ? (float)v / 1000000.0f : 0.0f;
    }
    if (s_cfg.lat < -90.0f || s_cfg.lat > 90.0f) s_cfg.lat = 0.0f;
    if (s_cfg.lon < -180.0f || s_cfg.lon > 180.0f) s_cfg.lon = 0.0f;

    load_i8(h, K_MIN_RSSI, &s_cfg.min_rssi, -95);
    load_i8(h, K_MQTT_RSSI, &s_cfg.mqtt_min_rssi, -60);
    load_u16(h, K_RDISC, &s_cfg.rediscover_min, 60);
    load_u8(h, K_SCAN_WIFI, &s_cfg.scan_wifi, 1);
    load_u8(h, K_SCAN_BLE, &s_cfg.scan_ble, 1);
    load_u8(h, K_BLE_FILTER, &s_cfg.ble_address_filter, RFS_BLE_FILTER_STABLE);
    if (s_cfg.ble_address_filter > RFS_BLE_FILTER_PUBLIC) s_cfg.ble_address_filter = RFS_BLE_FILTER_STABLE;
    load_u8(h, K_SCAN_AUTO, &s_cfg.scan_autostart, 1);
    load_u8(h, K_UI_LANG, &s_cfg.ui_lang, 0);

    load_u8(h, K_MQ_EN, &s_cfg.mqtt_enabled, 0);
    load_str(h, K_MQ_BROKER, s_cfg.mqtt_broker, CFG_BROKER_MAX, "");
    load_u16(h, K_MQ_PORT, &s_cfg.mqtt_port, 1883);
    load_str(h, K_MQ_USER, s_cfg.mqtt_user, CFG_MQTT_USER_MAX, "");
    load_str(h, K_MQ_PASS, s_cfg.mqtt_pass, CFG_MQTT_PASS_MAX, "");
    load_str(h, K_MQ_TOPIC, s_cfg.mqtt_topic, CFG_MQTT_TOPIC_MAX, "/rfsentinel/alert");
    load_str(h, K_MQ_ID, s_cfg.mqtt_client_id, CFG_MQTT_ID_MAX, "rfsentinel");
    load_u8(h, K_MQ_TLS, &s_cfg.mqtt_tls, 0);

    load_str(h, K_NTP, s_cfg.ntp_server, CFG_NTP_MAX, "pool.ntp.org");
    load_str(h, K_TZ, s_cfg.tz, CFG_TZ_MAX, "CET-1CEST,M3.5.0,M10.5.0/3");
    load_str(h, K_DEVID, s_cfg.device_id, CFG_DEVID_MAX, "rfsentinel-01");

    s_cfg_loaded = true;
    nvs_close(h);
    return ESP_OK;
}

const app_config_t *config_get(void) {
    if (!s_cfg_loaded) {
        config_defaults(&s_cfg);
        s_cfg_loaded = true;
    }
    return &s_cfg;
}

bool config_has_password(void) { return s_cfg.wifi_pass[0] != '\0'; }
bool config_has_token(void) { return s_cfg.wigle_token[0] != '\0'; }
bool config_has_ap_password(void) { return s_cfg.ap_password[0] != '\0'; }

bool config_check_credentials(const char *user, const char *pass) {
    if (!user || !pass) return false;
    /* premiere configuration : admin / admin accepte, changement exige */
    if (s_cfg.web_user[0] == '\0' && s_cfg.web_pass[0] == '\0') {
        return strcmp(user, CFG_DEFAULT_USER) == 0 &&
               strcmp(pass, CFG_DEFAULT_PASS) == 0;
    }
    return strcmp(user, s_cfg.web_user) == 0 &&
           strcmp(pass, s_cfg.web_pass) == 0;
}

esp_err_t config_set_web_auth(const char *user, const char *pass) {
    if (!user || !user[0] || !pass || strlen(pass) < 8) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    copy_str(s_cfg.web_user, CFG_USER_MAX, user);
    copy_str(s_cfg.web_pass, CFG_PASS_MAX, pass);
    save_str(h, K_WEB_USER, s_cfg.web_user);
    save_str(h, K_WEB_PASS, s_cfg.web_pass);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

bool config_needs_password_change(void) {
    return strcmp(s_cfg.web_user, CFG_DEFAULT_USER) == 0 &&
           strcmp(s_cfg.web_pass, CFG_DEFAULT_PASS) == 0;
}

bool config_hostname_valid(const char *hostname) {
    if (!hostname) return false;
    size_t len = strlen(hostname);
    if (!len || len >= CFG_HOSTNAME_MAX || hostname[0] == '-' || hostname[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        char c = hostname[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

esp_err_t config_set_hostname(const char *hostname) {
    if (!config_hostname_valid(hostname)) return ESP_ERR_INVALID_ARG;
    if (strcmp(s_cfg.hostname, hostname) == 0) return ESP_OK;
    nvs_handle_t h; esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_HOSTNAME, hostname);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) strlcpy(s_cfg.hostname, hostname, sizeof(s_cfg.hostname));
    return err;
}

esp_err_t config_set_wifi(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (ssid && ssid[0]) {
        copy_str(s_cfg.wifi_ssid, CFG_SSID_MAX, ssid);
        save_str(h, K_WIFI_SSID, s_cfg.wifi_ssid);
    }
    if (pass && pass[0]) {
        copy_str(s_cfg.wifi_pass, CFG_PASS_MAX, pass);
        save_str(h, K_WIFI_PASS, s_cfg.wifi_pass);
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_set_ap(const char *ssid, const char *pass) {
    if (ssid && ssid[0] && strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    if (pass && pass[0] && (strlen(pass) < 8 || strlen(pass) > 63)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (ssid && ssid[0]) err = nvs_set_str(h, K_AP_SSID, ssid);
    if (err == ESP_OK && pass && pass[0]) err = nvs_set_str(h, K_AP_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    if (ssid && ssid[0]) copy_str(s_cfg.ap_ssid, CFG_SSID_MAX, ssid);
    if (pass && pass[0]) copy_str(s_cfg.ap_password, CFG_PASS_MAX, pass);
    return ESP_OK;
}

esp_err_t config_set_credentials(const char *user, const char *pass) {
    if ((user && strlen(user) >= CFG_WIGLE_USER_MAX) ||
        (pass && strlen(pass) >= CFG_TOKEN_MAX)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (user && user[0]) err = nvs_set_str(h, K_WIGLE_USER, user);
    if (err == ESP_OK && pass && pass[0]) err = nvs_set_str(h, K_WIGLE_TOK, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        if (user && user[0]) copy_str(s_cfg.wigle_user, CFG_WIGLE_USER_MAX, user);
        if (pass && pass[0]) copy_str(s_cfg.wigle_token, CFG_TOKEN_MAX, pass);
    }
    return err;
}

esp_err_t config_set_token(const char *token) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (token && token[0]) {
        copy_str(s_cfg.wigle_token, CFG_TOKEN_MAX, token);
        save_str(h, K_WIGLE_TOK, s_cfg.wigle_token);
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_set_location(float lat, float lon) {
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    s_cfg.lat = lat;
    s_cfg.lon = lon;
    save_f32(h, K_LAT, s_cfg.lat);
    save_f32(h, K_LON, s_cfg.lon);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_set_scan_autostart(uint8_t enabled) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint8_t value = enabled ? 1 : 0;
    err = nvs_set_u8(h, K_SCAN_AUTO, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_cfg.scan_autostart = value;
    return err;
}

esp_err_t config_set_ble_address_filter(uint8_t mode) {
    if (mode > RFS_BLE_FILTER_PUBLIC) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_BLE_FILTER, mode);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_cfg.ble_address_filter = mode;
    return err;
}

esp_err_t config_set_options(int8_t min_rssi, uint8_t scan_wifi,
                             uint8_t scan_ble, uint8_t lang) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    s_cfg.min_rssi = min_rssi;
    s_cfg.scan_wifi = scan_wifi ? 1 : 0;
    s_cfg.scan_ble = scan_ble ? 1 : 0;
    s_cfg.ui_lang = (lang == 1) ? 1 : 0;
    save_i8(h, K_MIN_RSSI, s_cfg.min_rssi);
    save_u8(h, K_SCAN_WIFI, s_cfg.scan_wifi);
    save_u8(h, K_SCAN_BLE, s_cfg.scan_ble);
    save_u8(h, K_UI_LANG, s_cfg.ui_lang);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_set_options2(int8_t min_rssi, int8_t mqtt_min_rssi,
                              uint16_t rediscover_min, uint8_t scan_wifi,
                              uint8_t scan_ble, uint8_t lang) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    s_cfg.min_rssi = min_rssi;
    s_cfg.mqtt_min_rssi = mqtt_min_rssi;
    s_cfg.rediscover_min = rediscover_min;
    s_cfg.scan_wifi = scan_wifi ? 1 : 0;
    s_cfg.scan_ble = scan_ble ? 1 : 0;
    s_cfg.ui_lang = (lang == 1) ? 1 : 0;
    save_i8(h, K_MIN_RSSI, s_cfg.min_rssi);
    save_i8(h, K_MQTT_RSSI, s_cfg.mqtt_min_rssi);
    save_u16(h, K_RDISC, s_cfg.rediscover_min);
    save_u8(h, K_SCAN_WIFI, s_cfg.scan_wifi);
    save_u8(h, K_SCAN_BLE, s_cfg.scan_ble);
    save_u8(h, K_UI_LANG, s_cfg.ui_lang);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_set_rssi_threshold(bool mqtt, int8_t threshold) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i8(h, mqtt ? K_MQTT_RSSI : K_MIN_RSSI, threshold);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        if (mqtt) s_cfg.mqtt_min_rssi = threshold;
        else s_cfg.min_rssi = threshold;
    }
    return err;
}

esp_err_t config_set_mqtt(uint8_t enabled, const char *broker, uint16_t port,
                          const char *user, const char *pass,
                          const char *topic, const char *client_id, uint8_t tls) {
    if ((broker && strlen(broker) >= CFG_BROKER_MAX) ||
        (user && strlen(user) >= CFG_MQTT_USER_MAX) ||
        (pass && strlen(pass) >= CFG_MQTT_PASS_MAX) ||
        (topic && strlen(topic) >= CFG_MQTT_TOPIC_MAX) ||
        (client_id && strlen(client_id) >= CFG_MQTT_ID_MAX) || !port)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_MQ_EN, enabled ? 1 : 0);
    if (err == ESP_OK && broker && broker[0]) err = nvs_set_str(h, K_MQ_BROKER, broker);
    if (err == ESP_OK) err = nvs_set_u16(h, K_MQ_PORT, port);
    if (err == ESP_OK && user && user[0]) err = nvs_set_str(h, K_MQ_USER, user);
    if (err == ESP_OK && pass && pass[0]) err = nvs_set_str(h, K_MQ_PASS, pass);
    if (err == ESP_OK && topic && topic[0]) err = nvs_set_str(h, K_MQ_TOPIC, topic);
    if (err == ESP_OK && client_id && client_id[0]) err = nvs_set_str(h, K_MQ_ID, client_id);
    if (err == ESP_OK) err = nvs_set_u8(h, K_MQ_TLS, tls ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    s_cfg.mqtt_enabled = enabled ? 1 : 0;
    if (broker && broker[0]) copy_str(s_cfg.mqtt_broker, CFG_BROKER_MAX, broker);
    s_cfg.mqtt_port = port;
    if (user && user[0]) copy_str(s_cfg.mqtt_user, CFG_MQTT_USER_MAX, user);
    if (pass && pass[0]) copy_str(s_cfg.mqtt_pass, CFG_MQTT_PASS_MAX, pass);
    if (topic && topic[0]) copy_str(s_cfg.mqtt_topic, CFG_MQTT_TOPIC_MAX, topic);
    if (client_id && client_id[0]) copy_str(s_cfg.mqtt_client_id, CFG_MQTT_ID_MAX, client_id);
    s_cfg.mqtt_tls = tls ? 1 : 0;
    return ESP_OK;
}

esp_err_t config_set_time(const char *ntp_server, const char *tz) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (ntp_server && ntp_server[0]) {
        copy_str(s_cfg.ntp_server, CFG_NTP_MAX, ntp_server);
        save_str(h, K_NTP, s_cfg.ntp_server);
    }
    if (tz && tz[0]) {
        copy_str(s_cfg.tz, CFG_TZ_MAX, tz);
        save_str(h, K_TZ, s_cfg.tz);
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_set_device_id(const char *device_id) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (device_id && device_id[0]) {
        copy_str(s_cfg.device_id, CFG_DEVID_MAX, device_id);
        save_str(h, K_DEVID, s_cfg.device_id);
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_save_time_ref(time_t ts) {
    if (ts < 1000000000LL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u64(h, K_TIME_REF, (uint64_t)ts);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

time_t config_load_time_ref(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return 0;
    uint64_t v = 0;
    esp_err_t e = nvs_get_u64(h, K_TIME_REF, &v);
    nvs_close(h);
    if (e != ESP_OK || v < 1000000000ULL) return 0;
    return (time_t)v;
}
