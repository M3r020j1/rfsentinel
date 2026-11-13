#include "store.h"

#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_littlefs.h"

/* conversion struct tm (UTC) -> epoch, sans dependre de timegm() */
static time_t rfs_timegm(const struct tm *t) {
    int year = t->tm_year + 1900;
    int mon = t->tm_mon + 1;
    int y = year - (mon <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (time_t)(era * 146097 + (int)doe - 719468) * 86400
           + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"

#include "config.h"
#include "ble_address.h"
#include "net.h"

static const char *TAG = "rfs.store";

#define RFS_MOUNT "/rfs"
#define RFS_OBS_DIR "/rfs/observations"
#define RFS_OBS_PREFIX "obs_"
#define RFS_OBS_MAX_FILES 32
#define RFS_LINE_MAX 256
#define RFS_LIST_PAGE 14
#define RFS_HIST_MAX 10
#define RFS_USAGE_LIMIT_PCT 90
#define RFS_SEGMENT_MAX_BYTES (32 * 1024)

#define NVS_NS "rfsentinel"
#define K_BL_PREFIX "bl_"
#define K_BL_INDEX "bl_idx"
#define BL_INDEX_MAX (BL_CACHE_MAX * 18)

static bool s_mounted = false;
static SemaphoreHandle_t s_store_lock;
static bool s_counts_valid = false;
static uint32_t s_counts_wifi = 0, s_counts_ble = 0;
static uint32_t s_store_generation = 0;
static uint32_t s_store_rewrite_generation = 0;
/* Additions after the recount snapshot are applied once when it is published. */
static bool s_recount_active = false;
static uint32_t s_recount_wifi = 0, s_recount_ble = 0;
/* New segment names use a monotonic sequence, independent of wall time. */
static uint32_t s_segment_seq = 0;
static bool s_segment_initialized = false;
static char s_segment_name[32];
static char s_segment_quality;
static void store_counts_task(void *arg);

/* Cache RAM de la liste noire: charge une seule fois, invalide a chaque
   add/remove. Evite un acces NVS pour chaque observation du scan. */
#define BL_CACHE_MAX 64
static char s_bl_mac[BL_CACHE_MAX][18];
static int s_bl_count = 0;
static volatile bool s_bl_loaded = false;
static SemaphoreHandle_t s_bl_lock;

typedef struct {
    char mac[18];
    int64_t last_seen_us;
} seen_t;

#define SEEN_CAPACITY 128
static seen_t s_seen[SEEN_CAPACITY];
static int s_seen_count = 0;

static const char CSV_HDR[] =
    "WigleWifi-1.6,apssids=true,latlon=true,lockedToNetwork=false,cellBands=auto\n"
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";

/* ---- helpers ---- */

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
        } else if (c == '\t') {
            if (o + 2 >= n) break;
            dst[o++] = '\\';
            dst[o++] = 't';
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static void csv_escape(const char *src, char *dst, size_t n) {
    bool need = false;
    for (const char *p = src; p && *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') { need = true; break; }
    }
    size_t o = 0;
    if (need) {
        if (o + 1 < n) dst[o++] = '"';
        for (const char *p = src; p && *p; p++) {
            if (*p == '"') {
                if (o + 2 >= n) break;
                dst[o++] = '"';
                dst[o++] = '"';
            } else {
                if (o + 1 >= n) break;
                dst[o++] = *p;
            }
        }
        if (o + 1 < n) dst[o++] = '"';
    } else {
        for (const char *p = src; p && *p; p++) {
            if (o + 1 >= n) break;
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

/* liste des fichiers d'observations tries du plus ancien au plus recent */
static int list_obs_files(char names[][32], int max) {
    int count = 0;
    DIR *d = opendir(RFS_OBS_DIR);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < max) {
        if (strncmp(e->d_name, RFS_OBS_PREFIX, strlen(RFS_OBS_PREFIX)) != 0) continue;
        if (strstr(e->d_name, ".csv") == NULL) continue;
        strlcpy(names[count], e->d_name, 32);
        count++;
    }
    closedir(d);
    /* tri par nom (= ordre chronologique) */
    for (int i = 1; i < count; i++) {
        char tmp[32];
        strlcpy(tmp, names[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strlcpy(names[j + 1], names[j], 32);
            j--;
        }
        strlcpy(names[j + 1], tmp, 32);
    }
    return count;
}

/* Legacy date-named files predate sequenced segments. Their original write
   order cannot be reconstructed when the clock was reset. */
static bool segment_number(const char *name, uint32_t *number) {
    unsigned long value = 0;
    char quality = 0;
    int end = 0;
    if (strlen(name) != 21 || strncmp(name, "obs_s", 5) != 0) return false;
    for (int i = 5; i < 15; i++) if (!isdigit((unsigned char)name[i])) return false;
    if (sscanf(name, "obs_s%10lu_%c.csv%n", &value, &quality, &end) != 2 ||
        name[end] != '\0' || value > UINT32_MAX ||
        (quality != 'N' && quality != 'E' && quality != 'U')) return false;
    *number = (uint32_t)value;
    return true;
}

static esp_err_t segment_initialize(void) {
    if (s_segment_initialized) return ESP_OK;
    DIR *d = opendir(RFS_OBS_DIR);
    if (!d) return ESP_FAIL;
    struct dirent *e;
    uint32_t max = 0;
    while ((e = readdir(d)) != NULL) {
        uint32_t number;
        if (segment_number(e->d_name, &number) && number > max) max = number;
    }
    closedir(d);
    s_segment_seq = max;
    s_segment_initialized = true;
    return ESP_OK;
}

/* Find the oldest candidate across the whole directory, not just 32 files.
   Legacy files are removed first, ordered by their historic names. */
static esp_err_t rotate_prune_one(void) {
    DIR *d = opendir(RFS_OBS_DIR);
    if (!d) return ESP_FAIL;
    char oldest[32] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        size_t len = strlen(name);
        if (len >= sizeof(oldest) || len < 8 || strncmp(name, RFS_OBS_PREFIX, 4) ||
            strcmp(name + len - 4, ".csv") || strcmp(name, s_segment_name) == 0) continue;
        if (!oldest[0] || strcmp(name, oldest) < 0) strlcpy(oldest, name, sizeof(oldest));
    }
    closedir(d);
    if (!oldest[0]) {
        ESP_LOGE(TAG, "rotation: aucun ancien segment supprimable");
        return ESP_FAIL;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", RFS_OBS_DIR, oldest);
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "rotation: suppression impossible: %s", oldest);
        return ESP_FAIL;
    }
    s_store_generation++;
    s_store_rewrite_generation++;
    s_counts_valid = false;
    ESP_LOGW(TAG, "rotation: supprime %s", oldest);
    return ESP_OK;
}

/* ---- API publique ---- */

esp_err_t store_init(void) {
    if (!s_store_lock) s_store_lock = xSemaphoreCreateMutex();
    if (!s_store_lock) return ESP_ERR_NO_MEM;
    if (!s_bl_lock) s_bl_lock = xSemaphoreCreateMutex();
    if (!s_bl_lock) return ESP_ERR_NO_MEM;
    s_bl_loaded = false;

    esp_vfs_littlefs_conf_t conf = {
        .base_path = RFS_MOUNT,
        .partition_label = "storage",
        /* Never erase an existing journal on a mount or migration error. */
        .format_if_mount_failed = false,
        .grow_on_mount = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    struct stat st;
    if (stat(RFS_OBS_DIR, &st) != 0) {
        if (mkdir(RFS_OBS_DIR, 0777) != 0) {
            ESP_LOGE(TAG, "mkdir %s echoue", RFS_OBS_DIR);
        }
    }

    ESP_LOGI(TAG, "littlefs monte sur %s", RFS_MOUNT);
    if (xTaskCreate(store_counts_task, "rfs_counts", 6144, NULL, 1, NULL) != pdPASS)
        ESP_LOGW(TAG, "compteurs journal indisponibles: creation tache impossible");
    return ESP_OK;
}

bool store_ready(void) {
    return s_mounted;
}

esp_err_t store_upload_open(const char *path, FILE **file, size_t *length) {
    if (!path || !file || !length || !s_mounted || !s_store_lock) return ESP_ERR_INVALID_ARG;
    *file = NULL;
    *length = 0;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    struct stat st;
    bool ok = f && fstat(fileno(f), &st) == 0 && st.st_size > 0;
    if (ok) { *file = f; *length = (size_t)st.st_size; }
    else if (f) fclose(f);
    xSemaphoreGive(s_store_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t store_upload_read(FILE *file, void *buf, size_t want, size_t *got) {
    if (!file || !buf || !got || !s_store_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    *got = fread(buf, 1, want, file);
    bool failed = ferror(file);
    xSemaphoreGive(s_store_lock);
    return failed ? ESP_FAIL : ESP_OK;
}

void store_upload_close(FILE *file) {
    if (!file) return;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    fclose(file);
    xSemaphoreGive(s_store_lock);
}

static esp_err_t store_append_obs_locked(const s3_obs_t *obs, bool *appended) {
    if (!s_mounted || !obs) return ESP_ERR_INVALID_STATE;

    /* MAC exclue: on n'enregistre pas, ce n'est pas une erreur */
    if (store_is_blacklisted(obs->mac)) return ESP_OK;

    const app_config_t *cfg = config_get();

    /* seuil RSSI : ne pas enregistrer sous le seuil (comparaison signee) */
    if (obs->rssi < cfg->min_rssi) return ESP_OK;

    /* dedup + redécouverte */
    for (int i = 0; i < s_seen_count; i++) {
        if (strcmp(s_seen[i].mac, obs->mac) == 0) {
            int64_t now_us = esp_timer_get_time();
            if (cfg->rediscover_min > 0 &&
                (now_us - s_seen[i].last_seen_us) < (int64_t)cfg->rediscover_min * 60 * 1000000) {
                return ESP_OK; /* deja vu recemment */
            }
            break;
        }
    }

    char line[RFS_LINE_MAX];
    char ts[24];
    if (obs->ts[0] == '\0') {
        if (!net_time_utc(ts, sizeof(ts))) strlcpy(ts, "0", sizeof(ts));
    } else {
        strlcpy(ts, obs->ts, sizeof(ts));
    }

    char ssid_esc[CFG_SSID_MAX * 2 + 3];
    csv_escape(obs->ssid, ssid_esc, sizeof(ssid_esc));

    int n = snprintf(line, sizeof(line),
                     "%s,%s,%s,%s,%d,%d,%d,%.5f,%.5f,0,0,%s,%d,%s\n",
                     obs->mac, ssid_esc, obs->is_ble ? "BLE" : obs->auth, ts,
                     obs->channel, obs->freq, obs->rssi,
                     (double)cfg->lat, (double)cfg->lon, obs->is_ble ? obs->auth : "",
                     obs->mfgr, obs->is_ble ? "BLE" : "WIFI");
    if (n <= 0 || n >= (int)sizeof(line) - 2) return ESP_FAIL;

    /* Bounded segments limit how much history a space-based rotation drops.
       A new segment is also needed when the clock quality changes. */
    if (segment_initialize() != ESP_OK) return ESP_FAIL;
    ts_quality_t quality = (ts_quality_t)obs->time_quality;
    char quality_tag = quality == TS_NTP_SYNCED ? 'N' : (quality == TS_ESTIMATED ? 'E' : 'U');
    char path[80];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", RFS_OBS_DIR, s_segment_name);
    bool exists = s_segment_name[0] && stat(path, &st) == 0;
    if (!s_segment_name[0] || s_segment_quality != quality_tag ||
        (exists && (size_t)st.st_size + (size_t)n > RFS_SEGMENT_MAX_BYTES)) {
        if (s_segment_seq == UINT32_MAX) return ESP_FAIL;
        s_segment_seq++;
        snprintf(s_segment_name, sizeof(s_segment_name), "obs_s%010lu_%c.csv",
                 (unsigned long)s_segment_seq, quality_tag);
        s_segment_quality = quality_tag;
        snprintf(path, sizeof(path), "%s/%s", RFS_OBS_DIR, s_segment_name);
        exists = false;
    }

    /* Remove only oldest complete files until below the high-water mark. */
    size_t ftotal = 0, fused = 0;
    for (;;) {
        if (esp_littlefs_info("storage", &ftotal, &fused) != ESP_OK || ftotal == 0) {
            ESP_LOGE(TAG, "rotation: espace du journal indisponible");
            return ESP_FAIL;
        }
        if ((uint64_t)fused * 100 < (uint64_t)ftotal * RFS_USAGE_LIMIT_PCT) break;
        if (rotate_prune_one() != ESP_OK) return ESP_FAIL;
        vTaskDelay(1);
    }

    FILE *f = fopen(path, exists ? "a" : "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s: echec", path);
        return ESP_FAIL;
    }
    if (!exists) {
        fputs(CSV_HDR, f);
    }
    size_t wl = fwrite(line, 1, strlen(line), f);
    int close_result = fclose(f);
    if (wl != strlen(line) || close_result != 0) {
        ESP_LOGE(TAG, "fwrite incomplet");
        return ESP_FAIL;
    }
    *appended = true;
    s_store_generation++;
    if (s_recount_active) {
        if (obs->is_ble) s_recount_ble++;
        else s_recount_wifi++;
    }
    if (s_counts_valid) {
        if (obs->is_ble) s_counts_ble++;
        else s_counts_wifi++;
    }

    /* marquer comme vu */
    {
        bool found = false;
        for (int i = 0; i < s_seen_count; i++) {
            if (strcmp(s_seen[i].mac, obs->mac) == 0) {
                s_seen[i].last_seen_us = esp_timer_get_time();
                found = true;
                break;
            }
        }
        if (!found) {
            int slot = s_seen_count;
            if (slot < SEEN_CAPACITY) s_seen_count++;
            else {
                slot = 0;
                for (int i = 1; i < SEEN_CAPACITY; i++)
                    if (s_seen[i].last_seen_us < s_seen[slot].last_seen_us) slot = i;
            }
            strlcpy(s_seen[slot].mac, obs->mac, sizeof(s_seen[0].mac));
            s_seen[slot].last_seen_us = esp_timer_get_time();
        }
    }
    return ESP_OK;
}

esp_err_t store_append_obs(const s3_obs_t *obs, bool *appended) {
    if (!appended) return ESP_ERR_INVALID_ARG;
    *appended = false;
    if (!s_store_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    esp_err_t result = store_append_obs_locked(obs, appended);
    xSemaphoreGive(s_store_lock);
    return result;
}

/* Split CSV in place, including quoted fields, escaped quotes and empty fields. */
static int csv_fields(char *line, char **fields, int limit) {
    char *r = line, *w = line;
    int count = 0;
    while (count < limit) {
        fields[count++] = w;
        bool quoted = (*r == '"');
        if (quoted) r++;
        while (*r) {
            if (quoted && *r == '"') {
                if (r[1] == '"') { *w++ = '"'; r += 2; continue; }
                r++; break;
            }
            if (!quoted && (*r == ',' || *r == '\r' || *r == '\n')) break;
            *w++ = *r++;
        }
        bool more = (*r == ',');
        if (more) r++;
        *w++ = '\0';
        if (!more) break;
    }
    return count;
}

/* One logical record can contain newlines inside a quoted field. A malformed
   or overlong record is an error, never a partial record to purge or count. */
static int csv_read_record(FILE *f, char *record, size_t size) {
    size_t used = 0; bool quoted = false;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (used + 1 >= size) return -1;
        record[used++] = (char)c;
        if (c == '"') quoted = !quoted;
        if (c == '\n' && !quoted) { record[used] = '\0'; return 1; }
    }
    return used || ferror(f) ? -1 : 0;
}

static bool obs_file(const char *name) {
    size_t len = strlen(name);
    return strncmp(name, RFS_OBS_PREFIX, strlen(RFS_OBS_PREFIX)) == 0 &&
           len < 32 && len >= 4 && strcmp(name + len - 4, ".csv") == 0;
}

/* Old files have no quality tag; never infer their historic clock quality. */
static const char *obs_file_time_quality(const char *name) {
    size_t len = strlen(name);
    if (len < 6 || name[len - 6] != '_' || strcmp(name + len - 4, ".csv") != 0) return "UNKNOWN";
    if (name[len - 5] == 'N') return "NTP_SYNCED";
    if (name[len - 5] == 'E') return "ESTIMATED";
    return "UNKNOWN";
}

bool store_get_counts(uint32_t *wifi, uint32_t *ble) {
    if (!s_mounted || !s_store_lock || !wifi || !ble) return false;
    if (xSemaphoreTake(s_store_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool valid = s_counts_valid;
    if (valid) { *wifi = s_counts_wifi; *ble = s_counts_ble; }
    xSemaphoreGive(s_store_lock);
    return valid;
}

/* Count a fixed-length snapshot plus appends recorded during the pass.
   Purges and rotation invalidate the snapshot and require a retry. */
static void store_recount_once(void) {
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    bool valid = s_counts_valid;
    if (valid || segment_initialize() != ESP_OK) {
        xSemaphoreGive(s_store_lock);
        return;
    }
    uint32_t generation = s_store_rewrite_generation;
    uint32_t last_segment = s_segment_seq;
    char active[sizeof(s_segment_name)];
    strlcpy(active, s_segment_name, sizeof(active));
    long active_size = 0;
    if (active[0]) {
        char path[80]; struct stat st;
        snprintf(path, sizeof(path), "%s/%s", RFS_OBS_DIR, active);
        if (stat(path, &st) != 0) {
            if (errno != ENOENT) { xSemaphoreGive(s_store_lock); return; }
        } else {
            if (st.st_size < 0 || st.st_size > LONG_MAX) { xSemaphoreGive(s_store_lock); return; }
            active_size = (long)st.st_size;
        }
    }
    s_recount_wifi = s_recount_ble = 0;
    s_recount_active = true;
    xSemaphoreGive(s_store_lock);
    DIR *dir = opendir(RFS_OBS_DIR);
    if (!dir) {
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        s_recount_active = false;
        xSemaphoreGive(s_store_lock);
        return;
    }
    uint32_t wifi = 0, ble = 0;
    bool complete = true;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) complete = false; break; }
        if (!obs_file(entry->d_name)) continue;
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        uint32_t number;
        if (generation != s_store_rewrite_generation) {
            xSemaphoreGive(s_store_lock); complete = false; break;
        }
        if (segment_number(entry->d_name, &number) && number > last_segment) {
            xSemaphoreGive(s_store_lock); continue;
        }
        char path[80];
        snprintf(path, sizeof(path), "%s/%.31s", RFS_OBS_DIR, entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) { xSemaphoreGive(s_store_lock); complete = false; break; }
        char buffer[512], line[RFS_LINE_MAX];
        setvbuf(f, buffer, _IOFBF, sizeof(buffer));
        unsigned ln = 0;
        int read_status = 0;
        while ((!active[0] || strcmp(active, entry->d_name) != 0 || ftell(f) < active_size) &&
               (read_status = csv_read_record(f, line, sizeof(line))) > 0) {
            if (ln++ < 2) continue;
            char *fields[14];
            if (csv_fields(line, fields, 14) == 14) {
                if (strcmp(fields[13], "WIFI") == 0) wifi++;
                else if (strcmp(fields[13], "BLE") == 0) ble++;
            }
            if (ln % 64 == 0) vTaskDelay(1);
        }
        if (ferror(f) || read_status < 0) complete = false;
        fclose(f);
        xSemaphoreGive(s_store_lock);
        if (!complete) break;
        vTaskDelay(1);
    }
    closedir(dir);
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (complete && generation == s_store_rewrite_generation) {
        s_counts_wifi = wifi + s_recount_wifi;
        s_counts_ble = ble + s_recount_ble;
        s_counts_valid = true;
    }
    s_recount_active = false;
    xSemaphoreGive(s_store_lock);
}

static void store_counts_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        store_recount_once();
    }
}

static bool observation_in_range(const char *line, time_t from, time_t to) {
    char copy[RFS_LINE_MAX], *fields[14];
    strlcpy(copy, line, sizeof(copy));
    if (csv_fields(copy, fields, 14) != 14) return false;
    struct tm tm = {0};
    if (sscanf(fields[3], "%4d-%2d-%2d %2d:%2d:%2d", &tm.tm_year, &tm.tm_mon,
               &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return false;
    tm.tm_year -= 1900; tm.tm_mon--;
    time_t t = rfs_timegm(&tm);
    return t >= from && t <= to;
}

/* Validate a complete CSV record and extract FirstSeen without copying a long
   quoted SSID into the small observation parser's fixed-size buffer. */
esp_err_t store_export_record_in_range(const char *record, time_t from, time_t to, bool *matches) {
    if (!record || !matches) return ESP_ERR_INVALID_ARG;
    *matches = false;
    char date[32]; size_t d = 0;
    int field = 0; bool quoted = false, closed = false, start = true, ended = false;
    for (const char *p = record; *p; p++) {
        char c = *p;
        if (quoted) {
            if (c == '"') {
                if (p[1] == '"') { c = '"'; p++; }
                else { quoted = false; closed = true; continue; }
            }
        } else {
            if (start && c == '"') { quoted = true; start = false; continue; }
            if (c == ',' || c == '\n' || c == '\r') {
                if (c == ',') { field++; start = true; closed = false; continue; }
                if (c == '\r') { if (p[1] != '\n') return ESP_FAIL; p++; }
                if (p[1] != '\0') return ESP_FAIL;
                ended = true; break;
            }
            if (c == '"' || closed) return ESP_FAIL;
        }
        if (field == 3) {
            if (d + 1 >= sizeof(date)) return ESP_FAIL;
            date[d++] = c;
        }
        start = false;
    }
    if (quoted || !ended || field != 13) return ESP_FAIL;
    date[d] = '\0';
    if (strcmp(date, "0") == 0 || !date[0]) return ESP_OK;
    for (size_t i = 0; i < d; i++) {
        char expected = (i == 4 || i == 7) ? '-' : (i == 10 ? ' ' : ((i == 13 || i == 16) ? ':' : 0));
        if (expected ? date[i] != expected : !isdigit((unsigned char)date[i])) return ESP_FAIL;
    }
    struct tm tm = {0}; int consumed = 0;
    if (d != 19 || sscanf(date, "%4d-%2d-%2d %2d:%2d:%2d%n", &tm.tm_year, &tm.tm_mon,
            &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &consumed) != 6 || consumed != 19)
        return ESP_FAIL;
    int year = tm.tm_year, month = tm.tm_mon;
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 1970 || month < 1 || month > 12 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59 ||
        tm.tm_mday < 1 || tm.tm_mday > days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        return ESP_FAIL;
    tm.tm_year -= 1900; tm.tm_mon--;
    time_t t = rfs_timegm(&tm);
    *matches = t >= from && t <= to;
    return ESP_OK;
}

/* Ligne d'observation predecoupee pour permettre le tri en memoire. */
#define RFS_SORT_MAX (RFS_LIST_PAGE + 1)
typedef struct {
    char mac[18];
    char ssid[33];
    char auth[16];
    char ts[20];
    int  channel;
    int  freq;
    int  rssi;
    double lat;
    double lon;
    char tsq[16];
    char type[8];
    time_t epoch;
    char filename[32];
    unsigned line;
} obs_row_t;

static const char *s_sort_key;
static int s_sort_desc;

static int obs_row_cmp(const void *a, const void *b) {
    const obs_row_t *x = (const obs_row_t *)a;
    const obs_row_t *y = (const obs_row_t *)b;
    const char *k = s_sort_key;
    int r;
    if (!k || !*k || strcmp(k, "date") == 0) {
        r = (x->epoch < y->epoch) ? -1 : (x->epoch > y->epoch ? 1 : 0);
    } else if (strcmp(k, "mac") == 0) {
        r = strcmp(x->mac, y->mac);
    } else if (strcmp(k, "ssid") == 0) {
        r = strcasecmp(x->ssid, y->ssid);
    } else if (strcmp(k, "rssi") == 0) {
        r = (x->rssi < y->rssi) ? -1 : (x->rssi > y->rssi ? 1 : 0);
    } else if (strcmp(k, "tsq") == 0) {
        r = strcmp(x->tsq, y->tsq);
    } else if (strcmp(k, "type") == 0) {
        r = strcmp(x->type, y->type);
    } else {
        r = (x->epoch < y->epoch) ? -1 : (x->epoch > y->epoch ? 1 : 0);
    }
    if (s_sort_desc) r = -r;
    if (!r) r = strcmp(x->filename, y->filename);
    if (!r) r = (x->line > y->line) - (x->line < y->line);
    return r;
}

static void obs_row_parse(obs_row_t *r, char **fields, const char *tsq, const char *filename, unsigned line) {
    memset(r, 0, sizeof(*r));
    strlcpy(r->mac, fields[0], sizeof(r->mac)); strlcpy(r->ssid, fields[1], sizeof(r->ssid));
    strlcpy(r->auth, fields[2], sizeof(r->auth)); strlcpy(r->ts, fields[3], sizeof(r->ts));
    r->channel = atoi(fields[4]); r->freq = atoi(fields[5]); r->rssi = atoi(fields[6]);
    r->lat = atof(fields[7]); r->lon = atof(fields[8]);
    strlcpy(r->tsq, tsq, sizeof(r->tsq)); strlcpy(r->type, fields[13], sizeof(r->type));
    strlcpy(r->filename, filename, sizeof(r->filename)); r->line = line;
    struct tm tm = {0};
    if (sscanf(fields[3], "%4d-%2d-%2d %2d:%2d:%2d", &tm.tm_year, &tm.tm_mon,
               &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900; tm.tm_mon--; r->epoch = rfs_timegm(&tm);
    }
}

esp_err_t store_list(int page, const char *type, const rfs_list_opt_t *opt, char *out, size_t n) {
    if (!s_mounted || !out || n < 1024 || !s_store_lock) return ESP_ERR_INVALID_STATE;
    if (page < 1) page = 1;
    if (page > 100000) return ESP_ERR_INVALID_ARG;

    const char *type_s = (type && type[0]) ? type : NULL;
    const char *sort_s = (opt && opt->sort && opt->sort[0]) ? opt->sort : NULL;
    const char *dir_s  = (opt && opt->dir && opt->dir[0]) ? opt->dir : NULL;
    time_t from = opt ? opt->from : 0;
    time_t to   = opt ? opt->to : 0;
    bool use_filter = (from > 0) || (to > 0);
    if (use_filter && to <= 0) to = (time_t)0x7fffffff;
    bool use_sort = (sort_s != NULL);

    obs_row_t *rows = NULL;
    size_t rn = 0;
    bool truncated = false;
    obs_row_t anchor;
    bool has_anchor = false;
    if (use_sort) {
        rows = malloc(RFS_SORT_MAX * sizeof(obs_row_t));
        if (!rows) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    uint32_t list_generation = s_store_rewrite_generation;
    s_sort_key = sort_s;
    s_sort_desc = (dir_s && strcmp(dir_s, "desc") == 0);
    if (use_sort && opt && opt->cursor && opt->cursor[0]) {
        char name[32], extra; unsigned anchor_line;
        if (sscanf(opt->cursor, "%31[^:]:%u%c", name, &anchor_line, &extra) != 2 ||
            !obs_file(name) || strchr(name, '/') || strchr(name, '\\') || anchor_line < 3) {
            free(rows); xSemaphoreGive(s_store_lock); return ESP_ERR_INVALID_ARG;
        }
        char path[80]; snprintf(path, sizeof(path), "%s/%.31s", RFS_OBS_DIR, name);
        FILE *af = fopen(path, "r");
        char line[RFS_LINE_MAX]; unsigned i = 0; char *fields[14];
        if (af) { while (i < anchor_line && csv_read_record(af, line, sizeof(line)) > 0) i++; fclose(af); }
        if (!af || i != anchor_line || csv_fields(line, fields, 14) != 14) {
            free(rows); xSemaphoreGive(s_store_lock); return ESP_ERR_INVALID_STATE;
        }
        obs_row_parse(&anchor, fields, obs_file_time_quality(name), name, anchor_line);
        has_anchor = true;
    }
    DIR *dir = opendir(RFS_OBS_DIR);
    if (!dir) { if (rows) free(rows); xSemaphoreGive(s_store_lock); return ESP_FAIL; }
    size_t o = (size_t)snprintf(out, n, "{\"observations\":[");
    unsigned total = 0, shown = 0;
    bool has_next = false;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno) { closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_FAIL; }
            break;
        }
        if (!obs_file(entry->d_name)) continue;
        char path[80], filename[32];
        strlcpy(filename, entry->d_name, sizeof(filename));
        snprintf(path, sizeof(path), "%s/%s", RFS_OBS_DIR, filename);
        FILE *f = fopen(path, "r");
        if (!f) { closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_FAIL; }
        char read_buffer[512];
        setvbuf(f, read_buffer, _IOFBF, sizeof(read_buffer));
        char line[RFS_LINE_MAX];
        int ln = 0;
        const char *tsq = obs_file_time_quality(filename);
        int read_status = 0;
        while ((read_status = csv_read_record(f, line, sizeof(line))) > 0) {
            if (ln++ < 2) continue;
            if ((ln & 63) == 0) {
                long offset = ftell(f);
                bool failed = ferror(f) || offset < 0;
                if (fclose(f) != 0) failed = true;
                if (failed) { closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_FAIL; }
                xSemaphoreGive(s_store_lock);
                vTaskDelay(1);
                xSemaphoreTake(s_store_lock, portMAX_DELAY);
                if (list_generation != s_store_rewrite_generation) {
                    closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_ERR_INVALID_STATE;
                }
                s_sort_key = sort_s;
                s_sort_desc = (dir_s && strcmp(dir_s, "desc") == 0);
                f = fopen(path, "r");
                if (!f) { closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_FAIL; }
                setvbuf(f, read_buffer, _IOFBF, sizeof(read_buffer));
                if (fseek(f, offset, SEEK_SET) != 0) {
                    fclose(f); closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_FAIL;
                }
            }
            if (use_filter && !observation_in_range(line, from, to)) continue;
            char *fields[14];
            if (csv_fields(line, fields, 14) != 14) continue;
            if (type_s &&
                ((strcmp(type_s, "wifi") == 0 && strcmp(fields[13], "WIFI") != 0) ||
                 (strcmp(type_s, "ble") == 0 && strcmp(fields[13], "BLE") != 0))) continue;
            if (use_sort) {
                obs_row_t candidate;
                obs_row_parse(&candidate, fields, tsq, filename, (unsigned)ln);
                if (has_anchor && obs_row_cmp(&candidate, &anchor) <= 0) continue;
                size_t at = 0;
                while (at < rn && obs_row_cmp(&rows[at], &candidate) <= 0) at++;
                if (at < RFS_SORT_MAX) {
                    size_t end = rn < RFS_SORT_MAX ? rn++ : RFS_SORT_MAX - 1;
                    if (end > at) memmove(&rows[at + 1], &rows[at], (end - at) * sizeof(*rows));
                    rows[at] = candidate;
                }
                continue;
            }
            unsigned idx = total++;
            if (idx >= (unsigned)page * RFS_LIST_PAGE) { has_next = true; break; }
            if (idx < (unsigned)(page - 1) * RFS_LIST_PAGE || shown >= RFS_LIST_PAGE) continue;
            char mac[64], ssid[256], auth[64], ts[64], typ[32];
            json_escape(fields[0], mac, sizeof(mac));
            json_escape(fields[1], ssid, sizeof(ssid));
            json_escape(fields[2], auth, sizeof(auth));
            json_escape(fields[3], ts, sizeof(ts));
            json_escape(fields[13], typ, sizeof(typ));
            int w = snprintf(out + o, n - o,
                "%s{\"mac\":\"%s\",\"ssid\":\"%s\",\"auth\":\"%s\",\"ts\":\"%s\","
                "\"channel\":%d,\"freq\":%d,\"rssi\":%d,\"lat\":%.5f,\"lon\":%.5f,"
                "\"tsq\":\"%s\",\"type\":\"%s\"}",
                shown ? "," : "", mac, ssid, auth, ts, atoi(fields[4]),
                atoi(fields[5]), atoi(fields[6]), atof(fields[7]), atof(fields[8]), tsq, typ);
            if (w < 0 || (size_t)w >= n - o - 128) { fclose(f); closedir(dir); if (rows) free(rows); xSemaphoreGive(s_store_lock); return ESP_ERR_NO_MEM; }
            o += (size_t)w;
            shown++;
        }
        bool read_failed = ferror(f) || read_status < 0;
        fclose(f);
        if (read_failed) { closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_FAIL; }
        if (has_next && !use_sort) break;
        xSemaphoreGive(s_store_lock);
        vTaskDelay(1);
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        if (list_generation != s_store_rewrite_generation) { closedir(dir); free(rows); xSemaphoreGive(s_store_lock); return ESP_ERR_INVALID_STATE; }
        s_sort_key = sort_s;
        s_sort_desc = (dir_s && strcmp(dir_s, "desc") == 0);
    }
    closedir(dir);

    unsigned pages;
    if (use_sort) {
        size_t start = 0;
        has_next = rn > RFS_LIST_PAGE;
        if (has_next) rn = RFS_LIST_PAGE;
        char next_cursor[48] = "";
        if (rn) snprintf(next_cursor, sizeof(next_cursor), "%s:%u", rows[rn - 1].filename, rows[rn - 1].line);
        for (size_t i = start; i < rn && shown < RFS_LIST_PAGE; i++) {
            obs_row_t *r = &rows[i];
            char mac[64], ssid[256], auth[64], ts[64], typ[32], tsq[32];
            json_escape(r->mac, mac, sizeof(mac));
            json_escape(r->ssid, ssid, sizeof(ssid));
            json_escape(r->auth, auth, sizeof(auth));
            json_escape(r->ts, ts, sizeof(ts));
            json_escape(r->tsq, tsq, sizeof(tsq));
            json_escape(r->type, typ, sizeof(typ));
            int w = snprintf(out + o, n - o,
                "%s{\"mac\":\"%s\",\"ssid\":\"%s\",\"auth\":\"%s\",\"ts\":\"%s\","
                "\"channel\":%d,\"freq\":%d,\"rssi\":%d,\"lat\":%.5f,\"lon\":%.5f,"
                "\"tsq\":\"%s\",\"type\":\"%s\"}",
                shown ? "," : "", mac, ssid, auth, ts, r->channel, r->freq, r->rssi,
                r->lat, r->lon, tsq, typ);
            if (w < 0 || (size_t)w >= n - o - 128) { free(rows); xSemaphoreGive(s_store_lock); return ESP_ERR_NO_MEM; }
            o += (size_t)w;
            shown++;
        }
        pages = has_next ? (unsigned)page + 1 : (unsigned)page;
        if (snprintf(out + o, n - o, "],\"page\":%d,\"pages\":%u,\"has_next\":%s,\"truncated\":false,\"next_cursor\":\"%s\"}",
                     page, pages, has_next ? "true" : "false", next_cursor) >= (int)(n - o)) {
            free(rows); xSemaphoreGive(s_store_lock); return ESP_ERR_NO_MEM;
        }
        free(rows); xSemaphoreGive(s_store_lock); return ESP_OK;
    } else {
        pages = has_next ? (unsigned)page + 1 : (total + RFS_LIST_PAGE - 1) / RFS_LIST_PAGE;
        if (!pages) pages = 1;
    }

    snprintf(out + o, n - o, "],\"page\":%d,\"pages\":%u,\"has_next\":%s,\"truncated\":%s}",
             page, pages, has_next ? "true" : "false", truncated ? "true" : "false");
    xSemaphoreGive(s_store_lock);
    return ESP_OK;
}

esp_err_t store_history(const char *mac, char *out, size_t n) {
    if (!s_mounted || !s_store_lock || !mac || !out || n < 128) return ESP_ERR_INVALID_ARG;
    typedef struct { obs_row_t row; int acc, mfgr; } history_row_t;
    history_row_t *rows = calloc(RFS_HIST_MAX, sizeof(*rows));
    if (!rows) return ESP_ERR_NO_MEM;
    char macn[24]; strlcpy(macn, mac, sizeof(macn));
    for (char *p = macn; *p; p++) *p = (char)toupper((unsigned char)*p);
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    uint32_t generation = s_store_rewrite_generation;
    DIR *dir = opendir(RFS_OBS_DIR);
    if (!dir) { xSemaphoreGive(s_store_lock); free(rows); return ESP_FAIL; }
    esp_err_t result = ESP_OK;
    int count = 0;
    bool truncated = false;
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        if (!obs_file(entry->d_name)) continue;
        char filename[32], path[80];
        strlcpy(filename, entry->d_name, sizeof(filename));
        snprintf(path, sizeof(path), "%s/%s", RFS_OBS_DIR, filename);
        FILE *f = fopen(path, "r");
        if (!f) { result = ESP_FAIL; break; }
        char read_buffer[512], line[RFS_LINE_MAX];
        setvbuf(f, read_buffer, _IOFBF, sizeof(read_buffer));
        unsigned ln = 0; int read_status = 0;
        while ((read_status = csv_read_record(f, line, sizeof(line))) > 0) {
            ln++;
            if (ln <= 2) continue;
            if ((ln & 63) == 0) {
                long offset = ftell(f);
                bool failed = ferror(f) || offset < 0;
                if (fclose(f) != 0) failed = true;
                if (failed) { f = NULL; result = ESP_FAIL; break; }
                xSemaphoreGive(s_store_lock); vTaskDelay(1); xSemaphoreTake(s_store_lock, portMAX_DELAY);
                if (generation != s_store_rewrite_generation) { f = NULL; result = ESP_ERR_INVALID_STATE; break; }
                f = fopen(path, "r");
                if (!f) { result = ESP_FAIL; break; }
                setvbuf(f, read_buffer, _IOFBF, sizeof(read_buffer));
                if (fseek(f, offset, SEEK_SET) != 0) { result = ESP_FAIL; break; }
            }
            char *fields[14];
            if (csv_fields(line, fields, 14) != 14) continue;
            for (char *p = fields[0]; *p; p++) *p = (char)toupper((unsigned char)*p);
            if (strcmp(fields[0], macn)) continue;
            history_row_t candidate = {0};
            obs_row_parse(&candidate.row, fields, obs_file_time_quality(filename), filename, ln);
            candidate.acc = atoi(fields[10]); candidate.mfgr = atoi(fields[12]);
            int pos = 0;
            while (pos < count && rows[pos].row.epoch >= candidate.row.epoch) pos++;
            if (count >= RFS_HIST_MAX) truncated = true;
            if (pos >= RFS_HIST_MAX) continue;
            int end = count < RFS_HIST_MAX ? count++ : RFS_HIST_MAX - 1;
            if (end > pos) memmove(&rows[pos + 1], &rows[pos], (size_t)(end - pos) * sizeof(*rows));
            rows[pos] = candidate;
        }
        if (f) { if (ferror(f) || read_status < 0) result = ESP_FAIL; if (fclose(f) != 0) result = ESP_FAIL; }
        if (result != ESP_OK) break;
        xSemaphoreGive(s_store_lock); vTaskDelay(1); xSemaphoreTake(s_store_lock, portMAX_DELAY);
        if (generation != s_store_rewrite_generation) { result = ESP_ERR_INVALID_STATE; break; }
    }
    closedir(dir); xSemaphoreGive(s_store_lock);
    if (result != ESP_OK) { free(rows); return result; }
    size_t o = (size_t)snprintf(out, n, "{\"observations\":[");
    int shown = 0;
    for (int i = 0; i < count; i++) {
        obs_row_t *r = &rows[i].row;
        char ssid[256], mac_j[64], auth[64], ts[64], type[32];
        json_escape(r->ssid, ssid, sizeof(ssid)); json_escape(r->mac, mac_j, sizeof(mac_j));
        json_escape(r->auth, auth, sizeof(auth)); json_escape(r->ts, ts, sizeof(ts)); json_escape(r->type, type, sizeof(type));
        int w = snprintf(out + o, n - o,
            "%s{\"mac\":\"%s\",\"ssid\":\"%s\",\"auth\":\"%s\",\"ts\":\"%s\","
            "\"channel\":%d,\"freq\":%d,\"rssi\":%d,\"lat\":%.5f,\"lon\":%.5f,"
            "\"acc\":%d,\"mfgr\":%d,\"type\":\"%s\"}",
            shown ? "," : "", mac_j, ssid, auth, ts, r->channel, r->freq, r->rssi,
            r->lat, r->lon, rows[i].acc, rows[i].mfgr, type);
        if (w < 0 || (size_t)w + 32 >= n - o) { truncated = true; break; }
        o += (size_t)w; shown++;
    }
    snprintf(out + o, n - o, "],\"truncated\":%s}", truncated ? "true" : "false");
    free(rows); return ESP_OK;
}

/* Purge matches observation timestamps, not file names. Each file is replaced
 * atomically after its surviving records have been written. */
esp_err_t store_purge(time_t from, time_t to, int *removed) {
    if (!s_mounted || !s_store_lock) return ESP_ERR_INVALID_STATE;
    if (removed) *removed = 0;
    if (from <= 0 || to <= 0 || from > to) return ESP_ERR_INVALID_ARG;
    DIR *dir = opendir(RFS_OBS_DIR);
    if (!dir) return ESP_FAIL;
    int deleted = 0;
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        if (!obs_file(entry->d_name)) continue;
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        char path[80], tmp[90];
        snprintf(path, sizeof(path), "%s/%.31s", RFS_OBS_DIR, entry->d_name);
        snprintf(tmp, sizeof(tmp), "%s/.purge.tmp", RFS_MOUNT);
        FILE *in = fopen(path, "r");
        if (!in) { xSemaphoreGive(s_store_lock); result = ESP_FAIL; break; }
        char read_buffer[512];
        setvbuf(in, read_buffer, _IOFBF, sizeof(read_buffer));
        char line[RFS_LINE_MAX];
        int scan_line = 0;
        bool any_match = false;
        int read_status = 0;
        while ((read_status = csv_read_record(in, line, sizeof(line))) > 0) {
            if (scan_line++ >= 2 && observation_in_range(line, from, to)) { any_match = true; break; }
            if (scan_line % 64 == 0) vTaskDelay(1);
        }
        if (ferror(in) || read_status < 0) { fclose(in); xSemaphoreGive(s_store_lock); result = ESP_FAIL; break; }
        if (!any_match) { fclose(in); xSemaphoreGive(s_store_lock); vTaskDelay(1); continue; }
        if (fseek(in, 0, SEEK_SET) != 0) { fclose(in); xSemaphoreGive(s_store_lock); result = ESP_FAIL; break; }
        FILE *out = fopen(tmp, "w");
        if (!out) { fclose(in); xSemaphoreGive(s_store_lock); result = ESP_FAIL; break; }
        char write_buffer[512];
        setvbuf(out, write_buffer, _IOFBF, sizeof(write_buffer));
        int ln = 0, removed_file = 0, remaining = 0;
        bool failed = false;
        read_status = 0;
        while ((read_status = csv_read_record(in, line, sizeof(line))) > 0) {
            if (ln && ln % 64 == 0) vTaskDelay(1);
            bool drop = false;
            if (ln++ >= 2) {
                drop = observation_in_range(line, from, to);
                if (drop) removed_file++; else remaining++;
            }
            if (!drop && fputs(line, out) == EOF) { failed = true; break; }
        }
        if (ferror(in) || read_status < 0) failed = true;
        fclose(in);
        if (fclose(out) != 0) failed = true;
        if (failed) { unlink(tmp); xSemaphoreGive(s_store_lock); result = ESP_FAIL; break; }
        if (!removed_file) { unlink(tmp); xSemaphoreGive(s_store_lock); vTaskDelay(1); continue; }
        if (rename(tmp, path) != 0) { unlink(tmp); xSemaphoreGive(s_store_lock); result = ESP_FAIL; break; }
        deleted += removed_file;
        s_store_generation++;
        s_store_rewrite_generation++;
        s_counts_valid = false;
        /* Empty files no longer need a directory entry; remove them while locked. */
        if (!remaining && unlink(path) != 0) {
            xSemaphoreGive(s_store_lock);
            result = ESP_FAIL;
            break;
        }
        xSemaphoreGive(s_store_lock);
        vTaskDelay(1);
    }
    closedir(dir);
    if (removed) *removed = deleted;
    if (deleted > 0) {
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        s_seen_count = 0;
        xSemaphoreGive(s_store_lock);
    }
    return result;
}

/* ---- export complet du journal ---- */

/* Export en lecture sequentielle. Un seul en-tete CSV global est emis, puis les
   lignes de data de chaque segment dans l'ordre chronologique ; les deux lignes
   d'en-tete de chaque segment sont sautees. Le fichier obtenu est donc un CSV
   WiGLE valide, telechargeable puis re-televersable tel quel sur wigle.net.

   La taille du segment actif est figee a l'ouverture ; les segments crees
   ensuite sont exclus. La taille calculee sert aussi a l'envoi HTTPS WiGLE. */
/* The snapshot stores only the sequence boundary and the active segment size.
   Old files are immutable except for purge/rotation, which changes generation. */
#define EXPORT_NAME_BATCH 16
struct store_export {
    FILE *f;
    char fb[512];
    char active[32], previous[32];
    char names[EXPORT_NAME_BATCH][32];
    unsigned name_count, name_index;
    long active_size, remaining, total, delivered;
    uint32_t last_segment, rewrite_generation;
    bool header_done;
    uint8_t ble_filter;
    char input[512], record[512];
    size_t input_at, input_len, record_len, record_at;
    long filtered_total;
    unsigned record_number;
    bool quoted, raw_eof, record_ready;
};

static esp_err_t export_filtered_read(store_export_t *h, char *buf, size_t n, size_t *read);
static esp_err_t export_raw_read(store_export_t *h, char *buf, size_t n, size_t *read);

static long export_header_bytes(FILE *f) {
    char line[RFS_LINE_MAX];
    long bytes = 0;
    for (int i = 0; i < 2; i++) {
        if (!fgets(line, sizeof(line), f) || !strchr(line, '\n')) return -1;
        bytes += (long)strlen(line);
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (i == 0 && strncmp(line, "WigleWifi-", 10) != 0) return -1;
        if (i == 1) {
            const char *expected = strchr(CSV_HDR, '\n') + 1;
            if (strlen(expected) != len + 1 || strncmp(line, expected, len) != 0) return -1;
        }
    }
    return bytes;
}

/* Called with the store lock held. New sequenced files are outside the snapshot. */
static bool export_in_snapshot(const store_export_t *h, const char *name) {
    uint32_t number;
    if (h->active[0] && h->active_size == 0 && strcmp(h->active, name) == 0) return false;
    return obs_file(name) && (!segment_number(name, &number) || number <= h->last_segment);
}

static esp_err_t export_file_size(store_export_t *h, const char *name, long *skip, long *len) {
    char path[80]; struct stat st;
    snprintf(path, sizeof(path), "%s/%.31s", RFS_OBS_DIR, name);
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > LONG_MAX) return ESP_FAIL;
    long size = (long)st.st_size;
    if (h->active[0] && strcmp(name, h->active) == 0) {
        if (size < h->active_size) return ESP_ERR_INVALID_STATE;
        size = h->active_size;
    }
    FILE *f = fopen(path, "r");
    if (!f) return ESP_FAIL;
    *skip = export_header_bytes(f);
    bool failed = ferror(f);
    fclose(f);
    if (failed || *skip < 0 || *skip > size) return ESP_FAIL;
    *len = size - *skip;
    return ESP_OK;
}

esp_err_t store_export_open(store_export_t **out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_mounted || !s_store_lock) return ESP_ERR_INVALID_STATE;
    *out = NULL;
    store_export_t *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    h->rewrite_generation = s_store_rewrite_generation;
    h->ble_filter = config_get()->ble_address_filter;
    if (segment_initialize() != ESP_OK) result = ESP_FAIL;
    h->last_segment = s_segment_seq;
    strlcpy(h->active, s_segment_name, sizeof(h->active));
    if (result == ESP_OK && h->active[0]) {
        char path[80]; struct stat st;
        snprintf(path, sizeof(path), "%s/%s", RFS_OBS_DIR, h->active);
        if (stat(path, &st) != 0) {
            if (errno != ENOENT) result = ESP_FAIL;
            else h->active_size = 0;
        } else if (st.st_size <= 0 || st.st_size > LONG_MAX) result = ESP_FAIL;
        else h->active_size = (long)st.st_size;
    }
    DIR *dir = result == ESP_OK ? opendir(RFS_OBS_DIR) : NULL;
    if (result == ESP_OK && !dir) result = ESP_FAIL;
    h->total = (long)strlen(CSV_HDR);
    struct dirent *entry;
    unsigned checked = 0;
    while (result == ESP_OK) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        if (!export_in_snapshot(h, entry->d_name)) continue;
        long skip, len;
        result = export_file_size(h, entry->d_name, &skip, &len);
        if (result != ESP_OK) break;
        if (len > LONG_MAX - h->total) { result = ESP_ERR_INVALID_SIZE; break; }
        h->total += len;
        if (++checked % 8 == 0) {
            xSemaphoreGive(s_store_lock);
            vTaskDelay(1);
            xSemaphoreTake(s_store_lock, portMAX_DELAY);
            if (h->rewrite_generation != s_store_rewrite_generation) result = ESP_ERR_INVALID_STATE;
        }
    }
    if (dir) closedir(dir);
    xSemaphoreGive(s_store_lock);
    if (result != ESP_OK) { free(h); return result; }
    /* HTTP multipart requires an exact length. Scan the same bounded snapshot
       once before sending, then rewind it; rotation invalidates either pass. */
    char scratch[512]; size_t got = 0;
    while ((result = export_filtered_read(h, scratch, sizeof(scratch), &got)) == ESP_OK && got) {
        if ((long)got > LONG_MAX - h->filtered_total) { result = ESP_ERR_INVALID_SIZE; break; }
        h->filtered_total += (long)got;
    }
    if (h->f) {
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        fclose(h->f); h->f = NULL;
        xSemaphoreGive(s_store_lock);
    }
    if (result != ESP_OK) { free(h); return result; }
    h->previous[0] = '\0'; h->name_count = h->name_index = 0;
    h->remaining = h->delivered = 0; h->header_done = false;
    h->input_at = h->input_len = h->record_len = h->record_at = 0;
    h->record_number = 0; h->quoted = h->raw_eof = h->record_ready = false;
    *out = h;
    return ESP_OK;
}

