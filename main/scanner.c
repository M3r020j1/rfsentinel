#include "scanner.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#include "config.h"
#include "net.h"
#include "store.h"
#include "mqtt.h"
#include "presence.h"

static const char *TAG = "rfs.scanner";

#define SCAN_WIFI_INTERVAL_S 4
#define SCAN_WIFI_MAX_APC 40
#define SCAN_BLE_INTERVAL_S 20
#define SCAN_BLE_DURATION_MS 10000
#define SCAN_BLE_MAX 12

static bool s_running = false;
static uint32_t s_wifi_count = 0;
static uint32_t s_ble_count = 0;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_exit_sem = NULL;
static bool s_ble_ready = false;
static bool s_ble_failed = false;
static uint64_t s_last_wifi_us = 0;
static uint64_t s_last_ble_us = 0;
static uint8_t s_scan_channel = 1;
static portMUX_TYPE s_wifi_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static scanner_wifi_diagnostics_t s_wifi_diag;
static portMUX_TYPE s_cal_lock = portMUX_INITIALIZER_UNLOCKED;
static scanner_calibration_t s_cal;
static scanner_find_result_t s_find;
static bool s_cal_radio_done;
#define CALIBRATION_TIMEOUT_MS (15 * 60 * 1000)

static uint8_t wifi_next_channel(uint8_t first, uint8_t count) {
    if (first < 1 || first > 14 || count == 0 || count > 15 - first) {
        first = 1;
        count = 11;
    }
    if (s_scan_channel < first || s_scan_channel >= first + count) s_scan_channel = first;
    return s_scan_channel++;
}

void scanner_get_wifi_diagnostics(scanner_wifi_diagnostics_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_wifi_diag_lock);
    *out = s_wifi_diag;
    portEXIT_CRITICAL(&s_wifi_diag_lock);
}

static void wifi_diag_record(uint8_t channel, uint8_t first, uint8_t count, esp_err_t err, uint32_t found, uint32_t saved) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_wifi_diag_lock);
    s_wifi_diag.attempts++;
    if (err != ESP_OK) s_wifi_diag.failures++;
    s_wifi_diag.channel = channel;
    s_wifi_diag.first_channel = first;
    s_wifi_diag.channel_count = count;
    s_wifi_diag.last_error = err;
    s_wifi_diag.found = found;
    s_wifi_diag.saved = saved;
    s_wifi_diag.completed_us = now;
    portEXIT_CRITICAL(&s_wifi_diag_lock);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "scan Wi-Fi: canal=%u detectes=%lu enregistres=%lu", (unsigned)channel, (unsigned long)found, (unsigned long)saved);
    else
        ESP_LOGW(TAG, "scan Wi-Fi: canal=%u erreur=%s", (unsigned)channel, esp_err_to_name(err));
}

static void hex2str(const uint8_t *b, char *s) {
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        s[i * 3] = hx[(b[i] >> 4) & 0x0F];
        s[i * 3 + 1] = hx[b[i] & 0x0F];
        s[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
}

static void fill_auth(const wifi_ap_record_t *r, char *out, size_t n) {
    switch (r->authmode) {
        case WIFI_AUTH_OPEN: strlcpy(out, "Open", n); break;
        case WIFI_AUTH_WEP: strlcpy(out, "WEP", n); break;
        case WIFI_AUTH_WPA_PSK: strlcpy(out, "WP2", n); break;
        case WIFI_AUTH_WPA2_PSK: strlcpy(out, "WPA2", n); break;
        case WIFI_AUTH_WPA_WPA2_PSK: strlcpy(out, "WPA2", n); break;
        case WIFI_AUTH_WPA3_PSK: strlcpy(out, "WPA3", n); break;
        case WIFI_AUTH_WPA2_WPA3_PSK: strlcpy(out, "WPA3", n); break;
        default: strlcpy(out, "Unknown", n); break;
    }
}

static void wifi_sanitize_ssid(const uint8_t *raw, size_t raw_len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < raw_len && o + 1 < n; i++) {
        unsigned char c = raw[i];
        if (c == '\0') break;
        if (c >= 0x20 && c != 0x7f) out[o++] = (char)c;
    }
    out[o] = '\0';
}

