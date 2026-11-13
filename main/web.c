#include "web.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "auth.h"
#include "config.h"
#include "mqtt.h"
#include "presence.h"
#include "net.h"
#include "scanner.h"
#include "store.h"
#include "wigle.h"
#include "version.h"

static const char *TAG = "rfs.web";

static httpd_handle_t s_server = NULL;
typedef struct { time_t from; time_t to; } purge_args_t;
static portMUX_TYPE s_purge_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_purge_running = false;
static int s_purge_removed = 0;
static esp_err_t s_purge_result = ESP_OK;

static void purge_task(void *arg) {
    purge_args_t args = *(purge_args_t *)arg;
    free(arg);
    int removed = 0;
    esp_err_t result = store_purge(args.from, args.to, &removed);
    portENTER_CRITICAL(&s_purge_lock);
    s_purge_removed = removed;
    s_purge_result = result;
    s_purge_running = false;
    portEXIT_CRITICAL(&s_purge_lock);
    ESP_LOGI(TAG, "purge terminee: %d observations, %s", removed, esp_err_to_name(result));
    vTaskDelete(NULL);
}

static void apply_ap_later(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_err_t err = net_apply_ap_config();
    if (err != ESP_OK) ESP_LOGW(TAG, "nouvelle config AP enregistree ; appliquee au prochain demarrage ou mode AP");
    vTaskDelete(NULL);
}

extern const char _binary_index_html_start[];
extern const char _binary_index_html_end[];
extern const char _binary_rfsentinel_logo_svg_start[];
extern const char _binary_rfsentinel_logo_svg_end[];
extern const char _binary_rfsentinel_icon_svg_start[];
extern const char _binary_rfsentinel_icon_svg_end[];

#define RFS_UI_BUF      4096
#define RFS_OBS_BUF      8192
#define RFS_OBS_FALLBACK_BUF 4096
#define RFS_HIST_BUF    8192
#define RFS_POST_MAX    4096
#define RFS_WIGLE_BUF   8192

/* The HTTP server processes URI handlers sequentially. Reserve this workspace
   at boot so an 8 KiB allocation cannot split a large free block just before
   the WiGLE TLS handshake. It is also used for the bounded JSON body. */
static char s_wigle_workspace[RFS_WIGLE_BUF];

/* --- helpers --- */