long store_export_size(store_export_t *h) { return h ? h->filtered_total : 0; }
bool store_export_has_data(store_export_t *h) { return h && h->filtered_total > (long)strlen(CSV_HDR); }

/* Find the next name without storing a per-file table. This trades extra
   directory traversals for a fixed memory footprint even with many segments. */
static esp_err_t export_next(store_export_t *h, bool *found) {
    *found = false;
    if (h->name_index == h->name_count) {
        DIR *dir = opendir(RFS_OBS_DIR);
        if (!dir) return ESP_FAIL;
        h->name_count = h->name_index = 0;
        struct dirent *entry;
        for (;;) {
            errno = 0;
            entry = readdir(dir);
            if (!entry) { if (errno) { closedir(dir); return ESP_FAIL; } break; }
            const char *name = entry->d_name;
            if (!export_in_snapshot(h, name) || strcmp(name, h->previous) <= 0) continue;
            unsigned at = 0;
            while (at < h->name_count && strcmp(h->names[at], name) < 0) at++;
            if (at >= EXPORT_NAME_BATCH) continue;
            unsigned end = h->name_count < EXPORT_NAME_BATCH ? h->name_count++ : EXPORT_NAME_BATCH - 1;
            if (end > at) memmove(h->names[at + 1], h->names[at], (end - at) * sizeof(h->names[0]));
            strlcpy(h->names[at], name, sizeof(h->names[0]));
        }
        closedir(dir);
    }
    if (h->name_index == h->name_count) return ESP_OK;
    const char *next = h->names[h->name_index++];
    long skip, len;
    esp_err_t err = export_file_size(h, next, &skip, &len);
    if (err != ESP_OK) return err;
    strlcpy(h->previous, next, sizeof(h->previous));
    *found = true;
    if (!len) return ESP_OK;
    char path[80]; snprintf(path, sizeof(path), "%s/%.31s", RFS_OBS_DIR, next);
    h->f = fopen(path, "r");
    if (!h->f) return ESP_FAIL;
    setvbuf(h->f, h->fb, _IOFBF, sizeof(h->fb));
    if (fseek(h->f, skip, SEEK_SET) != 0) return ESP_FAIL;
    h->remaining = len;
    return ESP_OK;
}

