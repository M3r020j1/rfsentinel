#include "mqtt.h"

#define OBS_MAC_STR 18
#define OBS_TS_STR 20

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "config.h"
#include "net.h"

static const char *TAG = "rfs.mqtt";

#define MQTT_SEEN_MAX 16
#define MQTT_URL_MAX 96

typedef struct {
    char mac[OBS_MAC_STR];
    time_t last_seen;
} mq_seen_t;

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_initialized = false;
static SemaphoreHandle_t s_client_lock = NULL;
static volatile bool s_connected = false;
static char s_uri[MQTT_URL_MAX];
static mq_seen_t s_seen[MQTT_SEEN_MAX];
static int s_seen_count = 0;

static void json_escape(const char *src, char *dst, size_t n) {
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 2 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= n) break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 2 >= n) break;
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\r') {
            continue;
        } else if (c < 0x20 || c == 0x7f) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    (void)arg; (void)base;
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "connecte au broker");
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "deconnecte du broker");
            break;
        case MQTT_EVENT_ERROR: {
            esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
            if (event && event->error_handle) {
                const esp_mqtt_error_codes_t *e = event->error_handle;
                if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
                    ESP_LOGW(TAG, "broker refuse la connexion MQTT (code=%d)", (int)e->connect_return_code);
                else if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
                    ESP_LOGW(TAG, "transport MQTT: esp=0x%x tls=0x%x verification=0x%x socket=%d",
                             (unsigned)e->esp_tls_last_esp_err, (unsigned)e->esp_tls_stack_err,
                             (unsigned)e->esp_tls_cert_verify_flags, e->esp_transport_sock_errno);
                else ESP_LOGW(TAG, "erreur MQTT type=%d", (int)e->error_type);
            } else ESP_LOGW(TAG, "erreur MQTT sans detail");
            break;
        }
        default:
            break;
    }
}

esp_err_t mqtt_init(void) {
    if (!s_client_lock) {
        s_client_lock = xSemaphoreCreateMutex();
        if (!s_client_lock) return ESP_ERR_NO_MEM;
    }
    const app_config_t *cfg = config_get();
    if (!cfg->mqtt_enabled || cfg->mqtt_broker[0] == '\0') {
        ESP_LOGI(TAG, "MQTT desactive");
        return ESP_OK;
    }

    snprintf(s_uri, sizeof(s_uri), "%s%s:%u",
             cfg->mqtt_tls ? "mqtts://" : "mqtt://",
             cfg->mqtt_broker, (unsigned)cfg->mqtt_port);

    esp_mqtt_client_config_t mc = {0};
    mc.broker.address.uri = s_uri;
    if (cfg->mqtt_tls) mc.broker.verification.certificate = NULL;
    mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    if (cfg->mqtt_user[0]) mc.credentials.username = cfg->mqtt_user;
    if (cfg->mqtt_pass[0]) mc.credentials.authentication.password = cfg->mqtt_pass;
    if (cfg->mqtt_client_id[0]) mc.credentials.client_id = cfg->mqtt_client_id;

    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        xSemaphoreGive(s_client_lock);
        ESP_LOGE(TAG, "esp_mqtt_client_init: echec");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    s_initialized = true;
    xSemaphoreGive(s_client_lock);
    return ESP_OK;
}

void mqtt_start(void) {
    if (!s_client_lock) return;
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_initialized && s_client) esp_mqtt_client_start(s_client);
    xSemaphoreGive(s_client_lock);
}

void mqtt_stop(void) {
    if (!s_client_lock) return;
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_initialized && s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
    }
    s_client = NULL; s_initialized = false; s_connected = false;
    xSemaphoreGive(s_client_lock);
}

bool mqtt_is_connected(void) { return s_connected; }