static void json_escape(const char *src, char *dst, size_t n) {
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 2 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= n) break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 2 >= n) break;
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\r') {
            continue;
        } else if (c < 0x20 || c == 0x7f) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static void send_json(httpd_req_t *req, int code, const char *json) {
    char status[32];
    snprintf(status, sizeof(status), "%03d %s", code,
             (code == 200) ? "OK" :
             (code == 400) ? "Bad Request" :
             (code == 401) ? "Unauthorized" :
             (code == 404) ? "Not Found" :
             (code >= 500) ? "Internal Server Error" : "Unknown");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

static void send_err(httpd_req_t *req, const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
}

/* Le navigateur percent-encode les adresses materielles (deux-points -> %3A)
   et httpd_query_key_value recopie la valeur brute sans la decoder : sans
   cette etape une adresse parait invalide et ne matche aucune ligne du
   journal. On decode sur place, en ignorant un sequence incomplete. */
static void qs_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char h[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(h, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Adresse materielle normalisee : decodee puis en majuscules, comme le format
   du journal et celui attendu par WiGLE. */
static void qs_mac(char *s)
{
    qs_decode(s);
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
}

static char *read_body(httpd_req_t *req) {
    int len = req->content_len;
    if (len <= 0 || len > RFS_POST_MAX) return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[got] = '\0';
    return buf;
}

/* Autorisation : cookie de session OU HTTP Basic (repli robuste si le
   navigateur ne rejoue pas le cookie). */
static bool req_authorized(httpd_req_t *req) {
    /* Lockout en cours: ni cookie ni Basic ne passent (anti brute-force) */
    if (auth_locked()) return false;

    if (auth_check_request(req)) return true;

    char hdr[192] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;

    unsigned char dec[128];
    size_t olen = 0;
    const char *b64 = hdr + 6;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen,
                              (const unsigned char *)b64, strlen(b64)) != 0)
        return false;
    dec[olen] = '\0';

    char *sep = strchr((char *)dec, ':');
    if (!sep) return false;
    *sep = '\0';
    if (config_check_credentials((const char *)dec, sep + 1)) return true;

    /* mot de passe incorrect sur le canal Basic: meme compteur que /login */
    auth_note_failure();
    return false;
}

/* --- routes publiques --- */

static esp_err_t h_index(httpd_req_t *req) {
    size_t len = (size_t)(_binary_index_html_end - _binary_index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, _binary_index_html_start, (ssize_t)len);
    ESP_LOGI(TAG, "GET / : %u octets, resultat=%s", (unsigned)len, esp_err_to_name(err));
    return err;
}

static esp_err_t h_logo_logo(httpd_req_t *req) {
    size_t len = (size_t)(_binary_rfsentinel_logo_svg_end - _binary_rfsentinel_logo_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    esp_err_t err = httpd_resp_send(req, _binary_rfsentinel_logo_svg_start, (ssize_t)len);
    if (err != ESP_OK) ESP_LOGW(TAG, "GET logo: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t h_logo_icon(httpd_req_t *req) {
    size_t len = (size_t)(_binary_rfsentinel_icon_svg_end - _binary_rfsentinel_icon_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    esp_err_t err = httpd_resp_send(req, _binary_rfsentinel_icon_svg_start, (ssize_t)len);
    if (err != ESP_OK) ESP_LOGW(TAG, "GET icone: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t h_login(httpd_req_t *req) {
    char *body = read_body(req);
    if (!body) { send_err(req, "bad_request"); return ESP_OK; }

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { send_err(req, "bad_json"); return ESP_OK; }

    cJSON *ju = cJSON_GetObjectItemCaseSensitive(j, "user");
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "pass");
    const char *user = (ju && cJSON_IsString(ju)) ? ju->valuestring : "";
    const char *pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : "";

    bool ok = auth_login(user, pass);
    cJSON_Delete(j);

    if (!ok) {
        ESP_LOGW(TAG, "connexion refusee");
        /* ne jamais logguer ni echo le mot de passe */
        send_err(req, "bad_credentials");
        return ESP_OK;
    }

    char cookie[96];
    auth_set_cookie(req, cookie, sizeof(cookie));
    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"must_change\":%s,\"lang\":%d}",
             config_needs_password_change() ? "true" : "false",
             (int)config_get()->ui_lang);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    ESP_LOGI(TAG, "connexion acceptee, reponse=%s", esp_err_to_name(err));
    return ESP_OK;
}

static esp_err_t h_logout(httpd_req_t *req) {
    auth_logout();
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/* --- config --- */

static esp_err_t h_config_get(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }

    const app_config_t *c = config_get();
    char *out = malloc(RFS_UI_BUF);
    if (!out) { send_json(req, 400, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }

    char ssid[CFG_SSID_MAX * 2 + 3], apssid[CFG_SSID_MAX * 2 + 3];
    char wuser[CFG_USER_MAX * 2 + 3];
    char broker[CFG_BROKER_MAX * 2 + 3], mqtt_user[CFG_MQTT_USER_MAX * 2 + 3];
    char topic[CFG_MQTT_TOPIC_MAX * 2 + 3], cid[CFG_MQTT_ID_MAX * 2 + 3];
    char ntp[CFG_NTP_MAX * 2 + 3], tz[CFG_TZ_MAX * 2 + 3], dev[CFG_DEVID_MAX * 2 + 3];
    json_escape(c->wifi_ssid, ssid, sizeof(ssid));
    json_escape(c->web_user, wuser, sizeof(wuser));
    json_escape(c->ap_ssid, apssid, sizeof(apssid));
    json_escape(c->mqtt_broker, broker, sizeof(broker));
    json_escape(c->mqtt_user, mqtt_user, sizeof(mqtt_user));
    json_escape(c->mqtt_topic, topic, sizeof(topic));
    json_escape(c->mqtt_client_id, cid, sizeof(cid));
    json_escape(c->ntp_server, ntp, sizeof(ntp));
    json_escape(c->tz, tz, sizeof(tz));
    json_escape(c->device_id, dev, sizeof(dev));

    snprintf(out, RFS_UI_BUF,
        "{"
        "\"wifi_ssid\":\"%s\",\"wifi_pass_set\":%s,\"web_user\":\"%s\",\"web_pass_set\":%s,"
        "\"wigle_user_set\":%s,\"wigle_token_set\":%s,"
        "\"ap_ssid\":\"%s\",\"ap_pass_set\":%s,"
        "\"lat\":%.6f,\"lon\":%.6f,"
        "\"min_rssi\":%d,\"mqtt_min_rssi\":%d,\"rediscover_min\":%d,"
        "\"scan_wifi\":%d,\"scan_ble\":%d,\"ble_address_filter\":%d,\"scan_autostart\":%d,\"ui_lang\":%d,"
        "\"mqtt_enabled\":%d,\"mqtt_broker\":\"%s\",\"mqtt_port\":%d,"
        "\"mqtt_user_set\":%s,\"mqtt_user\":\"%s\",\"mqtt_pass_set\":%s,\"mqtt_topic\":\"%s\","
        "\"mqtt_client_id\":\"%s\",\"mqtt_tls\":%d,"
        "\"ntp_server\":\"%s\",\"tz\":\"%s\",\"device_id\":\"%s\""
        "}",
        ssid, c->wifi_pass[0] ? "true" : "false",
        wuser, c->web_pass[0] ? "true" : "false",
        c->wigle_user[0] ? "true" : "false", c->wigle_token[0] ? "true" : "false",
        apssid, c->ap_password[0] ? "true" : "false",
        (double)c->lat, (double)c->lon,
        (int)c->min_rssi, (int)c->mqtt_min_rssi, (int)c->rediscover_min,
        (int)c->scan_wifi, (int)c->scan_ble, (int)c->ble_address_filter, (int)c->scan_autostart, (int)c->ui_lang,
        (int)c->mqtt_enabled, broker, (int)c->mqtt_port,
        (c->mqtt_user[0] ? "true" : "false"), mqtt_user,
        c->mqtt_pass[0] ? "true" : "false",
        topic, cid, (int)c->mqtt_tls,
        ntp, tz, dev);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    ESP_LOGI(TAG, "GET config : %s", esp_err_to_name(err));
    free(out);
    return ESP_OK;
}

static esp_err_t h_config_set(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }

    char *body = read_body(req);
    if (!body) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_json\"}"); return ESP_OK; }

    cJSON *wigle_name = cJSON_GetObjectItemCaseSensitive(j, "wigle_user");
    cJSON *wigle_token = cJSON_GetObjectItemCaseSensitive(j, "wigle_token");
    if ((cJSON_IsString(wigle_name) && strlen(wigle_name->valuestring) >= CFG_WIGLE_USER_MAX) ||
        (cJSON_IsString(wigle_token) && strlen(wigle_token->valuestring) >= CFG_TOKEN_MAX)) {
        cJSON_Delete(j);
        send_json(req, 400, "{\"ok\":false,\"error\":\"wigle_credentials_too_long\"}");
        return ESP_OK;
    }

    cJSON *mqtt_port = cJSON_GetObjectItemCaseSensitive(j, "mqtt_port");
    cJSON *mqtt_user = cJSON_GetObjectItemCaseSensitive(j, "mqtt_user");
    cJSON *mqtt_pass = cJSON_GetObjectItemCaseSensitive(j, "mqtt_pass");
    cJSON *mqtt_broker = cJSON_GetObjectItemCaseSensitive(j, "mqtt_broker");
    cJSON *mqtt_topic = cJSON_GetObjectItemCaseSensitive(j, "mqtt_topic");
    cJSON *mqtt_id = cJSON_GetObjectItemCaseSensitive(j, "mqtt_client_id");
    cJSON *presence_topic = cJSON_GetObjectItemCaseSensitive(j, "presence_topic");
    cJSON *presence_absence = cJSON_GetObjectItemCaseSensitive(j, "presence_absence_s");
    cJSON *ble_filter = cJSON_GetObjectItemCaseSensitive(j, "ble_address_filter");
    if (ble_filter && (!cJSON_IsNumber(ble_filter) || ble_filter->valuedouble < 0 ||
        ble_filter->valuedouble > 2 || ble_filter->valuedouble != (int)ble_filter->valuedouble)) {
        cJSON_Delete(j); send_json(req, 400, "{\"ok\":false,\"error\":\"ble_filter_invalid\"}"); return ESP_OK;
    }
    if ((mqtt_port && (!cJSON_IsNumber(mqtt_port) || mqtt_port->valuedouble < 1 || mqtt_port->valuedouble > 65535 || mqtt_port->valuedouble != (int)mqtt_port->valuedouble)) ||
        (cJSON_IsString(mqtt_user) && strlen(mqtt_user->valuestring) >= CFG_MQTT_USER_MAX) ||
        (cJSON_IsString(mqtt_pass) && strlen(mqtt_pass->valuestring) >= CFG_MQTT_PASS_MAX) ||
        (cJSON_IsString(mqtt_broker) && strlen(mqtt_broker->valuestring) >= CFG_BROKER_MAX) ||
        (cJSON_IsString(mqtt_topic) && strlen(mqtt_topic->valuestring) >= CFG_MQTT_TOPIC_MAX) ||
        (cJSON_IsString(mqtt_id) && strlen(mqtt_id->valuestring) >= CFG_MQTT_ID_MAX)) {
        cJSON_Delete(j); send_json(req, 400, "{\"ok\":false,\"error\":\"mqtt_invalid\"}"); return ESP_OK;
    }
    if ((presence_topic || presence_absence) &&
        (!cJSON_IsString(presence_topic) || !presence_topic->valuestring[0] || strlen(presence_topic->valuestring) >= PRESENCE_TOPIC_MAX ||
         strpbrk(presence_topic->valuestring, "+#") || !cJSON_IsNumber(presence_absence) ||
         presence_absence->valuedouble < 30 || presence_absence->valuedouble > 65535 || presence_absence->valuedouble != (int)presence_absence->valuedouble ||
         (cJSON_IsString(mqtt_topic) && strcmp(presence_topic->valuestring, mqtt_topic->valuestring) == 0))) {
        cJSON_Delete(j); send_json(req, 400, "{\"ok\":false,\"error\":\"presence_invalid\"}"); return ESP_OK;
    }
    cJSON *webpass = cJSON_GetObjectItemCaseSensitive(j, "web_pass");
    if (cJSON_IsString(webpass) && webpass->valuestring[0] && strlen(webpass->valuestring) < 8) {
        cJSON_Delete(j); send_json(req, 400, "{\"ok\":false,\"error\":\"bad_web_credentials\"}"); return ESP_OK;
    }

    const app_config_t *c = config_get();
    bool ap_changed = false;

    /* wifi */
    cJSON *o;
    if ((o = cJSON_GetObjectItemCaseSensitive(j, "wifi_ssid")) && cJSON_IsString(o)) {
        config_set_wifi(o->valuestring, NULL);
    }
    if ((o = cJSON_GetObjectItemCaseSensitive(j, "wifi_pass")) && cJSON_IsString(o)
        && o->valuestring[0]) {
        config_set_wifi(NULL, o->valuestring);
    }
    /* ap */
    {
        cJSON *ss = cJSON_GetObjectItemCaseSensitive(j, "ap_ssid");
        cJSON *sp = cJSON_GetObjectItemCaseSensitive(j, "ap_pass");
        const char *ssv = (ss && cJSON_IsString(ss)) ? ss->valuestring : NULL;
        const char *spv = (sp && cJSON_IsString(sp)) ? sp->valuestring : NULL;
        ap_changed = (ssv && ssv[0] && strcmp(ssv, c->ap_ssid) != 0) ||
                     (spv && spv[0] && strcmp(spv, c->ap_password) != 0);
        if (ap_changed && config_set_ap(ssv, spv) != ESP_OK) {
            cJSON_Delete(j);
            send_json(req, 400, "{\"ok\":false,\"error\":\"invalid_ap_config\"}");
            return ESP_OK;
        }
    }
    /* wigle */
    {
        cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "wigle_user");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "wigle_token");
        const char *uv = (u && cJSON_IsString(u)) ? u->valuestring : NULL;
        const char *tv = (t && cJSON_IsString(t)) ? t->valuestring : NULL;
        if (uv || tv) {
            esp_err_t cred_err = config_set_credentials(uv, tv);
            if (cred_err != ESP_OK) {
                cJSON_Delete(j);
                send_json(req, cred_err == ESP_ERR_INVALID_ARG ? 400 : 500,
                          cred_err == ESP_ERR_INVALID_ARG ? "{\"ok\":false,\"error\":\"wigle_credentials_too_long\"}" : "{\"ok\":false,\"error\":\"wigle_credentials_save_failed\"}");
                return ESP_OK;
            }
        }
    }
    /* webui : changement des identifiants de connexion */
    {
        cJSON *wu = cJSON_GetObjectItemCaseSensitive(j, "web_user");
        cJSON *wp = cJSON_GetObjectItemCaseSensitive(j, "web_pass");
        const char *wuv = (wu && cJSON_IsString(wu) && wu->valuestring[0]) ? wu->valuestring : NULL;
        const char *wpv = (wp && cJSON_IsString(wp) && wp->valuestring[0]) ? wp->valuestring : NULL;
        if (wpv) {
            if (config_set_web_auth(wuv, wpv) != ESP_OK) {
                cJSON_Delete(j);
                send_json(req, 400, "{\"ok\":false,\"error\":\"bad_web_credentials\"}");
                return ESP_OK;
            }
        }
    }
    /* localisation */
    {
        cJSON *la = cJSON_GetObjectItemCaseSensitive(j, "lat");
        cJSON *lo = cJSON_GetObjectItemCaseSensitive(j, "lon");
        if (la && lo && cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
            if (config_set_location((float)la->valuedouble, (float)lo->valuedouble) != ESP_OK) {
                cJSON_Delete(j);
                send_json(req, 400, "{\"ok\":false,\"error\":\"bad_location\"}");
                return ESP_OK;
            }
        }
    }
    /* options */
    {
        cJSON *sa = cJSON_GetObjectItemCaseSensitive(j, "scan_autostart");
        if (sa && (!cJSON_IsNumber(sa) || (sa->valueint != 0 && sa->valueint != 1) ||
                   config_set_scan_autostart((uint8_t)sa->valueint) != ESP_OK)) {
            cJSON_Delete(j);
            send_json(req, 400, "{\"ok\":false,\"error\":\"scan_autostart\"}");
            return ESP_OK;
        }
        cJSON *mr = cJSON_GetObjectItemCaseSensitive(j, "min_rssi");
        cJSON *mm = cJSON_GetObjectItemCaseSensitive(j, "mqtt_min_rssi");
        cJSON *rd = cJSON_GetObjectItemCaseSensitive(j, "rediscover_min");
        cJSON *sw = cJSON_GetObjectItemCaseSensitive(j, "scan_wifi");
        cJSON *sb = cJSON_GetObjectItemCaseSensitive(j, "scan_ble");
        cJSON *lg = cJSON_GetObjectItemCaseSensitive(j, "ui_lang");
        if (mr && mm && rd && sw && sb && lg &&
            cJSON_IsNumber(mr) && cJSON_IsNumber(mm) && cJSON_IsNumber(rd) &&
            cJSON_IsNumber(sw) && cJSON_IsNumber(sb) && cJSON_IsNumber(lg)) {
            config_set_options2((int8_t)(int)mr->valuedouble,
                                (int8_t)(int)mm->valuedouble,
                                (uint16_t)(int)rd->valuedouble,
                                (uint8_t)(int)sw->valuedouble,
                                (uint8_t)(int)sb->valuedouble,
                                (uint8_t)(int)lg->valuedouble);
        }
    }
    if (ble_filter && config_set_ble_address_filter((uint8_t)ble_filter->valueint) != ESP_OK) {
        cJSON_Delete(j); send_json(req, 500, "{\"ok\":false,\"error\":\"ble_filter_save_failed\"}"); return ESP_OK;
    }
    /* mqtt */
    {
        cJSON *en = cJSON_GetObjectItemCaseSensitive(j, "mqtt_enabled");
        cJSON *br = cJSON_GetObjectItemCaseSensitive(j, "mqtt_broker");
        cJSON *po = cJSON_GetObjectItemCaseSensitive(j, "mqtt_port");
        cJSON *us = cJSON_GetObjectItemCaseSensitive(j, "mqtt_user");
        cJSON *pa = cJSON_GetObjectItemCaseSensitive(j, "mqtt_pass");
        cJSON *to = cJSON_GetObjectItemCaseSensitive(j, "mqtt_topic");
        cJSON *ci = cJSON_GetObjectItemCaseSensitive(j, "mqtt_client_id");
        cJSON *tl = cJSON_GetObjectItemCaseSensitive(j, "mqtt_tls");
        if (en && br && po && us && pa && to && ci && tl &&
            cJSON_IsNumber(en) && cJSON_IsString(br) && cJSON_IsNumber(po) &&
            cJSON_IsString(us) && cJSON_IsString(pa) && cJSON_IsString(to) &&
            cJSON_IsString(ci) && cJSON_IsNumber(tl)) {
            esp_err_t e = config_set_mqtt((uint8_t)(int)en->valuedouble,
                                          br->valuestring,
                                          (uint16_t)(int)po->valuedouble,
                                          us->valuestring,
                                          pa->valuestring,
                                          to->valuestring,
                                          ci->valuestring,
                                          (uint8_t)(int)tl->valuedouble);
            if (e != ESP_OK) {
                cJSON_Delete(j); send_json(req, 500, "{\"ok\":false,\"error\":\"mqtt_save_failed\"}"); return ESP_OK;
            }
            /* appliquer sans redemarrer */
            mqtt_stop();
            esp_err_t me = mqtt_init();
            if (me == ESP_OK) mqtt_start();
            else ESP_LOGW(TAG, "MQTT configuration enregistree mais demarrage impossible: %s", esp_err_to_name(me));
        }
    }
    /* time */
    if (presence_topic && presence_absence && presence_settings(presence_topic->valuestring, (unsigned)presence_absence->valueint) != ESP_OK) {
        cJSON_Delete(j); send_json(req, 500, "{\"ok\":false,\"error\":\"presence_save_failed\"}"); return ESP_OK;
    }
    {
        cJSON *ns = cJSON_GetObjectItemCaseSensitive(j, "ntp_server");
        cJSON *tz = cJSON_GetObjectItemCaseSensitive(j, "tz");
        const char *nsv = (ns && cJSON_IsString(ns)) ? ns->valuestring : NULL;
        const char *tzv = (tz && cJSON_IsString(tz)) ? tz->valuestring : NULL;
        if (nsv || tzv) {
            config_set_time(nsv, tzv);
            net_apply_time_config();
        }
    }
    /* device id */
    {
        cJSON *di = cJSON_GetObjectItemCaseSensitive(j, "device_id");
        if (di && cJSON_IsString(di)) config_set_device_id(di->valuestring);
    }

    cJSON_Delete(j);
    send_json(req, 200, ap_changed ? "{\"ok\":true,\"ap_changed\":true}" : "{\"ok\":true}");
    if (ap_changed && xTaskCreate(apply_ap_later, "apply_ap", 3072, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "application AP differee impossible ; redemarrage necessaire");
    return ESP_OK;
}