static esp_err_t export_raw_read(store_export_t *h, char *buf, size_t n, size_t *read) {
    if (!h || !buf || !read) return ESP_ERR_INVALID_ARG;
    *read = 0;
    size_t produced = 0;
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (h->rewrite_generation != s_store_rewrite_generation) { result = ESP_ERR_INVALID_STATE; goto done; }
    if (!h->header_done) {
        size_t hl = strlen(CSV_HDR);
        if (n < hl) { result = ESP_ERR_INVALID_SIZE; goto done; }
        memcpy(buf, CSV_HDR, hl); produced = hl; h->header_done = true;
    }
    while (produced < n) {
        if (!h->f) {
            bool found;
            result = export_next(h, &found);
            if (result != ESP_OK) goto done;
            if (!found) {
                if (h->delivered + (long)produced != h->total) result = ESP_ERR_INVALID_STATE;
                break;
            }
            if (!h->f) continue;
        }
        size_t want = n - produced;
        if ((long)want > h->remaining) want = (size_t)h->remaining;
        size_t got = fread(buf + produced, 1, want, h->f);
        if (ferror(h->f) || got == 0) { result = ESP_FAIL; goto done; }
        produced += got; h->remaining -= (long)got;
        if (h->remaining == 0) { fclose(h->f); h->f = NULL; }
    }
    if (h->delivered + (long)produced > h->total) result = ESP_ERR_INVALID_STATE;
done:
    if (result == ESP_OK) { h->delivered += (long)produced; *read = produced; }
    xSemaphoreGive(s_store_lock);
    taskYIELD();
    return result;
}

