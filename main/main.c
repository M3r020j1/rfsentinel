#include <stdio.h>
#include <time.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config.h"
#include "auth.h"
#include "net.h"
#include "store.h"
#include "scanner.h"
#include "mqtt.h"
#include "presence.h"
#include "display.h"
#include "wigle.h"
#include "web.h"

static const char *TAG = "main";

static void print_boot_banner(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, " RFSentinel - WiFi/BLE sensor");
    ESP_LOGI(TAG, " build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "=================================");
}

void app_main(void)
{
    print_boot_banner();

    /* 0. NVS : doit etre initialise AVANT config_init et esp_wifi */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS invalide/obsolète, effacement...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(nvs_err));
    }

    /* 1. config */
    esp_err_t err = config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_init: %s", esp_err_to_name(err));
    }

    /* 1b. Authentication (reset du compteur d'echecs) */
    auth_init();

    /* 2. Ecran (boot screen affiche plus tard, quand l'IP est connue) */
    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "display_init: ecran indisponible");
    }
    display_set_language(config_get()->ui_lang);

    /* 3. WiFi + SNTP */
    net_init();

    /* 4. Stockage LittleFS */
    err = store_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "store_init: %s (scan sans stockage)", esp_err_to_name(err));
    }

    /* Presence configuration is loaded independently of the ordinary alerts. */
    err = presence_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "presence_init: %s", esp_err_to_name(err));

    /* 5. Scanner WiFi + BLE */
    err = scanner_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scanner_init: BLE indisponible: %s", esp_err_to_name(err));
    }
    if (config_get()->scan_autostart) {
        err = scanner_start();
        if (err != ESP_OK) ESP_LOGW(TAG, "scanner_start: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "demarrage automatique du scan desactive");
    }

    /* 5b. MQTT : demarre ici, pas seulement apres un POST /api/config */
    {
        esp_err_t merr = mqtt_init();
        if (merr != ESP_OK) ESP_LOGW(TAG, "mqtt_init: %s", esp_err_to_name(merr));
        else mqtt_start();
    }

    /* 6. Interface web */
    err = web_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web_start: %s", esp_err_to_name(err));
    }

    /* Boot screen : logo + version + mode + IP */
    {
        char ip[16] = "";
        net_get_ip(ip, sizeof(ip));
        display_boot_screen(ip, net_is_connected());
    }

    ESP_LOGI(TAG, "RFSentinel operationnel");

    /* Boucle d'entretien : reseau et ecran. */
    int tick5 = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        net_tick();
        display_tick();
        if (++tick5 >= 5) {
            tick5 = 0;
            char ip[16] = "";
            uint32_t w = 0, b = 0;
            net_get_ip(ip, sizeof(ip));
            scanner_get_stats(&w, &b);
            display_status(ip, net_is_connected(), w, b);
        }
    }
}
