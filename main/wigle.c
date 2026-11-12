#include "wigle.h"
#include "config.h"
#include "store.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"


#define WIGLE_UPLOAD_URL "https://api.wigle.net/api/v2/file/upload"
#define WIGLE_NET_URL    "https://api.wigle.net/api/v2/network/search"
#define WIGLE_BT_URL     "https://api.wigle.net/api/v2/bluetooth/search"
#define WIGLE_NET_DETAIL_URL "https://api.wigle.net/api/v2/network/detail"
#define WIGLE_BT_DETAIL_URL  "https://api.wigle.net/api/v2/bluetooth/detail"
#define BOUNDARY         "rfsentinelboundary"

static void wigle_heap_log(const char *phase)
{
    ESP_LOGI("wigle", "%s: heap libre=%u bloc_max=%u min_libre=%u", phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

static bool build_auth_header(char *out, size_t outsz)
{
    const app_config_t *cfg = config_get();
    if (cfg->wigle_user[0] == '\0' || cfg->wigle_token[0] == '\0') return false;

    char creds[CFG_WIGLE_USER_MAX + CFG_TOKEN_MAX + 1];
    int creds_len = snprintf(creds, sizeof(creds), "%s:%s", cfg->wigle_user, cfg->wigle_token);
    if (creds_len < 0 || (size_t)creds_len >= sizeof(creds)) return false;

    unsigned char enc[512];
    size_t olen = 0;
    int rc = mbedtls_base64_encode(enc, sizeof(enc) - 1, &olen, (const unsigned char *)creds, strlen(creds));
    if (rc != 0) return false;
    enc[olen] = '\0';

    int n = snprintf(out, outsz, "Basic %s", (const char *)enc);
    memset(creds, 0, sizeof(creds));
    return n > 0 && (size_t)n < outsz;
}

/* The HTTP client may accept only part of a buffer in one write. */
static bool wigle_write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    while (len) {
        int n = esp_http_client_write(client, data, (int)len);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

esp_err_t wigle_upload_journal(store_export_t *journal, const char *fname, char *out, size_t outsz)
{
    if (!journal || !fname || !out || outsz < 256) return ESP_ERR_INVALID_ARG;
    strlcpy(out, "Erreur d'envoi inconnue", outsz);
    esp_err_t result = ESP_FAIL;
    long size = store_export_size(journal);
    if (size <= 0 || size > INT_MAX - 1024) {
        strlcpy(out, "Taille du journal invalide", outsz); return ESP_FAIL;
    }
    size_t remaining = (size_t)size;
    char auth_hdr[384];
    if (!build_auth_header(auth_hdr, sizeof(auth_hdr))) {
        strlcpy(out, "Identifiants API WiGLE manquants ou invalides", outsz); return ESP_FAIL;
    }
    esp_http_client_config_t cfg = { .url = WIGLE_UPLOAD_URL, .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 30000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { strlcpy(out, "Initialisation HTTPS impossible", outsz); return ESP_FAIL; }
    esp_err_t header_err = esp_http_client_set_header(client, "Authorization", auth_hdr);
    char *stored_auth = NULL;
    if (header_err == ESP_OK) header_err = esp_http_client_get_header(client, "Authorization", &stored_auth);
    bool auth_header_ok = header_err == ESP_OK && stored_auth && strcmp(stored_auth, auth_hdr) == 0;
    memset(auth_hdr, 0, sizeof(auth_hdr));
    if (!auth_header_ok) { strlcpy(out, "Impossible de preparer l'authentification WiGLE", outsz); goto done; }
    if (esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
        strlcpy(out, "Memoire insuffisante pour la requete WiGLE", outsz); goto done;
    }
    char ct[64];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", BOUNDARY);
    if (esp_http_client_set_header(client, "Content-Type", ct) != ESP_OK) {
        strlcpy(out, "Memoire insuffisante pour la requete WiGLE", outsz); goto done;
    }
    char pre[256];
    int pre_n = snprintf(pre, sizeof(pre), "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", fname);
    static const char post[] = "\r\n--" BOUNDARY "--\r\n";
    if (pre_n <= 0 || (size_t)pre_n >= sizeof(pre)) { strlcpy(out, "Nom de fichier invalide", outsz); goto done; }
    esp_err_t err = esp_http_client_open(client, pre_n + (int)remaining + (int)strlen(post));
    if (err != ESP_OK) { result = err; snprintf(out, outsz, "Connexion WiGLE impossible (%s)", esp_err_to_name(err)); goto done; }
    if (!wigle_write_all(client, pre, (size_t)pre_n)) { strlcpy(out, "Envoi HTTPS interrompu", outsz); goto close; }
    char buf[1024];
    while (remaining) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t n = 0;
        if (store_export_read(journal, buf, want, &n) != ESP_OK || n == 0 || !wigle_write_all(client, buf, n)) {
            strlcpy(out, "Lecture ou envoi du journal interrompu", outsz); goto close;
        }
        remaining -= n;
    }
    if (!wigle_write_all(client, post, strlen(post))) { strlcpy(out, "Envoi HTTPS incomplet", outsz); goto close; }
    if (esp_http_client_fetch_headers(client) < 0) { strlcpy(out, "Reponse WiGLE indisponible", outsz); goto close; }
    int code = esp_http_client_get_status_code(client);
    if (code == 401) {
        strlcpy(out, "HTTP 401 : verifiez API Name et API Token dans la configuration", outsz);
        result = RFS_ERR_WIGLE_AUTH;
        goto close;
    }
    char resp[1024];
    int len = esp_http_client_read_response(client, resp, sizeof(resp) - 1);
    if (len <= 0) { snprintf(out, outsz, "Reponse WiGLE vide (HTTP %d)", code); goto close; }
    resp[len] = '\0';
    cJSON *j = cJSON_Parse(resp);
    if (!j) { snprintf(out, outsz, "Reponse WiGLE invalide (HTTP %d)", code); goto close; }
    bool accepted = code >= 200 && code < 300 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "success"));
    if (accepted) { strlcpy(out, "Accepte par WiGLE, traitement en attente", outsz); result = ESP_OK; }
    else snprintf(out, outsz, "WiGLE a refuse le fichier (HTTP %d)", code);
    cJSON_Delete(j);
close:
    esp_http_client_close(client);
done:
    esp_http_client_cleanup(client);
    return result;
}

static void json_escape(const char *s, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (o + 3 >= outsz) break;
            out[o++] = '\\'; out[o++] = (char)c; out[o] = '\0';
        } else if (c < 32 || c == 127) {
            if (o + 2 >= outsz) break;
            out[o++] = ' '; out[o] = '\0';
        } else {
            if (o + 2 >= outsz) break;
            out[o++] = (char)c; out[o] = '\0';
        }
    }
}