/* Remove internal address-type metadata from the RCOIs field in the exported
   CSV; old BLE records have unknown type and cannot pass restrictive modes. */
static esp_err_t export_filter_record(store_export_t *h, bool *keep) {
    *keep = true;
    if (h->record_number++ < 2) return ESP_OK;
    char *begin[14] = {0}, *end[14] = {0};
    unsigned field = 0; bool quoted = false;
    begin[0] = h->record;
    for (size_t i = 0; i < h->record_len; i++) {
        char c = h->record[i];
        if (c == '"') quoted = !quoted;
        if (!quoted && (c == ',' || c == '\n' || c == '\r')) {
            if (field >= 14) return ESP_FAIL;
            end[field] = h->record + i;
            if (c == ',') { if (++field >= 14) return ESP_FAIL; begin[field] = h->record + i + 1; }
            else break;
        }
    }
    if (quoted || field != 13 || !end[13]) return ESP_FAIL;
    bool ble = end[13] - begin[13] == 3 && !strncmp(begin[13], "BLE", 3);
    if (!ble) return ESP_OK;
    size_t m = (size_t)(end[11] - begin[11]);
    rfs_ble_address_t type = RFS_BLE_UNKNOWN;
    if (m == 10 && !strncmp(begin[11], "BLE-PUBLIC", m)) type = RFS_BLE_PUBLIC;
    else if (m == 10 && !strncmp(begin[11], "BLE-STATIC", m)) type = RFS_BLE_STATIC;
    else if (m == 7 && !strncmp(begin[11], "BLE-RPA", m)) type = RFS_BLE_RPA;
    else if (m == 8 && !strncmp(begin[11], "BLE-NRPA", m)) type = RFS_BLE_NRPA;
    *keep = rfs_ble_filter_allows(h->ble_filter, type);
    if (m && (type != RFS_BLE_UNKNOWN || (m == 11 && !strncmp(begin[11], "BLE-UNKNOWN", m)))) {
        memmove(begin[11], end[11], h->record_len - (size_t)(end[11] - h->record));
        h->record_len -= m;
    }
    return ESP_OK;
}