static void parse_adv_name(const uint8_t *data, size_t len, char *out, size_t n) {
    out[0] = '\0';
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t adlen = data[i];
        if (adlen == 0 || i + 1 + adlen > len) break;
        uint8_t adtype = data[i + 1];
        if (adtype == 0x08 || adtype == 0x09) {
            size_t nlen = adlen - 1;
            if (nlen > n - 1) nlen = n - 1;
            for (size_t k = 0; k < nlen; k++) {
                unsigned char c = data[i + 2 + k];
                out[k] = (c >= 0x20 && c != 0x7f) ? (char)c : '?';
            }
            out[nlen] = '\0';
            return;
        }
        i += 1 + adlen;
    }
}

/* --- Wi-Fi : scan passif, 2.4 GHz uniquement --- */

static void do_wifi_scan(void) {
    wifi_sta_list_t clients = {0};
    bool has_clients = esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0;
    wifi_scan_config_t scan = {0};
    scan.show_hidden = true;
    scan.scan_type = WIFI_SCAN_TYPE_PASSIVE;
    /* ESP-IDF requires its default passive dwell when Bluetooth is enabled.
       Zero requests the driver default instead of an unsupported override. */
    wifi_country_t country;
    uint8_t first = 1, channels = 11;
    if (esp_wifi_get_country(&country) == ESP_OK && country.nchan > 0 &&
        country.schan >= 1 && country.schan <= 14 && country.nchan <= 15 - country.schan) {
        first = country.schan;
        channels = country.nchan;
    }
    /* With an AP client, scan just one channel per cycle to limit the time
       away from the AP. Rotate through channels supported by the country. */
    if (has_clients) {
        scan.channel = wifi_next_channel(first, channels);
    }

    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        wifi_diag_record(scan.channel, first, channels, err, 0, 0);
        return;
    }

    uint16_t found = 0;
    esp_err_t num_err = esp_wifi_scan_get_ap_num(&found);
    uint16_t count = SCAN_WIFI_MAX_APC;
    static wifi_ap_record_t recs[SCAN_WIFI_MAX_APC];
    err = esp_wifi_scan_get_ap_records(&count, recs);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        wifi_diag_record(scan.channel, first, channels, err, 0, 0);
        return;
    }
    if (num_err != ESP_OK) found = count;

    const app_config_t *cfg = config_get();
    uint32_t saved = 0;
    for (uint16_t i = 0; i < count; i++) {
        s3_obs_t obs = {0};
        hex2str(recs[i].bssid, obs.mac);
        wifi_sanitize_ssid(recs[i].ssid, sizeof(recs[i].ssid), obs.ssid, OBS_SSID_MAX);
        fill_auth(&recs[i], obs.auth, OBS_AUTH_MAX);
        obs.rssi = (int8_t)recs[i].rssi;
        obs.channel = (int8_t)recs[i].primary;
        obs.freq = recs[i].primary == 14 ? 2484 : 2412 + (int32_t)(recs[i].primary - 1) * 5;
        obs.mfgr = 0;
        obs.is_ble = 0;
        ts_quality_t quality = TS_UNKNOWN;
        if (!net_time_utc_with_quality(obs.ts, sizeof(obs.ts), &quality)) obs.ts[0] = '\0';
        obs.time_quality = (uint8_t)quality;

        /* Presence is independent of exclusions and RSSI thresholds. */
        presence_on_obs(&obs);
        /* Alerting is independent of the storage threshold and rediscovery. */
        if (store_is_blacklisted(obs.mac)) continue;
        mqtt_on_obs(&obs);
        /* seuil RSSI : en dessous, on n'enregistre pas */
        if (obs.rssi < cfg->min_rssi) continue;

        bool appended = false;
        if (store_append_obs(&obs, &appended) == ESP_OK && appended) {
            s_wifi_count++;
            saved++;
        }
    }
    wifi_diag_record(scan.channel, first, channels, ESP_OK, found, saved);
}