/* --- scan --- */

static esp_err_t h_scan_start(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    if (!scanner_is_running()) {
        esp_err_t err = scanner_start();
        if (err != ESP_OK) { send_json(req, 500, "{\"ok\":false,\"error\":\"scan_start_failed\"}"); return ESP_OK; }
    }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_scan_stop(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    esp_err_t err = scanner_stop();
    if (err != ESP_OK) { send_json(req, 500, "{\"ok\":false,\"error\":\"scan_stop_failed\"}"); return ESP_OK; }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_scan_status(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char out[960], ip[16] = "", ts[24] = "", sync[24] = "";
    char ts_local[24] = "", sync_local[24] = "";
    uint32_t wifi = 0, ble = 0;
    char wifi_json[16] = "null", ble_json[16] = "null";
    size_t total = 0, freeb = 0;
    net_get_ip(ip, sizeof(ip));
    net_time_utc(ts, sizeof(ts));
    net_last_sync(sync, sizeof(sync));
    if (!net_time_local(ts_local, sizeof(ts_local))) strlcpy(ts_local, ts, sizeof(ts_local));
    if (!net_last_sync_local(sync_local, sizeof(sync_local))) strlcpy(sync_local, sync, sizeof(sync_local));
    if (store_get_counts(&wifi, &ble)) {
        snprintf(wifi_json, sizeof(wifi_json), "%lu", (unsigned long)wifi);
        snprintf(ble_json, sizeof(ble_json), "%lu", (unsigned long)ble);
    }
    bool store_ok = store_stats(&total, &freeb, NULL) == ESP_OK;
    scanner_wifi_diagnostics_t diag = {0};
    scanner_get_wifi_diagnostics(&diag);
    scanner_ble_diagnostics_t bd = {0}; scanner_get_ble_diagnostics(&bd);
    int64_t age = diag.attempts ? (esp_timer_get_time() - diag.completed_us) / 1000000 : -1;
    snprintf(out, sizeof(out),
             "{\"mode\":\"%s\",\"ip\":\"%s\",\"ts\":\"%s\",\"ts_local\":\"%s\",\"tsq\":\"%s\","
             "\"version\":\"" RFS_VERSION "\","
             "\"last_sync\":\"%s\",\"last_sync_local\":\"%s\",\"wifi\":%s,\"ble\":%s,\"running\":%s,"
             "\"store_total\":%lu,\"store_used\":%lu,\"store_ok\":%s,\"mqtt_connected\":%s,"
             "\"wifi_scan_attempts\":%lu,\"wifi_scan_failures\":%lu,\"wifi_scan_age\":%lld,"
             "\"wifi_scan_channel\":%u,\"wifi_scan_first\":%u,\"wifi_scan_channel_count\":%u,"
             "\"wifi_scan_found\":%lu,\"wifi_scan_saved\":%lu,\"wifi_scan_error\":%ld,"
             "\"ble_public\":%lu,\"ble_static\":%lu,\"ble_rpa\":%lu,\"ble_nrpa\":%lu,\"ble_unknown\":%lu}",
             net_get_mode(), ip, ts, ts_local, net_time_quality_str(), sync, sync_local,
             wifi_json, ble_json, scanner_is_running() ? "true" : "false",
             (unsigned long)total, (unsigned long)(total - freeb), store_ok ? "true" : "false",
             mqtt_is_connected() ? "true" : "false",
             (unsigned long)diag.attempts, (unsigned long)diag.failures, (long long)age,
             (unsigned)diag.channel, (unsigned)diag.first_channel, (unsigned)diag.channel_count,
             (unsigned long)diag.found, (unsigned long)diag.saved, (long)diag.last_error,
             (unsigned long)bd.public_count, (unsigned long)bd.static_count, (unsigned long)bd.rpa_count,
             (unsigned long)bd.nrpa_count, (unsigned long)bd.unknown_count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* --- observations --- */

static esp_err_t h_observations(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }

    int page = 1;
    char type[8] = "";
    char sort[8] = "";
    char dir[8] = "";
    char cursor[48] = "";
    char qs[256] = {0};
    rfs_list_opt_t opt = { .from = 0, .to = 0 };
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char val[24] = {0};
        if (httpd_query_key_value(qs, "page", val, sizeof(val)) == ESP_OK) {
            page = atoi(val);
        }
        httpd_query_key_value(qs, "type", type, sizeof(type));
        httpd_query_key_value(qs, "sort", sort, sizeof(sort));
        httpd_query_key_value(qs, "dir", dir, sizeof(dir));
        httpd_query_key_value(qs, "cursor", cursor, sizeof(cursor));
        char bound[24] = {0};
        if (httpd_query_key_value(qs, "from", bound, sizeof(bound)) == ESP_OK) opt.from = (time_t)strtoll(bound, NULL, 10);
        if (httpd_query_key_value(qs, "to", bound, sizeof(bound)) == ESP_OK) opt.to = (time_t)strtoll(bound, NULL, 10);
    }
    if (type[0] && strcmp(type, "wifi") != 0 && strcmp(type, "ble") != 0) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"bad_type\"}");
        return ESP_OK;
    }
    if (page < 1) page = 1;
    opt.sort = sort;
    opt.dir = dir;
    opt.cursor = cursor;

    size_t outsz = RFS_OBS_BUF;
    char *out = malloc(outsz);
    if (!out) {
        outsz = RFS_OBS_FALLBACK_BUF;
        out = malloc(outsz);
        if (out) ESP_LOGW(TAG, "observations: tampon reduit a %u octets", (unsigned)outsz);
    }
    if (!out) { send_json(req, 400, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }

    esp_err_t err = store_list(page, type, &opt, out, outsz);
    if (err != ESP_OK) {
        free(out);
        send_json(req, 500, err == ESP_ERR_NO_MEM ? "{\"ok\":false,\"error\":\"oom\"}" : "{\"ok\":false,\"error\":\"store\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

static esp_err_t h_history(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }

    char qs[96] = {0};
    char mac[32] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "mac", mac, sizeof(mac));
        qs_mac(mac);
    }
    if (!mac[0]) { send_json(req, 400, "{\"ok\":false,\"error\":\"missing_mac\"}"); return ESP_OK; }

    char *out = malloc(RFS_HIST_BUF);
    if (!out) { send_json(req, 400, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }

    esp_err_t err = store_history(mac, out, RFS_HIST_BUF);
    if (err != ESP_OK) {
        free(out);
        send_json(req, 500, "{\"ok\":false,\"error\":\"store\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

/* --- blacklist --- */

static esp_err_t h_blacklist_add(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char *body = read_body(req);
    if (!body) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_json\"}"); return ESP_OK; }
    cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mac");
    cJSON *l = cJSON_GetObjectItemCaseSensitive(j, "label");
    if (!m || !cJSON_IsString(m) || !m->valuestring[0]) {
        cJSON_Delete(j);
        send_json(req, 400, "{\"ok\":false,\"error\":\"missing_mac\"}");
        return ESP_OK;
    }
    esp_err_t e = store_blacklist_add(m->valuestring,
                                      (l && cJSON_IsString(l)) ? l->valuestring : NULL);
    cJSON_Delete(j);
    send_json(req, (e == ESP_OK) ? 200 : 400,
              e == ESP_OK ? "{\"ok\":true}" :
              e == ESP_ERR_INVALID_ARG ? "{\"ok\":false,\"error\":\"invalid_mac\"}" :
              e == ESP_ERR_NO_MEM ? "{\"ok\":false,\"error\":\"blacklist_full\"}" : "{\"ok\":false,\"error\":\"nvs\"}");
    return ESP_OK;
}

static esp_err_t h_blacklist_remove(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char *body = read_body(req);
    if (!body) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_json\"}"); return ESP_OK; }
    cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mac");
    if (!m || !cJSON_IsString(m) || !m->valuestring[0]) {
        cJSON_Delete(j);
        send_json(req, 400, "{\"ok\":false,\"error\":\"missing_mac\"}");
        return ESP_OK;
    }
    esp_err_t e = store_blacklist_remove(m->valuestring);
    cJSON_Delete(j);
    send_json(req, (e == ESP_OK) ? 200 : 500,
              e == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"nvs\"}");
    return ESP_OK;
}

static esp_err_t h_blacklist_list(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char *out = malloc(8192);
    if (!out) { send_json(req, 400, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }
    esp_err_t e = store_blacklist_list(out, 8192);
    if (e != ESP_OK) {
        free(out);
        send_json(req, 500, "{\"ok\":false,\"error\":\"nvs\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

/* --- wigle --- */

static portMUX_TYPE s_upload_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_upload_running = false;
static bool s_upload_started = false;
static int s_upload_ok = 0, s_upload_failed = 0;
static char s_upload_error[192];

static void wigle_upload_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "WiGLE heap: libre=%u bloc_max=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    store_export_t *journal = NULL;
    esp_err_t err = store_export_open(&journal);
    char result[256] = "Journal inaccessible ou memoire insuffisante";
    bool attempted = false;
    if (err == ESP_OK && store_export_has_data(journal)) {
        attempted = true;
        err = wigle_upload_journal(journal, "rfsentinel_journal.csv", result, sizeof(result));
    }
    store_export_close(journal);
    portENTER_CRITICAL(&s_upload_lock);
    if (err != ESP_OK) { s_upload_failed = 1; strlcpy(s_upload_error, result, sizeof(s_upload_error)); }
    else if (attempted) s_upload_ok = 1;
    portEXIT_CRITICAL(&s_upload_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "WiGLE: %s (heap=%u bloc_max=%u)", result,
                               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    else if (attempted) ESP_LOGI(TAG, "WiGLE: journal consolide accepte");
    portENTER_CRITICAL(&s_upload_lock);
    s_upload_running = false;
    portEXIT_CRITICAL(&s_upload_lock);
    vTaskDelete(NULL);
}

static esp_err_t h_wigle_upload(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    const app_config_t *c = config_get();
    if (!c->wigle_user[0] || !c->wigle_token[0]) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"no_credentials\"}"); return ESP_OK;
    }
    if (!net_is_connected()) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"no_network\"}"); return ESP_OK;
    }
    if (!store_ready()) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"no_journal\"}"); return ESP_OK;
    }
    portENTER_CRITICAL(&s_upload_lock);
    bool busy = s_upload_running;
    if (!busy) {
        s_upload_running = true; s_upload_started = true;
        s_upload_ok = 0; s_upload_failed = 0; s_upload_error[0] = '\0';
    }
    portEXIT_CRITICAL(&s_upload_lock);
    if (busy) { send_json(req, 400, "{\"ok\":false,\"error\":\"upload_running\"}"); return ESP_OK; }
    if (xTaskCreate(wigle_upload_task, "rfs_wigle", 8192, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_upload_lock);
        s_upload_running = false; s_upload_failed = 1;
        strlcpy(s_upload_error, "Memoire insuffisante", sizeof(s_upload_error));
        portEXIT_CRITICAL(&s_upload_lock);
        send_json(req, 500, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK;
    }
    send_json(req, 200, "{\"ok\":true,\"running\":true}");
    return ESP_OK;
}