static esp_err_t export_filtered_read(store_export_t *h, char *buf, size_t n, size_t *read) {
    *read = 0;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    bool changed = h->rewrite_generation != s_store_rewrite_generation;
    xSemaphoreGive(s_store_lock);
    if (changed) return ESP_ERR_INVALID_STATE;
    while (*read < n) {
        if (h->record_ready && h->record_at < h->record_len) {
            size_t amount = h->record_len - h->record_at;
            if (amount > n - *read) amount = n - *read;
            memcpy(buf + *read, h->record + h->record_at, amount);
            h->record_at += amount; *read += amount;
            if (h->record_at < h->record_len) break;
            h->record_at = h->record_len = 0; h->record_ready = false;
        }
        if (h->input_at == h->input_len && !h->raw_eof) {
            esp_err_t err = export_raw_read(h, h->input, sizeof(h->input), &h->input_len);
            if (err != ESP_OK) return err;
            h->input_at = 0;
            if (!h->input_len) h->raw_eof = true;
        }
        if (h->raw_eof) return h->record_len || h->quoted ? ESP_FAIL : ESP_OK;
        char c = h->input[h->input_at++];
        if (h->record_len == sizeof(h->record)) return ESP_ERR_INVALID_SIZE;
        h->record[h->record_len++] = c;
        if (c == '"') h->quoted = !h->quoted;
        if (c == '\n' && !h->quoted) {
            bool keep; esp_err_t err = export_filter_record(h, &keep);
            if (err != ESP_OK) return err;
            if (!keep) h->record_len = 0;
            else h->record_ready = true;
        } else {
            /* The record isn't complete yet; do not expose partial data. */
            continue;
        }
    }
    return ESP_OK;
}