/* --- BLE : découverte passive via advertisements --- */

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        portENTER_CRITICAL(&s_cal_lock);
        if (arg == &s_cal && s_cal.active) s_cal_radio_done = true;
        portEXIT_CRITICAL(&s_cal_lock);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const app_config_t *cfg = config_get();
    int8_t rssi = (int8_t)event->disc.rssi;

    s3_obs_t obs = {0};
    {
        uint8_t *a = event->disc.addr.val;
        char tmp[18];
        /* adresse BLE little-endian -> affichage MSB first */
        uint8_t rev[6];
        for (int i = 0; i < 6; i++) rev[i] = a[5 - i];
        hex2str(rev, tmp);
        strlcpy(obs.mac, tmp, sizeof(obs.mac));
    }
    portENTER_CRITICAL(&s_cal_lock);
    bool calibrating = s_cal.active;
    if (calibrating && arg == &s_cal && !s_cal.stop_requested && rssi != 127) {
        if (strcmp(s_cal.kind, "find") == 0) {
            uint32_t i = 0;
            while (i < s_find.count && strcmp(s_find.entries[i].mac, obs.mac)) i++;
            if (i == s_find.count && i < SCANNER_FIND_MAX) {
                strlcpy(s_find.entries[i].mac, obs.mac, sizeof(s_find.entries[i].mac));
                s_find.count++;
            }
            if (i < s_find.count) {
                scanner_find_entry_t *entry = &s_find.entries[i];
                unsigned stage = s_find.stage;
                if (entry->counts[stage] < 1000000) {
                    entry->sums[stage] += rssi;
                    entry->counts[stage]++;
                }
                entry->rssi = rssi;
                entry->last_sample_us = esp_timer_get_time();
            } else s_find.overflow = true;
        } else if (strcmp(obs.mac, s_cal.mac) == 0) {
            s_cal.rssi = rssi;
            if (!s_cal.has_sample || rssi < s_cal.minimum) s_cal.minimum = rssi;
            s_cal.has_sample = true;
            s_cal.samples++;
            s_cal.last_sample_us = esp_timer_get_time();
        }
    }
    portEXIT_CRITICAL(&s_cal_lock);
    if (calibrating || arg == &s_cal) return 0;
    bool watched = presence_matches(obs.mac);
    bool excluded = store_is_blacklisted(obs.mac);
    if (!watched && excluded) return 0;
    obs.rssi = rssi;
    /* Format remaining fields before independently checking MQTT. */
    if (!watched && rssi < cfg->min_rssi && rssi < cfg->mqtt_min_rssi) return 0;
    parse_adv_name(event->disc.data, event->disc.length_data, obs.ssid, OBS_SSID_MAX);
    strlcpy(obs.auth, "BLE", OBS_AUTH_MAX);
    obs.rssi = rssi;
    obs.channel = 0;
    obs.freq = 0;
    obs.mfgr = 0;
    obs.is_ble = 1;
    ts_quality_t quality = TS_UNKNOWN;
    if (!net_time_utc_with_quality(obs.ts, sizeof(obs.ts), &quality)) obs.ts[0] = '\0';
    obs.time_quality = (uint8_t)quality;

    if (watched) presence_on_obs(&obs);
    if (excluded) return 0;
    mqtt_on_obs(&obs);
    if (rssi < cfg->min_rssi) return 0;

    bool appended = false;
    if (store_append_obs(&obs, &appended) == ESP_OK && appended) {
        s_ble_count++;
    }
    return 0;
}

static void do_ble_scan(void) {
    if (s_ble_failed) return;
    if (!s_ble_ready) {
        return;
    }

    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 1;
    params.passive = 1;
    /* NimBLE defaults to a 30 ms window every 30 ms (100% duty cycle).
       Leave radio time for the AP while retaining passive BLE discovery.
       Units are 0.625 ms: 40 ms out of each 160 ms. */
    params.itvl = 256;
    params.window = 64;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                          SCAN_BLE_DURATION_MS,
                          &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc: %d", rc);
    }
}

