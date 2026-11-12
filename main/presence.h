#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "store.h"
#define PRESENCE_MAX 32
#define PRESENCE_TOPIC_MAX 64
#define PRESENCE_LABEL_MAX 32
typedef struct { char mac[18]; char label[PRESENCE_LABEL_MAX]; } presence_item_t;
typedef struct { char topic[PRESENCE_TOPIC_MAX]; uint16_t absence_s; uint16_t count; presence_item_t items[PRESENCE_MAX]; } presence_config_t;
esp_err_t presence_init(void);
esp_err_t presence_get(presence_config_t *out);
esp_err_t presence_settings(const char *topic, unsigned absence_s);
esp_err_t presence_add(const char *mac, const char *label);
esp_err_t presence_remove(const char *mac);
void presence_on_obs(const s3_obs_t *obs);
bool presence_matches(const char *mac);