static esp_err_t h_wigle_upload_status(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    bool running, started;
    int uploaded, failed;
    char error[192], escaped[384], response[512];
    portENTER_CRITICAL(&s_upload_lock);
    running = s_upload_running; started = s_upload_started;
    uploaded = s_upload_ok; failed = s_upload_failed;
    strlcpy(error, s_upload_error, sizeof(error));
    portEXIT_CRITICAL(&s_upload_lock);
    json_escape(error, escaped, sizeof(escaped));
    snprintf(response, sizeof(response), "{\"ok\":true,\"started\":%s,\"running\":%s,\"uploaded\":%d,\"failed\":%d,\"last_error\":\"%s\"}",
             started ? "true" : "false", running ? "true" : "false", uploaded, failed, escaped);
    send_json(req, 200, response);
    return ESP_OK;
}

/* --- export du journal complet --- */

/* Le nom du fichier reprend l'heure courante, assainie : Content-Disposition ne
   doit contenir que des caracteres sans risque pour les navigateurs. */
static void export_filename(char *out, size_t n) {
    char ts[32];
    if (!net_time_utc(ts, sizeof(ts))) strlcpy(ts, "0", sizeof(ts));
    static const char prefix[] = "rfsentinel_journal_";
    static const char suffix[] = ".csv";
    size_t o = 0;
    for (size_t i = 0; prefix[i] && o + 1 < n; i++) out[o++] = prefix[i];
    for (size_t i = 0; ts[i] && o + sizeof(suffix) < n; i++) {
        char c = ts[i];
        if (c == ':') c = '-';
        else if (c == ' ') c = '_';
        else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) continue;
        out[o++] = c;
    }
    for (size_t i = 0; suffix[i] && o + 1 < n; i++) out[o++] = suffix[i];
    out[o] = '\0';
}

