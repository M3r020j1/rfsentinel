#include "display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"

#include "oled_font.h"
#include "oled_logo.h"
#include "version.h"

#include "config.h"
#include "net.h"

static const char *TAG = "rfs.display";

#define RFS_SCREEN_W 160
#define RFS_SCREEN_H 80
#define RFS_BOOT_HOLD_MS 60000
#define RFS_LINE_H 16
#define RFS_MARGIN 4
#define RFS_LOGO_H 64
/* Credit affiche en bas a droite de l'ecran de demarrage. */
/* Texte du credit : By M3r0W1g (bitmap ci-dessous). */
/* Version definie dans version.h (partagee avec la WebUI). */

/* Pins LilyGO T-Dongle S3 (verifiees: schema + ports ESPHome/LVGL). */
#define RFS_PIN_LCD_CLK 5
#define RFS_PIN_LCD_MOSI 3
#define RFS_PIN_LCD_DC 2
#define RFS_PIN_LCD_RST 1
#define RFS_PIN_LCD_CS 4
#define RFS_PIN_LCD_BL 38

static bool s_display_ready = false;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static int s_lang = 0;
static int64_t s_boot_us = 0;
static bool s_blanked = false;
static uint16_t *s_framebuffer = NULL;
static SemaphoreHandle_t s_transfer_done = NULL;
static bool s_transfer_pending = false;

/* The SPI driver keeps using the pixels after draw_bitmap returns. */
static bool lcd_transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx) {
    (void)io; (void)event; (void)ctx;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_transfer_done, &wake);
    return wake == pdTRUE;
}

static bool wait_for_frame(void) {
    if (!s_transfer_pending) return true;
    if (xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "transfert ecran en attente ; tampon conserve");
        return false;
    }
    s_transfer_pending = false;
    return true;
}

static void send_frame(void) {
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, RFS_SCREEN_W, RFS_SCREEN_H, s_framebuffer);
    if (err == ESP_OK) s_transfer_pending = true;
    else ESP_LOGW(TAG, "transfert ecran impossible: %s", esp_err_to_name(err));
}

static void release_frame(void) {
    if (!s_framebuffer || !wait_for_frame()) return;
    free(s_framebuffer);
    s_framebuffer = NULL;
    ESP_LOGI(TAG, "tampon ecran libere: libre=%u bloc_max=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void fill_screen(uint16_t *fb, uint16_t color) {
    for (int i = 0; i < RFS_SCREEN_W * RFS_SCREEN_H; i++) fb[i] = color;
}

/* Correct both panel axes in the framebuffer without changing GRAM offsets. */
static void rotate_frame(uint16_t *fb) {
    int pixels = RFS_SCREEN_W * RFS_SCREEN_H;
    for (int i = 0; i < pixels / 2; i++) {
        uint16_t tmp = fb[i];
        fb[i] = fb[pixels - 1 - i];
        fb[pixels - 1 - i] = tmp;
    }
}

static void draw_frame(uint16_t *fb, int x, int y, int w, int h, uint16_t color) {
    for (int i = x; i < x + w; i++) {
        if (i < 0 || i >= RFS_SCREEN_W) continue;
        if (y >= 0 && y < RFS_SCREEN_H) fb[y * RFS_SCREEN_W + i] = color;
        if (y + h - 1 >= 0 && y + h - 1 < RFS_SCREEN_H) fb[(y + h - 1) * RFS_SCREEN_W + i] = color;
    }
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= RFS_SCREEN_H) continue;
        if (x >= 0 && x < RFS_SCREEN_W) fb[j * RFS_SCREEN_W + x] = color;
        if (x + w - 1 >= 0 && x + w - 1 < RFS_SCREEN_W) fb[j * RFS_SCREEN_W + x + w - 1] = color;
    }
}

esp_err_t display_init(void) {
    s_transfer_done = xSemaphoreCreateBinary();
    if (!s_transfer_done) return ESP_ERR_NO_MEM;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RFS_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = RFS_PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RFS_SCREEN_W * RFS_SCREEN_H * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_init: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_transfer_done); s_transfer_done = NULL;
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = RFS_PIN_LCD_DC,
        .cs_gpio_num = RFS_PIN_LCD_CS,
        .pclk_hz = 20000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_transfer_done,
    };

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)1, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd io: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_transfer_done); s_transfer_done = NULL;
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = RFS_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_st7735(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st7735: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(s_io); s_io = NULL;
        vSemaphoreDelete(s_transfer_done); s_transfer_done = NULL;
        return err;
    }

    s_framebuffer = heap_caps_malloc(RFS_SCREEN_W * RFS_SCREEN_H * sizeof(*s_framebuffer), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "allocation tampon ecran impossible");
        esp_lcd_panel_del(s_panel); s_panel = NULL;
        esp_lcd_panel_io_del(s_io); s_io = NULL;
        vSemaphoreDelete(s_transfer_done); s_transfer_done = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* Format paysage 160x80 de la T-Dongle S3 (greentab) : swap XY +
       mirror(false,true) + decalage GRAM (gap 1,26) — conforme a l'exemple
       officiel LilyGO T-Dongle-S3 (examples/factory_screen). */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_set_gap(s_panel, 1, 26);
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* Retroéclairage GPIO38 ACTIF A L'ETAT BAS (LCD_BK_LIGHT_ON=0 dans le
       code officiel LilyGO) : LOW = allume, HIGH = eteint. */
    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << RFS_PIN_LCD_BL,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(RFS_PIN_LCD_BL, 0);

    s_display_ready = true;
    s_boot_us = esp_timer_get_time();
    s_blanked = false;
    s_lang = config_get()->ui_lang;

    ESP_LOGI(TAG, "display: pret");
    return ESP_OK;
}

