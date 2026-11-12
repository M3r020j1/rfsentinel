#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* RFSentinel — ecran ST7735 du LilyGO T-Dongle S3.
 * Ecran 160x80, extinction automatique apres 1 min. */

esp_err_t display_init(void);

/* Ecran de démarrage : logo + nom + version + mode + IP. */
void display_boot_screen(const char *ip, bool sta_mode);

/* Rafraichit l'etat (mode, IP, compteurs) sur l'ecran. */
void display_status(const char *ip, bool sta_mode, uint32_t wifi, uint32_t ble);

/* Tache periodique : gere l'extinction automatique. */
void display_tick(void);

/* 0 = FR, 1 = EN */
void display_set_language(int lang);

/* Met l'ecran en veille (retroéclairage eteint). */
void display_blank(void);

bool display_is_ready(void);
