#include "presence.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt.h"

#define PRESENCE_NS "rfs_presence"
#define PRESENCE_KEY "config_v1"
#define PRESENCE_RETRY_US (5LL * 1000000)
static SemaphoreHandle_t s_lock;
static presence_config_t s_config;
typedef struct { bool seen, pending; int64_t last_seen, last_try; } runtime_t;
static runtime_t s_runtime[PRESENCE_MAX];

static bool canonical(const char *mac, char out[18]) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (mac[i] != ':') return false; out[i] = ':'; }
        else { if (!isxdigit((unsigned char)mac[i])) return false; out[i] = (char)toupper((unsigned char)mac[i]); }
    }
    out[17] = 0; return true;
}
static bool valid_topic(const char *topic) {
    if (!topic || !*topic || strlen(topic) >= PRESENCE_TOPIC_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)topic; *p; p++)
        if (*p < 0x20 || *p == 0x7f || *p == '+' || *p == '#') return false;
    return true;
}
static esp_err_t save_locked(const presence_config_t *next) {
    nvs_handle_t h; esp_err_t err = nvs_open(PRESENCE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, PRESENCE_KEY, next, sizeof(*next));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h); return err;
}
esp_err_t presence_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    presence_config_t *loaded = calloc(1, sizeof(*loaded));
    if (!loaded) { vSemaphoreDelete(s_lock); s_lock=NULL; return ESP_ERR_NO_MEM; }
    strlcpy(loaded->topic, "/rfsentinel/presence", sizeof(loaded->topic)); loaded->absence_s = 120;
    nvs_handle_t h; esp_err_t err = nvs_open(PRESENCE_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        size_t size = sizeof(*loaded);
        err = nvs_get_blob(h, PRESENCE_KEY, loaded, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) { memset(loaded, 0, sizeof(*loaded)); strlcpy(loaded->topic, "/rfsentinel/presence", sizeof(loaded->topic)); loaded->absence_s=120; err=ESP_OK; }
        else if (err == ESP_OK && size != sizeof(*loaded)) err = ESP_ERR_INVALID_SIZE;
        nvs_close(h);
    }
    if (err == ESP_OK) {
        if (loaded->count > PRESENCE_MAX || !memchr(loaded->topic, 0, sizeof(loaded->topic)) || !valid_topic(loaded->topic) || loaded->absence_s < 30) err = ESP_ERR_INVALID_STATE;
        for (unsigned i=0; err==ESP_OK && i<loaded->count; i++) {
            char mac[18];
            if (!memchr(loaded->items[i].mac, 0, sizeof(loaded->items[i].mac)) || !memchr(loaded->items[i].label, 0, sizeof(loaded->items[i].label)) || !canonical(loaded->items[i].mac, mac) || strcmp(mac,loaded->items[i].mac)) err=ESP_ERR_INVALID_STATE;
        }
    }
    if (err == ESP_OK) { xSemaphoreTake(s_lock,portMAX_DELAY); s_config=*loaded; memset(s_runtime,0,sizeof(s_runtime)); xSemaphoreGive(s_lock); }
    else { vSemaphoreDelete(s_lock); s_lock=NULL; }
    free(loaded); return err;
}
esp_err_t presence_get(presence_config_t *out) {
    if (!out || !s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock,portMAX_DELAY); *out=s_config; xSemaphoreGive(s_lock); return ESP_OK;
}
esp_err_t presence_settings(const char *topic, unsigned absence_s) {
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!valid_topic(topic) || absence_s < 30 || absence_s > 65535) return ESP_ERR_INVALID_ARG;
    presence_config_t *next=malloc(sizeof(*next)); if(!next)return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock,portMAX_DELAY); *next=s_config; strlcpy(next->topic,topic,sizeof(next->topic)); next->absence_s=(uint16_t)absence_s;
    esp_err_t err=save_locked(next); if(err==ESP_OK)s_config=*next; xSemaphoreGive(s_lock); free(next); return err;
}
esp_err_t presence_add(const char *mac,const char *label) {
    char normalized[18]; if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!canonical(mac,normalized) || !label || strlen(label)>=PRESENCE_LABEL_MAX) return ESP_ERR_INVALID_ARG;
    presence_config_t *next=malloc(sizeof(*next)); if(!next)return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock,portMAX_DELAY); *next=s_config; unsigned i=0; while(i<next->count && strcmp(next->items[i].mac,normalized))i++;
    esp_err_t err=ESP_OK;
    if(i==next->count && i==PRESENCE_MAX)err=ESP_ERR_NO_MEM;
    else { if(i==next->count)next->count++; strlcpy(next->items[i].mac,normalized,sizeof(next->items[i].mac)); strlcpy(next->items[i].label,label,sizeof(next->items[i].label)); err=save_locked(next); if(err==ESP_OK){bool added=i>=s_config.count;s_config=*next;if(added)memset(&s_runtime[i],0,sizeof(s_runtime[i]));} }
    xSemaphoreGive(s_lock);free(next);return err;
}
esp_err_t presence_remove(const char *mac) {
    char normalized[18];if(!s_lock)return ESP_ERR_INVALID_STATE;if(!canonical(mac,normalized))return ESP_ERR_INVALID_ARG;
    presence_config_t *next=malloc(sizeof(*next));if(!next)return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock,portMAX_DELAY);*next=s_config;unsigned i=0;while(i<next->count && strcmp(next->items[i].mac,normalized))i++;
    esp_err_t err=ESP_OK;
    if(i<next->count){unsigned remaining=next->count-i-1;memmove(&next->items[i],&next->items[i+1],remaining*sizeof(next->items[0]));next->count--;memset(&next->items[next->count],0,sizeof(next->items[0]));err=save_locked(next);if(err==ESP_OK){s_config=*next;memmove(&s_runtime[i],&s_runtime[i+1],remaining*sizeof(s_runtime[0]));memset(&s_runtime[s_config.count],0,sizeof(s_runtime[0]));}}
    xSemaphoreGive(s_lock);free(next);return err;
}
bool presence_matches(const char *mac) {
    if (!s_lock || !mac) return false;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for(unsigned i=0;i<s_config.count;i++)if(!strcmp(s_config.items[i].mac,mac)){found=true;break;}
    xSemaphoreGive(s_lock);return found;
}
void presence_on_obs(const s3_obs_t *obs) {
    if(!s_lock || !obs)return;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    for(unsigned i=0;i<s_config.count;i++)if(!strcmp(s_config.items[i].mac,obs->mac)){
        runtime_t *r=&s_runtime[i];int64_t now=esp_timer_get_time();
        if(!r->seen || now-r->last_seen >= (int64_t)s_config.absence_s*1000000){r->pending=true;r->last_try=0;}
        r->seen=true;r->last_seen=now;
        if(r->pending && (!r->last_try || now-r->last_try>=PRESENCE_RETRY_US)){
            r->last_try=now;
            if(mqtt_presence_publish(obs,s_config.items[i].label,s_config.topic))r->pending=false;
        }
        break;
    }
    xSemaphoreGive(s_lock);
}