/* WiGLE renvoie tout le corps JSON. esp_http_client_perform() le vide dans son
   propre buffer, donc un esp_http_client_read() posterieur ne renvoie jamais
   rien et toute recherche parait vide. La requete est donc ouverte a la main,
   puis le corps est lu avec une borne haute pour ne pas epuiser le tas. */
#define WIGLE_BODY_MAX 6144
#define WIGLE_MAX_ROWS 10

static void search_error(char *out, size_t outsz, const char *code)
{
    snprintf(out, outsz, "{\"results\":[],\"count\":0,\"error\":\"%s\"}", code);
}

/* L'adresse part telle quelle dans l'URL de requete : on ne garde que les
   caracteres d'une adresse materielle, pour qu'une valeur fardee ne puisse pas
   ajouter des parametres a la requete WiGLE. */
static bool mac_is_safe(const char *s)
{
    int n = 0;
    for (; *s; s++, n++) {
        char c = *s;
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex && c != ':' && c != '-' && c != '.') return false;
    }
    return n >= 4 && n < 40;
}

/* Quand le corps est coupe a la borne de lecture, on garde le plus long prefix
   qui se termine juste apres un element complet, puis on referme les tableaux
   et objets restes ouverts. Sans cela, une reponse detaillee un peu longue
   paraitrait illegible alors qu'elle contient des dizaines de points. */