esp_err_t store_export_read(store_export_t *h, char *buf, size_t n, size_t *read) {
    if (!h || !buf || !read) return ESP_ERR_INVALID_ARG;
    return export_filtered_read(h, buf, n, read);
}

void store_export_close(store_export_t *h) {
    if (!h) return;
    if (h->f) { xSemaphoreTake(s_store_lock, portMAX_DELAY); fclose(h->f); xSemaphoreGive(s_store_lock); }
    free(h);
}

esp_err_t store_stats(size_t *total, size_t *freeb, int *files) {
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    size_t t = 0, f = 0;
    esp_err_t err = esp_littlefs_info("storage", &t, &f);
    if (total) *total = t;
    if (freeb) *freeb = (t > f) ? (t - f) : 0;
    if (files) {
        char names[RFS_OBS_MAX_FILES][32];
        *files = list_obs_files(names, RFS_OBS_MAX_FILES);
    }
    return err;
}

/* ---- blacklist / exclusion ---- */

static int bl_load(const char *key, char *out, size_t n) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return -1;
    size_t sz = n;
    err = nvs_get_str(h, key, out, &sz);
    nvs_close(h);
    if (err != ESP_OK) return -1;
    return 0;
}

/* index des MAC exclues en RAM: evite un nvs_open par observation */
static void bl_cache_load(void) {
    if (s_bl_loaded) return;
    s_bl_count = 0;
    char idx[BL_INDEX_MAX];
    if (bl_load(K_BL_INDEX, idx, sizeof(idx)) == 0) {
        char *save = NULL;
        for (char *tok = strtok_r(idx, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
            if (tok[0] == '\0') continue;
            if (s_bl_count >= BL_CACHE_MAX) break;
            strlcpy(s_bl_mac[s_bl_count], tok, sizeof(s_bl_mac[0]));
            s_bl_count++;
        }
    }
    s_bl_loaded = true;
}

static void bl_cache_invalidate(void) {
    s_bl_loaded = false;
    s_bl_count = 0;
}

bool store_is_blacklisted(const char *mac) {
    if (!s_bl_lock || !mac || mac[0] == '\0') return false;
    char macu[24];
    strlcpy(macu, mac, sizeof(macu));
    for (char *p = macu; *p; p++) *p = (char)toupper((unsigned char)*p);

    xSemaphoreTake(s_bl_lock, portMAX_DELAY);
    bl_cache_load();
    bool found = false;
    for (int i = 0; i < s_bl_count; i++) {
        if (strcmp(s_bl_mac[i], macu) == 0) { found = true; break; }
    }
    xSemaphoreGive(s_bl_lock);
    return found;
}

/* NVS keys are limited to 15 characters. Store labels under bl_ + 12 hex
 * digits while retaining colon-separated MACs in the existing index. */
static bool blacklist_key(const char *mac, char canonical[18], char key[16]) {
    if (!mac || strlen(mac) != 17) return false;
    memcpy(key, K_BL_PREFIX, 3);
    int k = 3;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
            canonical[i] = ':';
        } else {
            if (!isxdigit((unsigned char)mac[i])) return false;
            canonical[i] = (char)toupper((unsigned char)mac[i]);
            key[k++] = canonical[i];
        }
    }
    canonical[17] = '\0'; key[k] = '\0';
    return true;
}