/* --- tache de scan --- */

static void scan_loop(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "scanner: demarrage");

    while (s_running) {
        const app_config_t *cfg = config_get();
        int64_t now_us = esp_timer_get_time();

        /* A BLE discovery already in progress uses the shared radio. Defer
           the next Wi-Fi channel until it ends; the overdue scan is retried
           on the next loop, without cancelling BLE or the AP. */
        bool ble_discovery_active = s_ble_ready && ble_gap_disc_active();
        if (cfg->scan_wifi && !ble_discovery_active && (s_last_wifi_us == 0 ||
            (now_us - s_last_wifi_us) >= (int64_t)SCAN_WIFI_INTERVAL_S * 1000000)) {
            do_wifi_scan();
            s_last_wifi_us = esp_timer_get_time();
        }

        if (s_running && cfg->scan_ble && s_ble_ready && (s_last_ble_us == 0 ||
            (now_us - s_last_ble_us) >= (int64_t)SCAN_BLE_INTERVAL_S * 1000000)) {
            do_ble_scan();
            s_last_ble_us = esp_timer_get_time();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "scanner: arret");
    s_task = NULL;
    if (s_exit_sem) xSemaphoreGive(s_exit_sem);
    vTaskDelete(NULL);
}

static void ble_on_sync(void) { s_ble_failed = false; s_ble_ready = true; }

static void ble_on_reset(int reason) {
    ESP_LOGW(TAG, "BLE reset: %d", reason);
    s_ble_ready = false;
}

static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
}

