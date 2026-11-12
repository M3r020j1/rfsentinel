#pragma once
#include "esp_err.h"
#include <stddef.h>

/* WiGLE refused the API credentials (HTTP 401). */
#define RFS_ERR_WIGLE_AUTH ((esp_err_t)0x7101)

#include "store.h"

esp_err_t wigle_upload_journal(store_export_t *journal, const char *filename, char *out, size_t outsz);
/* Recherche d un equipement dans les tables publiques de WiGLE.
   type = WIFI, BLE, ou chaine vide pour tenter Wi-Fi puis Bluetooth.
   out recoit toujours un JSON valide: results/count/truncated,
   ou, en cas d echec, results vide avec un champ error. */
esp_err_t wigle_search_device(const char *mac, const char *type, char *out, size_t outsz);