esp_err_t store_blacklist_add(const char *mac, const char *label) {
    char macu[18], key[16];
    if (!blacklist_key(mac, macu, key)) return ESP_ERR_INVALID_ARG;
    if (!s_bl_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bl_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { xSemaphoreGive(s_bl_lock); return err; }
    char idx[BL_INDEX_MAX] = {0}, copy[BL_INDEX_MAX];
    size_t isz = sizeof(idx);
    err = nvs_get_str(h, K_BL_INDEX, idx, &isz);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err != ESP_OK) goto done_add;
    strlcpy(copy, idx, sizeof(copy));
    bool found = false;
    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        count++;
        if (strcmp(tok, macu) == 0) found = true;
    }
    if (!found) {
        if (count >= BL_CACHE_MAX || strlen(idx) + (idx[0] ? 1 : 0) + strlen(macu) + 1 > sizeof(idx)) {
            err = ESP_ERR_NO_MEM;
            goto done_add;
        }
        if (idx[0]) strlcat(idx, ",", sizeof(idx));
        strlcat(idx, macu, sizeof(idx));
    }
    if (label && label[0]) {
        char lab[32];
        size_t len = strlen(label);
        if (len >= sizeof(lab)) {
            len = sizeof(lab) - 1;
            while (len > 0 && (((unsigned char)label[len] & 0xc0) == 0x80)) len--;
        }
        memcpy(lab, label, len);
        lab[len] = '\0';
        err = nvs_set_str(h, key, lab);
    }
    if (err == ESP_OK && !found) err = nvs_set_str(h, K_BL_INDEX, idx);
    if (err == ESP_OK) err = nvs_commit(h);