/* Le flux part par morceaux, donc l'en-tete est emis avant la derniere octet et
   aucune taille totale ne peut etre annoncee. Entre chaque bloc la tache du
   serveur web rend la main : l'enregistrement des observations, les taches de
   fond et la surveillance du watchdog continuent de tourner pendant le
   telechargement, qui peut durer plusieurs secondes sur un journal bien rempli. */
#define RFS_EXPORT_BUF 4096

static esp_err_t h_export(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }

    char qs[96], vfrom[24], vto[24];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
        httpd_query_key_value(qs, "from", vfrom, sizeof(vfrom)) != ESP_OK ||
        httpd_query_key_value(qs, "to", vto, sizeof(vto)) != ESP_OK) {
        send_json(req, 400, "{\"error\":\"bad_dates\"}"); return ESP_OK;
    }
    char *end1, *end2;
    errno = 0;
    long long a = strtoll(vfrom, &end1, 10), b = strtoll(vto, &end2, 10);
    if (errno == ERANGE || !*vfrom || *end1 || !*vto || *end2 || a <= 0 || b < a || (time_t)a != a || (time_t)b != b) {
        send_json(req, 400, "{\"error\":\"bad_dates\"}"); return ESP_OK;
    }
    store_export_t *h = NULL;
    esp_err_t err = store_export_open(&h);
    if (err != ESP_OK || !h) {
        ESP_LOGW(TAG, "ouverture export journal: %s", esp_err_to_name(err));
        store_export_close(h);
        send_err(req, "export_open_failed");
        return ESP_OK;
    }

    long total = store_export_size(h);
    char name[80];
    export_filename(name, sizeof(name));
    char disp[128];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buf = malloc(RFS_EXPORT_BUF);
    if (!buf) { store_export_close(h); send_err(req, "oom"); return ESP_OK; }

    long sent = 0;
    char line[512];
    char *output = malloc(RFS_EXPORT_BUF);
    if (!output) { free(buf); store_export_close(h); send_err(req, "oom"); return ESP_OK; }
    size_t llen = 0, used = 0;
    unsigned line_no = 0;
    bool in_quotes = false;
    while (true) {
        size_t got = 0;
        err = store_export_read(h, buf, RFS_EXPORT_BUF, &got);
        if (err != ESP_OK || got == 0) break;
        for (size_t i = 0; i < got; i++) {
            if (llen + 1 >= sizeof(line)) { err = ESP_FAIL; break; }
            line[llen++] = buf[i];
            if (buf[i] == '"') in_quotes = !in_quotes;
            if (buf[i] != '\n' || in_quotes) continue;
            line[llen] = 0;
            bool keep = line_no++ < 2;
            if (!keep) {
                err = store_export_record_in_range(line, (time_t)a, (time_t)b, &keep);
                if (err != ESP_OK) break;
            }
            if (keep) {
                if (used + llen > RFS_EXPORT_BUF) {
                    err = httpd_resp_send_chunk(req, output, used);
                    if (err != ESP_OK) break;
                    sent += used; used = 0;
                }
                memcpy(output + used, line, llen); used += llen;
            }
            llen = 0;
        }
        if (err != ESP_OK) break;
        vTaskDelay(1);
    }
    if (err == ESP_OK && (llen || in_quotes)) err = ESP_FAIL;
    if (err == ESP_OK && used) { err = httpd_resp_send_chunk(req, output, used); sent += used; }
    free(output);
    free(buf);
    store_export_close(h);

    if (err != ESP_OK) {
        /* L'en-tete est deja parti : le navigateur recoit un fichier tronque,
           seul le journal serial permet de le savoir. */
        ESP_LOGW(TAG, "export journal interrompu: %ld/%ld octets, %s",
                 sent, total, esp_err_to_name(err));
        return err;
    }

    err = httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "export journal: %ld/%ld octets, %s", sent, total, esp_err_to_name(err));
    return err;
}

