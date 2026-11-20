#include "net.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "mdns.h"

static esp_netif_t *s_netif = NULL;
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "config.h"

static const char *TAG = "rfs.net";

#define NVS_NS "rfsentinel"
#define K_TIMETS "timets"

#define STA_CONNECT_TIMEOUT_MS 8000
#define STA_MAX_RETRY 4
#define NTP_RESYNC_INTERVAL_S (6 * 3600)
#define TIME_REF_SAVE_INTERVAL_S 3600
#define FALLBACK_EPOCH 1735689600ULL /* 2025-01-01 00:00:00 UTC */
#define TZ_DEFAULT "CET-1CEST,M3.5.0,M10.5.0/3"

static bool s_wifi_connected = false;
static bool s_sntp_synced = false;
static bool s_has_ref = false;
static time_t s_ref_utc = 0;
static int64_t s_ref_uptime_us = 0;
static bool s_ap_active = false;
static bool s_sta_enabled = false;
static int s_sta_retry = 0;
static TickType_t s_next_retry_at = 0;
static char s_ntp_server[CFG_NTP_MAX] = "pool.ntp.org";
static char s_tz[CFG_TZ_MAX] = TZ_DEFAULT;
static time_t s_last_sync_ts = 0;
static int64_t s_last_ref_save_us = 0;
static bool s_initialized = false;
static bool s_sntp_ready = false;

esp_err_t net_apply_ap_config(void) {
    const app_config_t *cfg = config_get();
    wifi_config_t ap = {0};
    const char *ssid = cfg->ap_ssid[0] ? cfg->ap_ssid : CFG_AP_SSID;
    size_t ssid_len = strnlen(ssid, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, ssid, ssid_len);
    ap.ap.ssid_len = ssid_len;
    strlcpy((char *)ap.ap.password, cfg->ap_password, sizeof(ap.ap.password));
    ap.ap.channel = 1;
    ap.ap.authmode = strlen(cfg->ap_password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.max_connection = 3;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) ESP_LOGE(TAG, "configuration AP: %s", esp_err_to_name(err));
    return err;
}

static void tz_apply(void) {
    /* Un TZ vide ferait comporter localtime_r comme UTC : on garde la reference. */
    setenv("TZ", s_tz[0] ? s_tz : TZ_DEFAULT, 1);
    tzset();
}

static time_t current_utc(void);
static void log_time_state(const char *why);

static void save_time_ref(time_t ts) {
    int64_t now_us = esp_timer_get_time();
    if (s_last_ref_save_us != 0 &&
        (now_us - s_last_ref_save_us) < (int64_t)TIME_REF_SAVE_INTERVAL_S * 1000000) {
        return;
    }
    esp_err_t err = config_save_time_ref(ts);
    if (err == ESP_OK) s_last_ref_save_us = now_us;
    else ESP_LOGW(TAG, "reference temporelle non sauvegardee: %s", esp_err_to_name(err));
}

static void sntp_synced_cb(struct timeval *tv) {
    (void)tv;
    s_sntp_synced = true;
    time_t now = time(NULL);
    s_last_sync_ts = now;
    s_ref_utc = now;
    s_ref_uptime_us = esp_timer_get_time();
    save_time_ref(now);
    ESP_LOGI(TAG, "NTP: horloge synchronisee");
    log_time_state("NTP");
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        s_wifi_connected = true;
        s_sta_retry = 0;
        s_next_retry_at = 0;
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK) s_ap_active = (mode == WIFI_MODE_APSTA);
        ESP_LOGI(TAG, "STA connecte");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_sta_retry < STA_MAX_RETRY) {
            s_sta_retry++;
            s_next_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(2000 * s_sta_retry);
        } else {
            ESP_LOGW(TAG, "STA: echec connexion, bascule AP");
            if (!s_ap_active) {
                s_ap_active = true;
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                net_apply_ap_config();
            }
            s_next_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        }
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "AP: client connecte");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "AP: client deconnecte (raison=%u)", event ? event->reason : 0);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data) {
    (void)arg;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    if (evt) {
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    }
    if (s_sntp_ready) {
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) ESP_LOGW(TAG, "NTP demarrage: %s", esp_err_to_name(err));
        else ESP_LOGI(TAG, "NTP demarre apres obtention de l'IP");
    }
}

