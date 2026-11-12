#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "store.h"

/* RFSentinel — service MQTT : alertes de nouveaux equipements. */

esp_err_t mqtt_init(void);
void mqtt_start(void);
void mqtt_stop(void);
bool mqtt_is_connected(void);

/* Appelee par le scanner apres chaque observation enregistree.
 * Gere la deduplication et le seuil RSSI MQTT en interne. */
void mqtt_on_obs(const s3_obs_t *obs);
/* Queue a presence event on its own topic without applying RSSI thresholds. */
bool mqtt_presence_publish(const s3_obs_t *obs, const char *label, const char *topic);