/* --- time --- */

static esp_err_t h_time(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char ts[32], ls[32], tsl[32], lsl[32];
    bool sync = net_time_utc(ts, sizeof(ts));
    if (!sync) strlcpy(ts, "0", sizeof(ts));
    net_last_sync(ls, sizeof(ls));
    if (!net_time_local(tsl, sizeof(tsl))) strlcpy(tsl, ts, sizeof(tsl));
    if (!net_last_sync_local(lsl, sizeof(lsl))) strlcpy(lsl, ls, sizeof(lsl));
    char out[256];
    snprintf(out, sizeof(out),
             "{\"ts\":\"%s\",\"ts_local\":\"%s\",\"tsq\":\"%s\",\"last_sync\":\"%s\",\"last_sync_local\":\"%s\",\"mode\":\"%s\"}",
             ts, tsl, net_time_quality_str(), ls, lsl, net_get_mode());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t h_time_set(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char *body = read_body(req);
    if (!body) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_json\"}"); return ESP_OK; }
    cJSON *epoch = cJSON_GetObjectItemCaseSensitive(j, "epoch");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsNumber(epoch) && isfinite(epoch->valuedouble) &&
        epoch->valuedouble >= 1704067200.0 && epoch->valuedouble <= 4102444799.0)
        err = net_set_time_from_client((time_t)epoch->valuedouble);
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_STATE) send_json(req, 400, "{\"ok\":false,\"error\":\"ntp_active\"}");
    else if (err != ESP_OK) send_json(req, 400, "{\"ok\":false,\"error\":\"invalid_time\"}");
    else send_json(req, 200, "{\"ok\":true,\"tsq\":\"ESTIMATED\"}");
    return ESP_OK;
}

/* --- wigle / purge --- */

static esp_err_t h_wigle(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }

    char qs[128] = {0};
    char mac[32] = {0};
    char type[8] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "mac", mac, sizeof(mac));
        httpd_query_key_value(qs, "type", type, sizeof(type));
        qs_mac(mac);
    }
    if (!mac[0]) { send_json(req, 400, "{\"ok\":false,\"error\":\"missing_mac\"}"); return ESP_OK; }

    char *out = s_wigle_workspace;
    ESP_LOGI(TAG, "WiGLE tampon reserve: libre=%u bloc_max=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* WiGLE : liste des presences connues de cet equipement. Le corps renvoye
       porte deja count, truncated et eventuellement un code d'erreur, donc la
       page peut afficher la cause au lieu d'un simple "ca n'a pas marche". */
    wigle_search_device(mac, type, out, RFS_WIGLE_BUF);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    ESP_LOGI(TAG, "WiGLE reponse envoyee: libre=%u bloc_max=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

static esp_err_t h_purge(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }

    char *body = read_body(req);
    if (!body) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_json\"}"); return ESP_OK; }

    cJSON *jf = cJSON_GetObjectItemCaseSensitive(j, "from");
    cJSON *jt = cJSON_GetObjectItemCaseSensitive(j, "to");
    if (!jf || !jt || !cJSON_IsNumber(jf) || !cJSON_IsNumber(jt) ||
        jf->valuedouble <= 0 || jt->valuedouble <= 0 ||
        jf->valuedouble >= jt->valuedouble) {
        cJSON_Delete(j);
        send_json(req, 400, "{\"ok\":false,\"error\":\"bad_range\"}");
        return ESP_OK;
    }

    purge_args_t *args = malloc(sizeof(*args));
    if (!args) { cJSON_Delete(j); send_json(req, 500, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }
    args->from = (time_t)jf->valuedouble;
    args->to = (time_t)jt->valuedouble;
    cJSON_Delete(j);
    portENTER_CRITICAL(&s_purge_lock);
    bool busy = s_purge_running;
    if (!busy) {
        s_purge_running = true;
        s_purge_removed = 0;
        s_purge_result = ESP_OK;
    }
    portEXIT_CRITICAL(&s_purge_lock);
    if (busy) { free(args); send_json(req, 400, "{\"ok\":false,\"error\":\"purge_running\"}"); return ESP_OK; }
    if (xTaskCreate(purge_task, "rfs_purge", 6144, args, 3, NULL) != pdPASS) {
        free(args);
        portENTER_CRITICAL(&s_purge_lock);
        s_purge_running = false;
        s_purge_result = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_purge_lock);
        send_json(req, 500, "{\"ok\":false,\"error\":\"oom\"}");
        return ESP_OK;
    }
    send_json(req, 200, "{\"ok\":true,\"running\":true}");
    return ESP_OK;
}