esp_err_t scanner_init(void) {
    if (!s_exit_sem) {
        s_exit_sem = xSemaphoreCreateBinary();
        if (!s_exit_sem) {
            ESP_LOGE(TAG, "semaphore: echec allocation");
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_init: echec, BLE desactive");
        s_ble_failed = true;
        return err;
    }
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    nimble_port_freertos_init(ble_host_task);
    s_running = false;
    return ESP_OK;
}

esp_err_t scanner_start(void) {
    portENTER_CRITICAL(&s_cal_lock);
    bool calibrating = s_cal.active;
    portEXIT_CRITICAL(&s_cal_lock);
    if (calibrating) return ESP_ERR_INVALID_STATE;
    if (s_running || s_task) return ESP_ERR_INVALID_STATE;
    s_running = true;
    s_last_wifi_us = 0;
    s_last_ble_us = 0;
    if (xTaskCreate(scan_loop, "rfs_scan", 6144, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t scanner_stop(void) {
    portENTER_CRITICAL(&s_cal_lock);
    bool calibrating = s_cal.active;
    portEXIT_CRITICAL(&s_cal_lock);
    if (calibrating) return ESP_ERR_INVALID_STATE;
    if (!s_running && !s_task) return ESP_OK;
    s_running = false;
    if (s_ble_ready) ble_gap_disc_cancel();
    /* La boucle de scan doit etre terminee avant qu'un start suivant ne soit
       autorise, sinon deux taches peuvent tourner en parallele. */
    if (s_exit_sem) {
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGW(TAG, "scanner_stop: tache toujours active");
            return ESP_ERR_TIMEOUT;
        }
    }
    s_task = NULL;
    return ESP_OK;
}

bool scanner_is_running(void) { return s_running; }

static void calibration_task(void *arg) {
    (void)arg;
    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 0;
    params.passive = 1;
    params.itvl = 256;
    params.window = 64;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, CALIBRATION_TIMEOUT_MS, &params, gap_event_cb, &s_cal);
    int64_t deadline = esp_timer_get_time() + (int64_t)CALIBRATION_TIMEOUT_MS * 1000;
    for (;;) {
        portENTER_CRITICAL(&s_cal_lock);
        bool done = s_cal.stop_requested || s_cal_radio_done;
        portEXIT_CRITICAL(&s_cal_lock);
        if (rc || done || !s_ble_ready || esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    portENTER_CRITICAL(&s_cal_lock);
    bool resume = s_cal.resume_scan;
    s_cal.radio_error = rc != 0 || !s_ble_ready;
    s_cal.active = false;
    portEXIT_CRITICAL(&s_cal_lock);
    if (resume && scanner_start() != ESP_OK) {
        portENTER_CRITICAL(&s_cal_lock);
        s_cal.resume_error = true;
        portEXIT_CRITICAL(&s_cal_lock);
    }
    vTaskDelete(NULL);
}

esp_err_t scanner_calibration_start(const char *mac, const char *kind) {
    if (!mac || strlen(mac) != 17 || !kind || (strcmp(kind, "zone") && strcmp(kind, "alert") && strcmp(kind, "find")))
        return ESP_ERR_INVALID_ARG;
    char normalized[18];
    for (int i = 0; i < 17; i++) {
        if ((i % 3 == 2 && mac[i] != ':') || (i % 3 != 2 && !isxdigit((unsigned char)mac[i])))
            return ESP_ERR_INVALID_ARG;
        normalized[i] = (char)toupper((unsigned char)mac[i]);
    }
    normalized[17] = '\0';
    if (!scanner_ble_available()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_cal_lock);
    bool busy = s_cal.active;
    portEXIT_CRITICAL(&s_cal_lock);
    if (busy) return ESP_ERR_INVALID_STATE;
    bool resume = scanner_is_running();
    if (resume && scanner_stop() != ESP_OK) return ESP_FAIL;
    portENTER_CRITICAL(&s_cal_lock);
    memset(&s_cal, 0, sizeof(s_cal));
    if (strcmp(kind, "find") == 0) memset(&s_find, 0, sizeof(s_find));
    strlcpy(s_cal.mac, normalized, sizeof(s_cal.mac));
    strlcpy(s_cal.kind, kind, sizeof(s_cal.kind));
    s_cal.active = true;
    s_cal.resume_scan = resume;
    s_cal_radio_done = false;
    portEXIT_CRITICAL(&s_cal_lock);
    if (xTaskCreate(calibration_task, "rfs_cal", 4096, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_cal_lock); s_cal.active = false; portEXIT_CRITICAL(&s_cal_lock);
        if (resume && scanner_start() != ESP_OK) {
            portENTER_CRITICAL(&s_cal_lock); s_cal.resume_error = true; portEXIT_CRITICAL(&s_cal_lock);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t scanner_calibration_stop(void) {
    portENTER_CRITICAL(&s_cal_lock);
    bool active = s_cal.active;
    if (active) s_cal.stop_requested = true;
    portEXIT_CRITICAL(&s_cal_lock);
    return active ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void scanner_calibration_status(scanner_calibration_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_cal_lock);
    *out = s_cal;
    portEXIT_CRITICAL(&s_cal_lock);
}

esp_err_t scanner_find_start(void) {
    return scanner_calibration_start("00:00:00:00:00:00", "find");
}

esp_err_t scanner_find_next(void) {
    portENTER_CRITICAL(&s_cal_lock);
    bool ok = s_cal.active && !s_cal.stop_requested && strcmp(s_cal.kind, "find") == 0 && s_find.stage < 2;
    if (ok) s_find.stage++;
    portEXIT_CRITICAL(&s_cal_lock);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void scanner_find_status(scanner_find_result_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_cal_lock);
    *out = s_find;
    portEXIT_CRITICAL(&s_cal_lock);
}

void scanner_get_stats(uint32_t *wifi, uint32_t *ble) {
    if (wifi) *wifi = s_wifi_count;
    if (ble) *ble = s_ble_count;
}

void scanner_stats(char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    snprintf(out, outsz, "{\"wifi\":%lu,\"ble\":%lu,\"running\":%s}",
             (unsigned long)s_wifi_count, (unsigned long)s_ble_count,
             s_running ? "true" : "false");
}

bool scanner_ble_available(void) { return s_ble_ready && !s_ble_failed; }

void scanner_set_enabled(bool scan_wifi, bool scan_ble) {
    app_config_t *cfg = (app_config_t *)config_get();
    cfg->scan_wifi = scan_wifi;
    cfg->scan_ble = scan_ble;
}