static bool json_salvage(char *b, size_t n, size_t cap)
{
    char stack[16];
    int depth = 0;
    bool in_str = false, escaped = false;
    size_t best = 0;
    int best_depth = 0;

    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        if (in_str) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{' || c == '[') {
            if (depth >= (int)sizeof(stack)) return false;
            stack[depth] = c;
            depth++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (depth > 0) depth--;
            if (depth >= 1) { best = i + 1; best_depth = depth; }
        }
    }
    if (best == 0 || best + (size_t)best_depth + 1 > cap) return false;
    for (int d = best_depth - 1; d >= 0; d--)
        b[best++] = (stack[d] == '[') ? ']' : '}';
    b[best] = '\0';
    return true;
}


/* Lecture du corps apres esp_http_client_open + fetch_headers. */
static esp_err_t wigle_read_body(esp_http_client_handle_t client, char *buf, size_t cap, bool *truncated)
{
    if (!buf || cap < WIGLE_BODY_MAX + 4) return ESP_ERR_INVALID_SIZE;
    size_t total = 0;
    *truncated = false;
    for (;;) {
        if (total >= WIGLE_BODY_MAX) { *truncated = true; break; }
        int r = esp_http_client_read(client, buf + total, (int)(WIGLE_BODY_MAX - total));
        if (r < 0) return ESP_FAIL;
        if (r == 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    if (total == 0) return ESP_ERR_NOT_FOUND;
    if (*truncated && !json_salvage(buf, total, cap)) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

/* Les deux tables de WiGLE (Wi-Fi et Bluetooth) ne nomment pas les champs de la
   meme facon : on garde la premiere clef presente dans l'objet. */
static void first_str(const cJSON *obj, char *dst, size_t dstsz,
                      const char *k1, const char *k2, const char *k3)
{
    const char *keys[3] = { k1, k2, k3 };
    for (int i = 0; i < 3; i++) {
        if (!keys[i]) break;
        cJSON *x = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
        if (x && cJSON_IsString(x) && x->valuestring[0]) {
            strlcpy(dst, x->valuestring, dstsz);
            return;
        }
    }
}

static double first_num(const cJSON *obj, const char *k1, const char *k2,
                        const char *k3)
{
    const char *keys[3] = { k1, k2, k3 };
    for (int i = 0; i < 3; i++) {
        if (!keys[i]) break;
        cJSON *x = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
        if (x && cJSON_IsNumber(x)) return x->valuedouble;
    }
    return 0;
}

/* --- requete et construction de la reponse ----------------------------- */

/* The response body aliases out; it must not be freed separately. */
static esp_err_t wigle_fetch(const char *full_url, char **body_out, int *status_out,
                             bool *truncated_out, char *out, size_t outsz)
{
    char auth_hdr[384];
    bool truncated = false;
    int status = 0;
    char code[24];
    esp_err_t err = ESP_FAIL;

    *body_out = NULL;
    *status_out = 0;
    *truncated_out = false;

    if (!build_auth_header(auth_hdr, sizeof(auth_hdr))) {
        search_error(out, outsz, "not_configured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t cfg = {
        .url = full_url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };
    wigle_heap_log("GET avant initialisation TLS");
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { wigle_heap_log("GET initialisation TLS echouee"); search_error(out, outsz, "http_init"); return ESP_FAIL; }

    if (esp_http_client_set_header(client, "Authorization", auth_hdr) != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
        search_error(out, outsz, "nomem");
        goto cleanup;
    }
    esp_err_t open_err = esp_http_client_open(client, 0);
    if (open_err != ESP_OK) {
        ESP_LOGW("wigle", "connexion HTTPS impossible: %s, tas libre %u, plus grand bloc %u",
                 esp_err_to_name(open_err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        search_error(out, outsz, "network");
        goto cleanup;
    }
    wigle_heap_log("GET TLS connecte");
    if (esp_http_client_fetch_headers(client) < 0) {
        wigle_heap_log("GET entetes echoues");
        search_error(out, outsz, "no_response");
        goto close_client;
    }

    status = esp_http_client_get_status_code(client);
    *status_out = status;
    ESP_LOGI("wigle", "GET HTTP %d", status);
    if (status != 200) {
        snprintf(code, sizeof(code), "http_%d", status);
        search_error(out, outsz, code);
        if (status == 401) err = RFS_ERR_WIGLE_AUTH;
        else if (status == 404) err = ESP_ERR_NOT_FOUND;
        else err = ESP_FAIL;
        goto close_client;
    }

    /* Reuse the response buffer while TLS is still active.  The parsed JSON
       owns its strings, so it can subsequently be overwritten with results. */
    err = wigle_read_body(client, out, outsz, &truncated);
    if (err == ESP_ERR_NOT_FOUND) { search_error(out, outsz, "empty"); err = ESP_ERR_NOT_FOUND; goto close_client; }
    if (err != ESP_OK) { wigle_heap_log("GET lecture echouee"); search_error(out, outsz, "read"); goto close_client; }
    ESP_LOGI("wigle", "GET corps: %u octets, tronque=%s", (unsigned)strlen(out), truncated ? "oui" : "non");
    wigle_heap_log("GET corps lu");

    *body_out = out;
    *truncated_out = truncated;
    err = ESP_OK;

close_client:
    esp_http_client_close(client);
cleanup:
    memset(auth_hdr, 0, sizeof(auth_hdr));
    esp_http_client_cleanup(client);
    wigle_heap_log("GET TLS libere");
    return err;
}

/* Adresse materielle prete pour une URL : deux-points echappes, et casse du
   reseau demande, WiGLE indexant le Wi-Fi en majuscules et le Bluetooth en
   minuscules. */
static void enc_mac(const char *value, bool lower, char *enc, size_t encsz)
{
    size_t ei = 0;
    for (const char *p = value; *p && ei + 4 < encsz; p++) {
        char c = lower ? (char)tolower((unsigned char)*p) : *p;
        if (c == ':') { enc[ei++] = '%'; enc[ei++] = '3'; enc[ei++] = 'A'; }
        else enc[ei++] = c;
    }
    enc[ei] = '\0';
}

/* WiGLE can return netId as a decimal representation of the 48-bit MAC. */
static bool wigle_normalize_mac(const char *raw, const char *requested, char out[18])
{
    if (!raw || !*raw) return false;
    char hex[13]; size_t nh = 0; bool digits = true, separated = false;
    for (const char *p = raw; *p; p++) {
        if (*p == ':' || *p == '-' || *p == '.') { separated = true; continue; }
        if (!isxdigit((unsigned char)*p)) return false;
        if (!isdigit((unsigned char)*p)) digits = false;
        if (nh < 12) hex[nh] = (char)toupper((unsigned char)*p);
        nh++;
    }
    char as_hex[18] = "", as_dec[18] = "";
    if (nh == 12) {
        hex[12] = '\0';
        snprintf(as_hex, sizeof(as_hex), "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                 hex, hex+2, hex+4, hex+6, hex+8, hex+10);
    }
    if (digits && !separated) {
        uint64_t v = 0; bool valid = true;
        for (const char *p = raw; *p; p++) {
            unsigned d = (unsigned)(*p - '0');
            if (v > (UINT64_C(0xFFFFFFFFFFFF) - d) / 10) { valid = false; break; }
            v = v * 10 + d;
        }
        if (valid) snprintf(as_dec, sizeof(as_dec), "%02X:%02X:%02X:%02X:%02X:%02X",
            (unsigned)((v >> 40) & 255), (unsigned)((v >> 32) & 255),
            (unsigned)((v >> 24) & 255), (unsigned)((v >> 16) & 255),
            (unsigned)((v >> 8) & 255), (unsigned)(v & 255));
    }
    const char *chosen = NULL;
    if (requested && as_dec[0] && !strcasecmp(as_dec, requested)) chosen = as_dec;
    else if (as_hex[0]) chosen = as_hex;
    else if (as_dec[0]) chosen = as_dec;
    if (!chosen) return false;
    strlcpy(out, chosen, 18); return true;
}

/* Construction du tableau rendu a la page, identique qu'on vienne de la
   recherche simple ou de l'historique detaille. Les lignes sont d'abord
   rassemblees en memoire pour que la liste soit rendue dans l'ordre
   chronologique, le plus recent en dernier. */
typedef struct {
    char mac[24];
    char ssid[64];
    char kind[24];
    char ts[32];
    char last[32];
    double lat, lon, acc, rssi;
} wigle_row_t;

typedef struct { wigle_row_t *v; int n; int cap; bool truncated; } rows_t;

/* Allocate result rows only after HTTPS has closed, leaving RAM for TLS. */
static bool rows_open(rows_t *r)
{
    r->v = calloc((size_t)WIGLE_MAX_ROWS, sizeof(*r->v));
    if (!r->v) return false;
    r->n = 0;
    r->cap = WIGLE_MAX_ROWS;
    r->truncated = false;
    return true;
}

static void rows_add(rows_t *r, const char *mac, const char *ssid, const char *kind,
                     const char *ts, const char *last, double lat, double lon,
                     double acc, double rssi)
{
    int slot = r->n;
    if (r->n >= r->cap) {
        r->truncated = true;
        if (!ts || !ts[0]) return;
        slot = 0;
        for (int i = 1; i < r->n; i++) {
            if (!r->v[i].ts[0] || (r->v[slot].ts[0] && strcmp(r->v[i].ts, r->v[slot].ts) < 0)) slot = i;
        }
        if (r->v[slot].ts[0] && strcmp(ts, r->v[slot].ts) <= 0) return;
    }
    wigle_row_t *e = &r->v[slot];
    strlcpy(e->mac, mac, sizeof(e->mac));
    strlcpy(e->ssid, ssid, sizeof(e->ssid));
    strlcpy(e->kind, kind, sizeof(e->kind));
    strlcpy(e->ts, ts, sizeof(e->ts));
    strlcpy(e->last, last, sizeof(e->last));
    e->lat = lat; e->lon = lon; e->acc = acc; e->rssi = rssi;
    if (r->n < r->cap) r->n++;
}

/* Les dates de WiGLE sont au format ISO : un tri lexicographique suffit. Une
   ligne sans date est reculee a la fin pour ne pas masquer le reste. */
static int row_cmp(const void *a, const void *b)
{
    const wigle_row_t *x = (const wigle_row_t *)a;
    const wigle_row_t *y = (const wigle_row_t *)b;
    if (!x->ts[0] && !y->ts[0]) return 0;
    if (!x->ts[0]) return 1;
    if (!y->ts[0]) return -1;
    return strcmp(x->ts, y->ts);
}

static void rows_emit(rows_t *r, char *out, size_t outsz, bool truncated)
{
    if (r->n > 1) qsort(r->v, (size_t)r->n, sizeof(wigle_row_t), row_cmp);

    size_t pos = 0;
    int written = 0;
    pos += (size_t)snprintf(out, outsz, "{\"results\":[");
    for (int i = 0; i < r->n; i++) {
        char e_mac[64], e_ssid[160], e_kind[56], e_ts[56], e_last[56];
        json_escape(r->v[i].mac, e_mac, sizeof(e_mac));
        json_escape(r->v[i].ssid, e_ssid, sizeof(e_ssid));
        json_escape(r->v[i].kind, e_kind, sizeof(e_kind));
        json_escape(r->v[i].ts, e_ts, sizeof(e_ts));
        json_escape(r->v[i].last, e_last, sizeof(e_last));
        int w = snprintf(out + pos, outsz - pos,
            "%s{\"mac\":\"%s\",\"ssid\":\"%s\",\"time\":\"%s\",\"lasttime\":\"%s\","
            "\"lat\":%.6f,\"lon\":%.6f,\"accuracy\":%.0f,\"rssi\":%.0f,\"networkType\":\"%s\"}",
            written ? "," : "", e_mac, e_ssid, e_ts, e_last,
            r->v[i].lat, r->v[i].lon, r->v[i].acc, r->v[i].rssi, e_kind);
        if (w <= 0 || pos + (size_t)w + 96 >= outsz) break;
        pos += (size_t)w;
        written++;
    }
    snprintf(out + pos, outsz - pos, "],\"count\":%d,\"truncated\":%s}",
             written, (truncated || r->truncated || written < r->n) ? "true" : "false");
}

/* --- recherche simple : une ligne par reseau, position moyenne ----------- */

static esp_err_t wigle_search_one(const char *url, const char *extra, const char *value,
                                  bool lower, char *out, size_t outsz,
                                  int *rows_out)
{
    char enc[80];
    char full_url[320];
    char *body = NULL;
    int status = 0;
    bool truncated = false;

    if (out == NULL || outsz < 512 || rows_out == NULL) return ESP_ERR_INVALID_ARG;
    *rows_out = 0;
    search_error(out, outsz, "empty");

    enc_mac(value, lower, enc, sizeof(enc));
    snprintf(full_url, sizeof(full_url), "%s?netid=%s%s", url, enc,
             (extra && extra[0]) ? extra : "");

    esp_err_t err = wigle_fetch(full_url, &body, &status, &truncated, out, outsz);
    if (err != ESP_OK) return err;
    (void)status;

    cJSON *j = cJSON_Parse(body);
    if (!j) { wigle_heap_log("GET analyse JSON echouee"); search_error(out, outsz, "parse"); return ESP_FAIL; }

    cJSON *res = cJSON_GetObjectItem(j, "results");
    if (!cJSON_IsArray(res)) { cJSON_Delete(j); search_error(out, outsz, "empty"); return ESP_OK; }

    rows_t rb;
    if (!rows_open(&rb)) { wigle_heap_log("GET allocation resultats echouee"); cJSON_Delete(j); search_error(out, outsz, "nomem"); return ESP_ERR_NO_MEM; }

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, res) {
        char mac[24] = "", ssid[64] = "", kind[24] = "", ts[32] = "", last[32] = "";
        first_str(it, mac, sizeof(mac), "netid", "id", "networkId");
        first_str(it, ssid, sizeof(ssid), "ssid", "name", NULL);
        first_str(it, kind, sizeof(kind), "type", "networkType", "BTType");
        first_str(it, ts, sizeof(ts), "firsttime", "time", NULL);
        first_str(it, last, sizeof(last), "lasttime", "time", NULL);
        char normalized[18];
        if (!wigle_normalize_mac(mac, value, normalized)) continue;
        rows_add(&rb, normalized, ssid, kind, ts, last,
                 first_num(it, "trilat", "lat", "centroidLatitude"),
                 first_num(it, "trilong", "lon", "centroidLongitude"),
                 first_num(it, "locAccuracy", "accuracy", NULL),
                 first_num(it, "rssi", "signal", NULL));
    }
    rows_emit(&rb, out, outsz, truncated);
    *rows_out = rb.n;
    free(rb.v);
    cJSON_Delete(j);
    return ESP_OK;
}

/* --- historique detaille : un point par observation, avec sa date -------- */

/* WiGLE range les observations dans des tableaux imbriques : locationData pour
   le Wi-Fi, une liste de clusters pour le Bluetooth. */
static bool detail_locations(rows_t *rb, const cJSON *locs, const char *mac,
                             const char *ssid, const char *kind)
{
    if (!cJSON_IsArray(locs)) return false;
    bool found = false;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, locs) {
        char ts[40] = "", lmac[32] = "", lssid[64] = "";
        first_str(it, ts, sizeof(ts), "time", "lastupdt", NULL);
        first_str(it, lmac, sizeof(lmac), "netId", "netid", NULL);
        first_str(it, lssid, sizeof(lssid), "ssid", "name", NULL);
        double lat = first_num(it, "latitude", "lat", NULL);
        double lon = first_num(it, "longitude", "lon", NULL);
        if (lat == 0 && lon == 0) continue;
        char normalized[18];
        if (!wigle_normalize_mac(lmac[0] ? lmac : mac, mac, normalized)) continue;
        found = true;
        rows_add(rb, normalized, (lssid[0] ? lssid : ssid), kind, ts, ts,
                 lat, lon, first_num(it, "accuracy", "locAccuracy", NULL),
                 first_num(it, "signal", "rssi", NULL));
    }
    return found;
}

static esp_err_t wigle_detail_one(const char *url, const char *extra, const char *value,
                                  bool lower, bool bt, char *out, size_t outsz,
                                  int *rows_out)
{
    char enc[80];
    char full_url[320];
    char *body = NULL;
    int status = 0;
    bool truncated = false;

    if (out == NULL || outsz < 512 || rows_out == NULL) return ESP_ERR_INVALID_ARG;
    *rows_out = 0;
    search_error(out, outsz, "empty");

    enc_mac(value, lower, enc, sizeof(enc));
    snprintf(full_url, sizeof(full_url), "%s?netid=%s%s", url, enc,
             (extra && extra[0]) ? extra : "");

    esp_err_t err = wigle_fetch(full_url, &body, &status, &truncated, out, outsz);
    if (err != ESP_OK) return err;
    (void)status;

    cJSON *j = cJSON_Parse(body);
    if (!j) { wigle_heap_log("GET analyse JSON echouee"); search_error(out, outsz, "parse"); return ESP_FAIL; }

    cJSON *res = cJSON_GetObjectItem(j, "results");
    if (!cJSON_IsArray(res)) { cJSON_Delete(j); search_error(out, outsz, "empty"); return ESP_OK; }

    rows_t rb;
    if (!rows_open(&rb)) { wigle_heap_log("GET allocation resultats echouee"); cJSON_Delete(j); search_error(out, outsz, "nomem"); return ESP_ERR_NO_MEM; }

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, res) {
        char mac[24] = "", name[64] = "", kind[24] = "";
        bool has_locations = false;
        first_str(it, mac, sizeof(mac), "networkId", "netid", "id");
        char normalized_mac[18];
        if (wigle_normalize_mac(mac, value, normalized_mac)) strlcpy(mac, normalized_mac, sizeof(mac));
        else strlcpy(mac, value, sizeof(mac));
        first_str(it, name, sizeof(name), "ssid", "name", NULL);
        first_str(it, kind, sizeof(kind), "type", "networkType", "BTType");

        if (bt) {
            cJSON *cls = cJSON_GetObjectItemCaseSensitive(it, "locationClusters");
            if (cJSON_IsArray(cls)) {
                cJSON *cl = NULL;
                cJSON_ArrayForEach(cl, cls)
                    has_locations |= detail_locations(&rb, cJSON_GetObjectItemCaseSensitive(cl, "locations"),
                                                       mac, name, kind);
            }
        } else {
            has_locations = detail_locations(&rb, cJSON_GetObjectItemCaseSensitive(it, "locationData"),
                                             mac, name, kind);
        }

        /* Pas de points detailles : la position trilaterale de l'entete reste
           exploitable, avec la periode couverte. */
        if (!has_locations) {
            char ts[40] = "", last[40] = "";
            first_str(it, ts, sizeof(ts), "firstSeen", "firsttime", NULL);
            first_str(it, last, sizeof(last), "lastSeen", "lasttime", NULL);
            double lat = first_num(it, "trilateratedLatitude", "trilat", NULL);
            double lon = first_num(it, "trilateratedLongitude", "trilong", NULL);
            if (mac[0] && !(lat == 0 && lon == 0))
                rows_add(&rb, mac, name, kind, ts, last, lat, lon, 0, 0);
        }
    }
    rows_emit(&rb, out, outsz, truncated);
    *rows_out = rb.n;
    free(rb.v);
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t wigle_search_device(const char *mac, const char *type, char *out, size_t outsz)
{
    if (!mac || !mac[0] || !out || outsz < 512) return ESP_ERR_INVALID_ARG;
    if (!mac_is_safe(mac)) { search_error(out, outsz, "bad_mac"); return ESP_ERR_INVALID_ARG; }

    /* La reponse part d'un contenu valide : sinon la page lirait un buffer vide. */
    search_error(out, outsz, "empty");

    ESP_LOGI("wigle", "recherche %s type %s, tas libre %u", mac,
             (type && type[0]) ? type : "?",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    bool only_ble = type && strcasecmp(type, "BLE") == 0;
    bool only_wifi = type && strcasecmp(type, "WIFI") == 0;
    int rows = 0;
    bool summary_only = false;
    bool detail_failed = false;
    esp_err_t err = ESP_OK;

    /* Le type vient de l'observation cliquee : il choisit la table, exactement
       comme les onglets Wi-Fi et Bluetooth du site. Interroger l'autre table ne
       ferait que ajouter quelques secondes pour rien. */
    bool try_wifi = !only_ble;
    bool try_bt = !only_wifi;

    /* 1) l'historique point par point, seul a donner une date par position */
    if (try_wifi) {
        err = wigle_detail_one(WIGLE_NET_DETAIL_URL, "&type=WIFI", mac, false, false,
                               out, outsz, &rows);
        if (rows > 0) goto done;
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) detail_failed = true;
    }
    if (try_bt) {
        err = wigle_detail_one(WIGLE_BT_DETAIL_URL, NULL, mac, true, true,
                               out, outsz, &rows);
        if (rows > 0) goto done;
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) detail_failed = true;
    }

    /* 2) a defaut la recherche simple, qui ne donne que la position moyenne.
       La table Bluetooth exige ses filtres de type, et la recherche est
       plafonnee pour rester dans une reponse lisible. */
    if (try_wifi && rows == 0) {
        summary_only = true;
        err = wigle_search_one(WIGLE_NET_URL, "&resultsPerPage=10", mac, false,
                               out, outsz, &rows);
        if (rows > 0) goto done;
    }
    if (try_bt && rows == 0) {
        summary_only = true;
        err = wigle_search_one(WIGLE_BT_URL, "&showBt=true&showBle=true&resultsPerPage=10",
                               mac, true, out, outsz, &rows);
    }
done:
    if (rows > 0 && summary_only) {
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] == '}') {
            int n = snprintf(out + len - 1, outsz - len + 1,
                             ",\"summary_only\":true,\"detail_failed\":%s}",
                             detail_failed ? "true" : "false");
            if (n < 0 || (size_t)n >= outsz - len + 1) {
                search_error(out, outsz, "nomem");
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    ESP_LOGI("wigle", "fin de recherche, %d ligne(s), resultat=%s", rows, esp_err_to_name(err));
    wigle_heap_log("recherche terminee");
    return err;
}