static esp_err_t h_purge_status(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    portENTER_CRITICAL(&s_purge_lock);
    bool running = s_purge_running;
    int removed = s_purge_removed;
    esp_err_t result = s_purge_result;
    portEXIT_CRITICAL(&s_purge_lock);
    char out[120];
    snprintf(out, sizeof(out), "{\"ok\":true,\"running\":%s,\"removed\":%d,\"error\":%s}",
             running ? "true" : "false", removed, result == ESP_OK ? "null" : "\"store\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t h_calibration_get(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    scanner_calibration_t c; scanner_calibration_status(&c);
    long age = c.has_sample ? (long)((esp_timer_get_time() - c.last_sample_us) / 1000000) : -1;
    char out[384];
    snprintf(out, sizeof(out), "{\"ok\":true,\"active\":%s,\"stopping\":%s,\"has_sample\":%s,"
             "\"radio_error\":%s,\"resume_error\":%s,\"mac\":\"%s\",\"kind\":\"%s\","
             "\"rssi\":%d,\"minimum\":%d,\"samples\":%lu,\"age\":%ld}",
             c.active ? "true" : "false", c.stop_requested ? "true" : "false", c.has_sample ? "true" : "false",
             c.radio_error ? "true" : "false", c.resume_error ? "true" : "false", c.mac, c.kind,
             c.rssi, c.minimum, (unsigned long)c.samples, age);
    send_json(req, 200, out);
    return ESP_OK;
}

static esp_err_t h_find_get(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    scanner_find_result_t *snapshot = malloc(sizeof(*snapshot));
    char *out = malloc(8192);
    if (!snapshot || !out) {
        free(snapshot); free(out); send_json(req, 500, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK;
    }
    scanner_find_status(snapshot);
    size_t used = (size_t)snprintf(out, 8192, "{\"ok\":true,\"stage\":%u,\"overflow\":%s,\"candidates\":[",
                                   snapshot->stage, snapshot->overflow ? "true" : "false");
    int64_t now = esp_timer_get_time();
    for (uint32_t i = 0; i < snapshot->count; i++) {
        scanner_find_entry_t *e = &snapshot->entries[i];
        int a = e->counts[0] ? (int)(e->sums[0] / (int32_t)e->counts[0]) : 0;
        int b = e->counts[1] ? (int)(e->sums[1] / (int32_t)e->counts[1]) : 0;
        int c = e->counts[2] ? (int)(e->sums[2] / (int32_t)e->counts[2]) : 0;
        int w = snprintf(out + used, 8192 - used,
            "%s{\"mac\":\"%s\",\"near1\":%d,\"far\":%d,\"near2\":%d,\"counts\":[%lu,%lu,%lu],\"rssi\":%d,\"age\":%ld}",
            i ? "," : "", e->mac, a, b, c, (unsigned long)e->counts[0], (unsigned long)e->counts[1],
            (unsigned long)e->counts[2], e->rssi, (long)((now - e->last_sample_us) / 1000000));
        if (w < 0 || (size_t)w >= 8192 - used - 4) {
            free(snapshot); free(out); send_json(req, 500, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK;
        }
        used += (size_t)w;
    }
    snprintf(out + used, 8192 - used, "]}");
    free(snapshot); send_json(req, 200, out); free(out);
    return ESP_OK;
}

static esp_err_t h_presence_get(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    presence_config_t *p = malloc(sizeof(*p)); char *out = malloc(4096);
    if (!p || !out) { free(p); free(out); send_json(req, 500, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }
    if (presence_get(p) != ESP_OK) { free(p); free(out); send_json(req, 500, "{\"ok\":false,\"error\":\"presence_unavailable\"}"); return ESP_OK; }
    char topic[PRESENCE_TOPIC_MAX * 2 + 3]; json_escape(p->topic, topic, sizeof(topic));
    size_t used = (size_t)snprintf(out, 4096, "{\"ok\":true,\"topic\":\"%s\",\"absence_s\":%u,\"items\":[", topic, (unsigned)p->absence_s);
    for (unsigned i = 0; i < p->count; i++) {
        char label[PRESENCE_LABEL_MAX * 2 + 3]; json_escape(p->items[i].label, label, sizeof(label));
        int n = snprintf(out + used, 4096 - used, "%s{\"mac\":\"%s\",\"label\":\"%s\"}", i ? "," : "", p->items[i].mac, label);
        if (n < 0 || (size_t)n >= 4096 - used - 4) { free(p); free(out); send_json(req, 500, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }
        used += (size_t)n;
    }
    snprintf(out + used, 4096 - used, "]}");
    free(p); send_json(req, 200, out); free(out); return ESP_OK;
}

static esp_err_t h_presence_post(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char *body = read_body(req);
    if (!body) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_json\"}"); return ESP_OK; }
    cJSON *action = cJSON_GetObjectItemCaseSensitive(j, "action");
    cJSON *mac = cJSON_GetObjectItemCaseSensitive(j, "mac");
    cJSON *label = cJSON_GetObjectItemCaseSensitive(j, "label");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(action) && cJSON_IsString(mac)) {
        if (!strcmp(action->valuestring, "add") && (!label || cJSON_IsString(label)))
            err = presence_add(mac->valuestring, label ? label->valuestring : "");
        else if (!strcmp(action->valuestring, "remove")) err = presence_remove(mac->valuestring);
    }
    cJSON_Delete(j);
    if (err != ESP_OK) { send_json(req, err == ESP_ERR_INVALID_ARG ? 400 : 500, "{\"ok\":false,\"error\":\"presence_save_failed\"}"); return ESP_OK; }
    send_json(req, 200, "{\"ok\":true}"); return ESP_OK;
}

static esp_err_t h_calibration_post(httpd_req_t *req) {
    if (!req_authorized(req)) { send_err(req, "unauthorized"); return ESP_OK; }
    char *body = read_body(req);
    if (!body) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_request\"}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_json(req, 400, "{\"ok\":false,\"error\":\"bad_json\"}"); return ESP_OK; }
    cJSON *action = cJSON_GetObjectItemCaseSensitive(j, "action");
    cJSON *mac = cJSON_GetObjectItemCaseSensitive(j, "mac");
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(j, "kind");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(action) && strcmp(action->valuestring, "start") == 0 && cJSON_IsString(mac) && cJSON_IsString(kind))
        err = scanner_calibration_start(mac->valuestring, kind->valuestring);
    else if (cJSON_IsString(action) && strcmp(action->valuestring, "find_start") == 0)
        err = scanner_find_start();
    else if (cJSON_IsString(action) && strcmp(action->valuestring, "find_next") == 0)
        err = scanner_find_next();
    else if (cJSON_IsString(action) && strcmp(action->valuestring, "stop") == 0)
        err = scanner_calibration_stop();
    else if (cJSON_IsString(action) && strcmp(action->valuestring, "apply") == 0 && cJSON_IsString(kind)) {
        scanner_calibration_t c; scanner_calibration_status(&c);
        if (!c.active && c.has_sample && !c.radio_error && strcmp(c.kind, kind->valuestring) == 0)
            err = config_set_rssi_threshold(strcmp(c.kind, "alert") == 0, (int8_t)c.minimum);
        else err = ESP_ERR_INVALID_STATE;
    }
    cJSON_Delete(j);
    if (err != ESP_OK) {
        send_json(req, err == ESP_ERR_NO_MEM ? 500 : 400,
                  err == ESP_ERR_INVALID_ARG ? "{\"ok\":false,\"error\":\"bad_calibration\"}" :
                  err == ESP_ERR_NO_MEM ? "{\"ok\":false,\"error\":\"oom\"}" :
                  "{\"ok\":false,\"error\":\"calibration_unavailable\"}");
        return ESP_OK;
    }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/* --- init --- */

static const httpd_uri_t r = {
    .uri = "/", .method = HTTP_GET, .handler = h_index,
};

static const httpd_uri_t r_logo_logo = {
    .uri = "/logo/rfsentinel-logo.svg", .method = HTTP_GET, .handler = h_logo_logo,
};

static const httpd_uri_t r_logo_icon = {
    .uri = "/logo/rfsentinel-icon.svg", .method = HTTP_GET, .handler = h_logo_icon,
};

static const httpd_uri_t r_login = {
    .uri = "/api/login", .method = HTTP_POST, .handler = h_login,
};

static const httpd_uri_t r_logout = {
    .uri = "/api/logout", .method = HTTP_POST, .handler = h_logout,
};

static const httpd_uri_t r_config_get = {
    .uri = "/api/config", .method = HTTP_GET, .handler = h_config_get,
};

static const httpd_uri_t r_config_set = {
    .uri = "/api/config", .method = HTTP_POST, .handler = h_config_set,
};

static const httpd_uri_t r_scan_start = {
    .uri = "/api/scan/start", .method = HTTP_POST, .handler = h_scan_start,
};

static const httpd_uri_t r_scan_stop = {
    .uri = "/api/scan/stop", .method = HTTP_POST, .handler = h_scan_stop,
};

static const httpd_uri_t r_scan_status = {
    .uri = "/api/scan/status", .method = HTTP_GET, .handler = h_scan_status,
};

static const httpd_uri_t r_observations = {
    .uri = "/api/observations", .method = HTTP_GET, .handler = h_observations,
};

static const httpd_uri_t r_history = {
    .uri = "/api/history", .method = HTTP_GET, .handler = h_history,
};

static const httpd_uri_t r_blacklist_add = {
    .uri = "/api/blacklist/add", .method = HTTP_POST, .handler = h_blacklist_add,
};

static const httpd_uri_t r_blacklist_remove = {
    .uri = "/api/blacklist/remove", .method = HTTP_POST, .handler = h_blacklist_remove,
};

static const httpd_uri_t r_blacklist_list = {
    .uri = "/api/blacklist", .method = HTTP_GET, .handler = h_blacklist_list,
};

static const httpd_uri_t r_wigle_upload = {
    .uri = "/api/wigle/upload", .method = HTTP_POST, .handler = h_wigle_upload,
};

static const httpd_uri_t r_wigle_upload_status = {
    .uri = "/api/wigle/upload/status", .method = HTTP_GET, .handler = h_wigle_upload_status,
};

static const httpd_uri_t r_wigle = {
    .uri = "/api/wigle", .method = HTTP_GET, .handler = h_wigle,
};

static const httpd_uri_t r_purge = {
    .uri = "/api/purge", .method = HTTP_POST, .handler = h_purge,
};

static const httpd_uri_t r_purge_status = {
    .uri = "/api/purge/status", .method = HTTP_GET, .handler = h_purge_status,
};
static const httpd_uri_t r_calibration_get = {
    .uri = "/api/calibration", .method = HTTP_GET, .handler = h_calibration_get,
};
static const httpd_uri_t r_calibration_post = {
    .uri = "/api/calibration", .method = HTTP_POST, .handler = h_calibration_post,
};
static const httpd_uri_t r_find_get = {
    .uri = "/api/calibration/find", .method = HTTP_GET, .handler = h_find_get,
};
static const httpd_uri_t r_presence_get = { .uri = "/api/presence", .method = HTTP_GET, .handler = h_presence_get };
static const httpd_uri_t r_presence_post = { .uri = "/api/presence", .method = HTTP_POST, .handler = h_presence_post };

static const httpd_uri_t r_export = {
    .uri = "/api/export", .method = HTTP_GET, .handler = h_export,
};

static const httpd_uri_t r_time = {
    .uri = "/api/time", .method = HTTP_GET, .handler = h_time,
};

static const httpd_uri_t r_time_set = {
    .uri = "/api/time", .method = HTTP_POST, .handler = h_time_set,
};

esp_err_t web_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 29;
    cfg.stack_size = 8192;
    /* < CONFIG_LWIP_MAX_SOCKETS (12) : sinon httpd_start refuse de demarrer */
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.send_wait_timeout = 15;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    httpd_register_uri_handler(s_server, &r);
    httpd_register_uri_handler(s_server, &r_logo_logo);
    httpd_register_uri_handler(s_server, &r_logo_icon);
    httpd_register_uri_handler(s_server, &r_login);
    httpd_register_uri_handler(s_server, &r_logout);
    httpd_register_uri_handler(s_server, &r_config_get);
    httpd_register_uri_handler(s_server, &r_config_set);
    httpd_register_uri_handler(s_server, &r_scan_start);
    httpd_register_uri_handler(s_server, &r_scan_stop);
    httpd_register_uri_handler(s_server, &r_scan_status);
    httpd_register_uri_handler(s_server, &r_observations);
    httpd_register_uri_handler(s_server, &r_history);
    httpd_register_uri_handler(s_server, &r_blacklist_add);
    httpd_register_uri_handler(s_server, &r_blacklist_remove);
    httpd_register_uri_handler(s_server, &r_blacklist_list);
    httpd_register_uri_handler(s_server, &r_wigle_upload);
    httpd_register_uri_handler(s_server, &r_wigle_upload_status);
    httpd_register_uri_handler(s_server, &r_wigle);
    httpd_register_uri_handler(s_server, &r_purge);
    httpd_register_uri_handler(s_server, &r_purge_status);
    httpd_register_uri_handler(s_server, &r_calibration_get);
    httpd_register_uri_handler(s_server, &r_calibration_post);
    httpd_register_uri_handler(s_server, &r_find_get);
    httpd_register_uri_handler(s_server, &r_presence_get);
    httpd_register_uri_handler(s_server, &r_presence_post);
    httpd_register_uri_handler(s_server, &r_export);
    httpd_register_uri_handler(s_server, &r_time);
    httpd_register_uri_handler(s_server, &r_time_set);

    ESP_LOGI(TAG, "web: UI sur port 80");
    return ESP_OK;
}