void net_init(void) {
    if (s_initialized) return;
    s_initialized = true;

    const app_config_t *cfg = config_get();
    strlcpy(s_ntp_server, cfg->ntp_server, CFG_NTP_MAX);
    strlcpy(s_tz, cfg->tz, CFG_TZ_MAX);
    s_sta_enabled = (cfg->wifi_ssid[0] != '\0');

    tz_apply();

    /* reference temporelle persistante */
    time_t ref = config_load_time_ref();
    if (ref > 0) {
        s_has_ref = true;
        s_ref_utc = ref;
        s_ref_uptime_us = esp_timer_get_time();
        struct timeval tv = { .tv_sec = ref, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) != 0) ESP_LOGW(TAG, "restauration horloge systeme impossible");
        ESP_LOGI(TAG, "heure restauree depuis reference: %ld", (long)ref);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_err_t hostname_err = esp_netif_set_hostname(s_netif, cfg->hostname);
    if (hostname_err != ESP_OK) ESP_LOGW(TAG, "hostname DHCP: %s", esp_err_to_name(hostname_err));

    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_err = mdns_hostname_set(cfg->hostname);
        if (mdns_err != ESP_OK) mdns_free();
    }
    if (mdns_err != ESP_OK) ESP_LOGW(TAG, "mDNS indisponible: %s", esp_err_to_name(mdns_err));
    else ESP_LOGI(TAG, "hostname: %s, mDNS: %s.local", cfg->hostname, cfg->hostname);

    wifi_init_config_t cfg_init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg_init);

    /* France: enable the locally permitted 2.4 GHz channels, including
       12 and 13. When associated, follow the upstream AP's 802.11d info. */
    ESP_ERROR_CHECK(esp_wifi_set_country_code("FR", true));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL);

    /* STA : connexion au reseau configure */
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    /* AP de secours : SSID/mot de passe configures */
    net_apply_ap_config();
    if (s_sta_enabled) esp_wifi_set_mode(WIFI_MODE_STA);

    /* Defer DNS/NTP until the station has an IP address. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    sntp_cfg.sync_cb = sntp_synced_cb;
    sntp_cfg.start = false;
    esp_err_t sntp_err = esp_netif_sntp_init(&sntp_cfg);
    s_sntp_ready = (sntp_err == ESP_OK);
    if (!s_sntp_ready) ESP_LOGW(TAG, "NTP init: %s", esp_err_to_name(sntp_err));

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_start();

    /* pas de SSID : AP de secours immediat. APSTA (et non AP seul) car
       esp_wifi_scan_start exige une interface STA ; la STA reste inactive
       (SSID vide, aucune connexion tentee). */
    if (cfg->wifi_ssid[0] == '\0') {
        s_ap_active = true;
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    ESP_LOGI(TAG, "net: init ok (mode=%s)", cfg->wifi_ssid[0] ? "STA+AP" : "AP");
}

bool net_is_connected(void) { return s_wifi_connected; }

const char *net_get_mode(void) {
    if (s_wifi_connected) return s_ap_active ? "STA+AP" : "STA";
    return "AP";
}

bool net_get_ip(char *out, size_t n) {
    if (!out || n == 0) return false;
    esp_netif_ip_info_t ip;
    if (s_wifi_connected &&
        esp_netif_get_ip_info(s_netif, &ip) == ESP_OK &&
        ip.ip.addr != 0) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
        return true;
    }
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip) == ESP_OK &&
        ip.ip.addr != 0) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
        return true;
    }
    strlcpy(out, NET_AP_DEFAULT_IP, n);
    return true;
}

bool net_time_synced(void) { return s_sntp_synced; }