static void draw_text_line(uint16_t *fb, int x, int y, const char *s) {
    if (!s) return;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const uint16_t *g = oled_font_8x16[c - FONT_FIRST];
        for (int col = 0; col < FONT_W; col++) {
            uint16_t bits = g[col];
            for (int row = 0; row < FONT_H; row++) {
                int px = x + i * FONT_W + col;
                int py = y + row;
                if (px < 0 || px >= RFS_SCREEN_W || py < 0 || py >= RFS_SCREEN_H) continue;
                fb[py * RFS_SCREEN_W + px] = (bits & (1u << row)) ? 0xFFFF : 0x0000;
            }
        }
    }
}

/* Compact 5x7 bitmap centered below the logo. */
static void draw_credit(uint16_t *fb) {
    static const uint8_t glyphs[][7] = {
        {30,17,17,30,17,17,30}, /* B */
        {0,17,17,15,1,17,14},   /* y */
        {0,0,0,0,0,0,0},       /* space */
        {17,27,21,21,17,17,17}, /* M */
        {30,1,1,14,1,1,30},    /* 3 */
        {0,0,22,25,16,16,16},  /* r */
        {14,17,19,21,25,17,14},/* 0 */
        {17,17,17,21,21,21,10},/* W */
        {4,12,4,4,4,4,14},     /* 1 */
        {0,15,17,17,15,1,14},  /* g */
    };
    const int count = (int)(sizeof(glyphs) / sizeof(glyphs[0]));
    const int scale = 1, advance = 6;
    const int width = (count - 1) * advance + 5 * scale;
    const int x0 = (RFS_SCREEN_W - width) / 2;
    const int y0 = OLED_LOGO_H + (RFS_SCREEN_H - OLED_LOGO_H - 7 * scale) / 2;
    for (int i = 0; i < count; i++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyphs[i][row] & (1u << (4 - col)))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++)
                            fb[(y0 + row * scale + dy) * RFS_SCREEN_W + x0 + i * advance + col * scale + dx] = 0xFFFF;
}

void display_boot_screen(const char *ip, bool sta_mode) {
    if (!s_display_ready || s_blanked || !s_framebuffer || !wait_for_frame()) return;
    uint16_t *fb = s_framebuffer;

    fill_screen(fb, 0x0000);

    {
        const int icon_width = OLED_LOGO_W;
        int x0 = (RFS_SCREEN_W - icon_width) / 2;
        /* Le logo 64x64 est colle en haut : 64 + 16 = 80, la hauteur utile de
           l'ecran. Le credit en bas de ligne ne chevauche ainsi aucun pixel du
           logo, dont les lignes allees s'arretent a y=62. */
        int y0 = 0;
        const int row_bytes = (OLED_LOGO_W + 7) / 8;
        for (int row = 0; row < OLED_LOGO_H; row++) {
            int y = y0 + row;
            if (y >= RFS_SCREEN_H) break;
            const uint8_t *bits = &oled_logo_bits[row * row_bytes];
            for (int col = 0; col < icon_width; col++) {
                int x = x0 + col;
                if (x >= RFS_SCREEN_W) break;
                int on = (bits[col >> 3] >> (7 - (col & 7))) & 1;
                fb[y * RFS_SCREEN_W + x] = on ? 0xFFFF : 0x0000;
            }
        }
    }

    /* Credit centre dans la zone sous le logo. */
    draw_credit(fb);

    (void)ip;
    (void)sta_mode;
    rotate_frame(fb);
    send_frame();
}

void display_status(const char *ip, bool sta_mode, uint32_t wifi, uint32_t ble) {
    if (!s_display_ready || s_blanked || !s_framebuffer || !wait_for_frame()) return;
    uint16_t *fb = s_framebuffer;

    fill_screen(fb, 0x0000);

    int y = 16;
    char line[64];

    snprintf(line, sizeof(line), "RFSentinel v%s", RFS_VERSION);
    draw_text_line(fb, RFS_MARGIN, y, line);
    y += RFS_LINE_H;

    if (ip && ip[0]) {
        snprintf(line, sizeof(line), "%s: %s", s_lang == 1 ? "IP" : "IP", ip);
    } else {
        strlcpy(line, s_lang == 1 ? "Connecting..." : "Connexion...", sizeof(line));
    }
    draw_text_line(fb, RFS_MARGIN, y, line);
    y += RFS_LINE_H;
    draw_text_line(fb, RFS_MARGIN, y, sta_mode ? "Mode: Wi-Fi" : "Mode: AP");
    (void)wifi;
    (void)ble;

    rotate_frame(fb);
    send_frame();
}

void display_tick(void) {
    if (!s_display_ready) return;
    int64_t now = esp_timer_get_time();
    if (!s_blanked && (now - s_boot_us) / 1000 >= RFS_BOOT_HOLD_MS) {
        display_blank();
    }
    if (s_blanked) release_frame();
}

void display_set_language(int lang) { s_lang = (lang == 1) ? 1 : 0; }

void display_blank(void) {
    if (!s_display_ready) return;
    if (s_blanked) { release_frame(); return; }
    s_blanked = true;
    esp_lcd_panel_disp_on_off(s_panel, false);
    gpio_set_level(RFS_PIN_LCD_BL, 1);
    release_frame();
    ESP_LOGI(TAG, "ecran eteint (economies)");
}

bool display_is_ready(void) { return s_display_ready; }
