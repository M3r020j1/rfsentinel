#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* RFSentinel — scans passifs Wi-Fi 2.4 GHz et BLE via advertisements. */

esp_err_t scanner_init(void);

/* Demarre le scan (Wi-Fi passif + BLE). */
esp_err_t scanner_start(void);

/* Arreter les scans. */
esp_err_t scanner_stop(void);
bool scanner_is_running(void);

typedef struct {
    bool active, stop_requested, has_sample, radio_error, resume_error;
    bool resume_scan;
    int rssi, minimum;
    uint32_t samples;
    int64_t last_sample_us;
    char mac[18];
    char kind[8];
} scanner_calibration_t;
esp_err_t scanner_calibration_start(const char *mac, const char *kind);
esp_err_t scanner_calibration_stop(void);
void scanner_calibration_status(scanner_calibration_t *out);

#define SCANNER_FIND_MAX 48
typedef struct {
    char mac[18];
    int32_t sums[3];
    uint32_t counts[3];
    int rssi;
    int64_t last_sample_us;
} scanner_find_entry_t;
typedef struct {
    scanner_find_entry_t entries[SCANNER_FIND_MAX];
    uint32_t count;
    uint8_t stage;
    bool overflow;
} scanner_find_result_t;
esp_err_t scanner_find_start(void);
esp_err_t scanner_find_next(void);
void scanner_find_status(scanner_find_result_t *out);

typedef struct {
    uint32_t attempts;
    uint32_t failures;
    uint32_t found;
    uint32_t saved;
    int32_t last_error;
    uint8_t channel; /* 0 = all channels */
    uint8_t first_channel;
    uint8_t channel_count;
    int64_t completed_us;
} scanner_wifi_diagnostics_t;
void scanner_get_wifi_diagnostics(scanner_wifi_diagnostics_t *out);
typedef struct { uint32_t public_count, static_count, rpa_count, nrpa_count, unknown_count; } scanner_ble_diagnostics_t;
void scanner_get_ble_diagnostics(scanner_ble_diagnostics_t *out);

/* true si le stack BLE est operationnel. */
bool scanner_ble_available(void);

/* Etat des scans: {"wifi":N,"ble":M,"running":true|false} */
void scanner_stats(char *out, size_t outsz);

/* Compteurs bruts (Wi-Fi, BLE). */
void scanner_get_stats(uint32_t *wifi, uint32_t *ble);

/* Active/desactive chaque type de scan a chaud. */
void scanner_set_enabled(bool scan_wifi, bool scan_ble);
