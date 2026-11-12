#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include "esp_err.h"

#define OBS_SSID_MAX 33
#define OBS_AUTH_MAX 16

typedef struct {
    char mac[18];
    char ssid[33];
    char auth[16];
    char ts[20];
    int  channel;
    int  freq;
    int  rssi;
    int  mfgr;
    bool is_ble;
    uint8_t time_quality;
} s3_obs_t;

esp_err_t store_init(void);
bool store_ready(void);
bool store_get_counts(uint32_t *wifi, uint32_t *ble);
/* Open a stable-length view while serializing metadata and reads with writers. */
esp_err_t store_upload_open(const char *path, FILE **file, size_t *length);
esp_err_t store_upload_read(FILE *file, void *buf, size_t want, size_t *got);
void store_upload_close(FILE *file);
esp_err_t store_append_obs(const s3_obs_t *obs, bool *appended);
/* Options de liste : tri a la demande et filtre par plage horaire (epoch UTC).
   Un champ nul ou zero laisse le critere inactif. */
typedef struct {
    const char *sort;
    const char *dir;
    const char *cursor;
    time_t from;
    time_t to;
} rfs_list_opt_t;

esp_err_t store_list(int page, const char *type, const rfs_list_opt_t *opt, char *out, size_t outsz);
esp_err_t store_history(const char *mac, char *out, size_t n);
esp_err_t store_purge(time_t from, time_t to, int *removed);
/* Export complet du journal : lecture sequentielle de tous les segments dans un
   seul flux CSV au format WiGLE. store_export_read renvoie 0 octets en fin de flux. */
typedef struct store_export store_export_t;
esp_err_t store_export_record_in_range(const char *record, time_t from, time_t to, bool *matches);
esp_err_t store_export_open(store_export_t **out);
long store_export_size(store_export_t *h);
bool store_export_has_data(store_export_t *h);
esp_err_t store_export_read(store_export_t *h, char *buf, size_t n, size_t *read);
void store_export_close(store_export_t *h);
esp_err_t store_stats(size_t *total, size_t *freeb, int *files);
esp_err_t store_blacklist_add(const char *mac, const char *label);
esp_err_t store_blacklist_list(char *out, size_t outsz);
esp_err_t store_blacklist_remove(const char *mac);
bool store_is_blacklisted(const char *mac);
