#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

/* RFSentinel — couche reseau : STA + AP de secours, NTP, horloge. */

#define NET_AP_DEFAULT_IP "192.168.4.1"

typedef enum {
    TS_UNKNOWN = 0,
    TS_ESTIMATED = 1,
    TS_NTP_SYNCED = 2
} ts_quality_t;

/* Initialise le Wi-Fi (STA + AP de secours) + NTP. */
void net_init(void);
esp_err_t net_apply_ap_config(void);

/* true si connecte au reseau configure. */
bool net_is_connected(void);

/* "STA", "AP" ou "STA+AP". */
const char *net_get_mode(void);

/* IP actuelle (STA si connecte, sinon AP). */
bool net_get_ip(char *out, size_t n);

/* Qualite de l'horodatage courant. */
bool net_time_synced(void);
ts_quality_t net_time_quality(void);
const char *net_time_quality_str(void);

/* Horodatage UTC "YYYY-MM-DD HH:MM:SS" (toujours best-effort). */
bool net_time_utc(char *buf, size_t n);
bool net_time_utc_with_quality(char *buf, size_t n, ts_quality_t *quality);
esp_err_t net_set_time_from_client(time_t utc);

/* Dernier reglage reussi de l'horloge : NTP ou heure transmise par le
   navigateur ("0" si aucun). */
bool net_last_sync(char *buf, size_t n);

/* Variantes en heure locale, pour les affichages lisibles uniquement. */
bool net_time_local(char *buf, size_t n);
bool net_last_sync_local(char *buf, size_t n);

/* Reprise du fuseau horaire enregistre, sans redemarrer. */
void net_apply_time_config(void);

/* Tache periodique non bloquante (reconnexion, resync NTP). */
void net_tick(void);