bool mqtt_presence_publish(const s3_obs_t *obs, const char *label, const char *topic) {
    if (!s_client_lock || !obs || !label || !topic || !config_get()->mqtt_enabled) return false;
    char ssid[OBS_SSID_MAX * 2 + 3], name[96], device[CFG_DEVID_MAX * 2 + 3], ts[OBS_TS_STR], signal[16], payload[512];
    json_escape(obs->ssid, ssid, sizeof(ssid));
    json_escape(label, name, sizeof(name));
    json_escape(config_get()->device_id, device, sizeof(device));
    if (obs->ts[0]) strlcpy(ts, obs->ts, sizeof(ts));
    else if (!net_time_utc(ts, sizeof(ts))) strlcpy(ts, "0", sizeof(ts));
    if (obs->is_ble && obs->rssi == 127) strlcpy(signal, "null", sizeof(signal));
    else snprintf(signal, sizeof(signal), "%d", (int)obs->rssi);
    int n = snprintf(payload, sizeof(payload),
        "{\"event\":\"presence\",\"device_id\":\"%s\",\"mac\":\"%s\",\"type\":\"%s\","
        "\"label\":\"%s\",\"ssid\":\"%s\",\"rssi\":%s,\"ts\":\"%s\",\"tsq\":\"%s\"}",
        device, obs->mac, obs->is_ble ? "BLE" : "WIFI", name, ssid, signal, ts, net_time_quality_str());
    if (n <= 0 || n >= (int)sizeof(payload)) return false;
    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    int mid = (s_initialized && s_client) ? esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, 0, true) : -1;
    xSemaphoreGive(s_client_lock);
    return mid >= 0;
}

void mqtt_on_obs(const s3_obs_t *obs) {
    if (!s_client_lock || !obs) return;

    const app_config_t *cfg = config_get();
    if (!cfg->mqtt_enabled) return;

    /* seuil MQTT independant du seuil d'enregistrement */
    if (obs->rssi < cfg->mqtt_min_rssi) return;

    /* BLE callbacks and the Wi-Fi task can arrive concurrently. Keep the
       rediscovery check, publish and cache update in one critical section. */
    xSemaphoreTake(s_client_lock, portMAX_DELAY);

    /* deduplication: ne pas realerter avant le delai d'absence */
    time_t now = time(NULL);
    for (int i = 0; i < s_seen_count; i++) {
        if (strcmp(s_seen[i].mac, obs->mac) == 0) {
            if (cfg->rediscover_min > 0 &&
                (now - s_seen[i].last_seen) < (time_t)cfg->rediscover_min * 60) {
                xSemaphoreGive(s_client_lock);
                return;
            }
            break;
        }
    }

    char mac[OBS_MAC_STR];
    strlcpy(mac, obs->mac, sizeof(mac));
    char ssid[OBS_SSID_MAX];
    strlcpy(ssid, obs->ssid, sizeof(ssid));
    char ssid_j[OBS_SSID_MAX * 2 + 3];
    json_escape(ssid, ssid_j, sizeof(ssid_j));

    char ts[OBS_TS_STR];
    if (obs->ts[0]) {
        strlcpy(ts, obs->ts, sizeof(ts));
    } else {
        if (!net_time_utc(ts, sizeof(ts))) strlcpy(ts, "0", sizeof(ts));
    }

    char payload[384];
    int n = snprintf(payload, sizeof(payload),
                     "{\"device_id\":\"%s\",\"mac\":\"%s\",\"type\":\"%s\",\"ssid\":\"%s\","
                     "\"rssi\":%d,\"ts\":\"%s\",\"tsq\":\"%s\",\"lat\":%.5f,\"lon\":%.5f}",
                     cfg->device_id, mac, obs->is_ble ? "BLE" : "WIFI", ssid_j,
                     (int)obs->rssi, ts, net_time_quality_str(),
                     (double)cfg->lat, (double)cfg->lon);
    if (n <= 0 || n >= (int)sizeof(payload)) { xSemaphoreGive(s_client_lock); return; }

    int mid = (s_initialized && s_client) ? esp_mqtt_client_publish(s_client, cfg->mqtt_topic, payload, 0, 1, 0) : -1;
    if (mid < 0) {
        ESP_LOGW(TAG, "publish: echec");
        xSemaphoreGive(s_client_lock);
        return;
    }

    /* marquer comme vu */
    bool found = false;
    for (int i = 0; i < s_seen_count; i++) {
        if (strcmp(s_seen[i].mac, mac) == 0) {
            s_seen[i].last_seen = now;
            found = true;
            break;
        }
    }
    if (!found) {
        int slot = s_seen_count;
        if (slot < MQTT_SEEN_MAX) s_seen_count++;
        else {
            slot = 0;
            for (int i = 1; i < MQTT_SEEN_MAX; i++)
                if (s_seen[i].last_seen < s_seen[slot].last_seen) slot = i;
        }
        strlcpy(s_seen[slot].mac, mac, sizeof(s_seen[0].mac));
        s_seen[slot].last_seen = now;
    }
    xSemaphoreGive(s_client_lock);
}