done_add:
    bl_cache_invalidate();
    nvs_close(h);
    xSemaphoreGive(s_bl_lock);
    return err;
}

esp_err_t store_blacklist_remove(const char *mac) {
    char macu[18], key[16];
    if (!blacklist_key(mac, macu, key)) return ESP_ERR_INVALID_ARG;
    if (!s_bl_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bl_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { xSemaphoreGive(s_bl_lock); return err; }
    char idx[BL_INDEX_MAX] = {0}, newidx[BL_INDEX_MAX] = {0};
    size_t isz = sizeof(idx);
    err = nvs_get_str(h, K_BL_INDEX, idx, &isz);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err != ESP_OK) goto done_remove;
    char *save = NULL;
    for (char *tok = strtok_r(idx, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, macu) == 0) continue;
        if (newidx[0]) strlcat(newidx, ",", sizeof(newidx));
        strlcat(newidx, tok, sizeof(newidx));
    }
    err = nvs_set_str(h, K_BL_INDEX, newidx);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
done_remove:
    bl_cache_invalidate();
    nvs_close(h);
    xSemaphoreGive(s_bl_lock);
    return err;
}

esp_err_t store_blacklist_list(char *out, size_t n) {
    if (!out || n < 16) return ESP_ERR_INVALID_ARG;
    if (!s_bl_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bl_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) { xSemaphoreGive(s_bl_lock); return err; }
    char idx[BL_INDEX_MAX] = {0};
    size_t sz = sizeof(idx);
    err = nvs_get_str(h, K_BL_INDEX, idx, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err != ESP_OK) goto done_list;
    size_t o = (size_t)snprintf(out, n, "{\"items\":[");
    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(idx, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char macu[18], key[16], lab[32] = {0}, lab_j[96];
        if (!blacklist_key(tok, macu, key)) continue;
        size_t lsz = sizeof(lab);
        esp_err_t read_err = nvs_get_str(h, key, lab, &lsz);
        if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND) { err = read_err; break; }
        json_escape(lab, lab_j, sizeof(lab_j));
        int w = snprintf(out + o, n - o, "%s{\"mac\":\"%s\",\"label\":\"%s\"}",
                         count ? "," : "", macu, lab_j);
        if (w < 0 || (size_t)w >= n - o - 2) { err = ESP_ERR_NO_MEM; break; }
        o += (size_t)w;
        count++;
    }
    if (err == ESP_OK) snprintf(out + o, n - o, "]}");
done_list:
    nvs_close(h);
    xSemaphoreGive(s_bl_lock);
    return err;
}