esp_err_t net_set_time_from_client(time_t utc) {
    if (utc < 1704067200LL || utc > 4102444799LL) return ESP_ERR_INVALID_ARG;
    if (s_sntp_synced) return ESP_ERR_INVALID_STATE;
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;
    s_ref_utc = utc;
    s_ref_uptime_us = esp_timer_get_time();
    s_has_ref = true;
    s_last_sync_ts = utc;
    /* Explicit user corrections must replace an earlier saved reference,
       even if the automatic NTP write throttle is active. */
    esp_err_t err = config_save_time_ref(utc);
    if (err == ESP_OK) s_last_ref_save_us = s_ref_uptime_us;
    else ESP_LOGW(TAG, "heure reglee mais reference non sauvegardee: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "heure estimee reglee depuis le navigateur");
    log_time_state("navigateur");
    return ESP_OK;
}

ts_quality_t net_time_quality(void) {
    if (s_sntp_synced) return TS_NTP_SYNCED;
    if (s_has_ref) return TS_ESTIMATED;
    return TS_UNKNOWN;
}

const char *net_time_quality_str(void) {
    switch (net_time_quality()) {
        case TS_NTP_SYNCED: return "NTP_SYNCED";
        case TS_ESTIMATED:  return "ESTIMATED";
        default:            return "UNKNOWN";
    }
}

static time_t current_utc(void) {
    ts_quality_t q = net_time_quality();
    if (q == TS_NTP_SYNCED) return time(NULL);
    if (q == TS_ESTIMATED) {
        int64_t now_us = esp_timer_get_time();
        int64_t el = (now_us - s_ref_uptime_us) / 1000000;
        if (el < 0) el = 0;
        return s_ref_utc + el;
    }
    return (time_t)(FALLBACK_EPOCH + esp_timer_get_time() / 1000000);
}

/* Diagnostic de l'horloge : UTC brut, heure locale derivee, fuseau applique.
   Sert a controler sur la carte qu'aucune correction manuelle ne s'ajoute
   au fuseau. Une ligne par synchronisation, donc quasi nulle en usage normal. */
static void log_time_state(const char *why) {
    time_t now = current_utc();
    struct tm utc = {0}, local = {0};
    gmtime_r(&now, &utc);
    localtime_r(&now, &local);
    const char *tz = getenv("TZ");
    ESP_LOGI(TAG, "TIME %s : UTC %02d:%02d - LOCAL %02d:%02d - TZ %s",
             why, utc.tm_hour, utc.tm_min, local.tm_hour, local.tm_min,
             (tz && tz[0]) ? tz : "(vide)");
}

bool net_time_utc_with_quality(char *buf, size_t n, ts_quality_t *quality) {
    if (!buf || n < 20) return false;

    time_t now = current_utc();
    if (quality) *quality = net_time_quality();

    struct tm t = {0};
    if (gmtime_r(&now, &t) == NULL) return false;
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &t);
    return true;
}

bool net_time_utc(char *buf, size_t n) {
    return net_time_utc_with_quality(buf, n, NULL);
}

/* Heure locale pour les affichages lisibles. Le journal, le CSV WiGLE et les
   echanges MQTT restent en UTC : la conversion ne se fait qu'ici, une fois. */
static bool format_local(time_t now, char *buf, size_t n) {
    if (!buf || n < 20) return false;
    struct tm t = {0};
    if (localtime_r(&now, &t) == NULL) return false;
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &t);
    return true;
}

bool net_time_local(char *buf, size_t n) {
    return format_local(current_utc(), buf, n);
}

bool net_last_sync(char *buf, size_t n) {
    if (!buf || n < 20) return false;
    if (s_last_sync_ts == 0) { strlcpy(buf, "0", n); return true; }
    struct tm t = {0};
    time_t ts = s_last_sync_ts;
    gmtime_r(&ts, &t);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &t);
    return true;
}

bool net_last_sync_local(char *buf, size_t n) {
    if (!buf || n < 20) return false;
    if (s_last_sync_ts == 0) { strlcpy(buf, "0", n); return true; }
    return format_local(s_last_sync_ts, buf, n);
}

void net_apply_time_config(void) {
    const app_config_t *cfg = config_get();
    if (cfg->tz[0] && strcmp(cfg->tz, s_tz) != 0) {
        strlcpy(s_tz, cfg->tz, CFG_TZ_MAX);
        tz_apply();
        ESP_LOGI(TAG, "fuseau horaire mis a jour");
    }
    log_time_state("config");
}

void net_tick(void) {
    if (!s_initialized) return;

    /* reconnexion STA non bloquante */
    if (s_sta_enabled && !s_wifi_connected && s_next_retry_at != 0 &&
        xTaskGetTickCount() >= s_next_retry_at) {
        s_next_retry_at = 0;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA reconnexion: %s", esp_err_to_name(err));
            s_next_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        }
    }

    /* resync NTP periodique */
    {
        static int64_t last_resync_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (last_resync_us == 0) last_resync_us = now_us;
        if (s_sntp_synced &&
            (now_us - last_resync_us) >= (int64_t)NTP_RESYNC_INTERVAL_S * 1000000) {
            last_resync_us = now_us;
            if (s_sntp_ready && s_wifi_connected) esp_netif_sntp_start();
        }
    }
}
