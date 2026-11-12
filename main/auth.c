#include "auth.h"
#include "config.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "auth";

#define SESSION_TTL_US (60LL * 60 * 1000 * 1000)

static char s_token[33] = {0};
static int64_t s_expire_us = 0;
static int s_fail = 0;
static int64_t s_lock_until_us = 0;

void auth_init(void) {
    s_token[0] = 0;
    s_expire_us = 0;
    s_fail = 0;
    s_lock_until_us = 0;
}

static void gen_token(void) {
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        s_token[i * 2] = hex[rnd[i] >> 4];
        s_token[i * 2 + 1] = hex[rnd[i] & 0x0F];
    }
    s_token[32] = 0;
    s_expire_us = esp_timer_get_time() + SESSION_TTL_US;
}

bool auth_login(const char *user, const char *pass) {
    int64_t now = esp_timer_get_time();
    if (now < s_lock_until_us) return false;

    bool ok = config_check_credentials(user, pass);
    if (ok) {
        s_fail = 0;
        gen_token();
        return true;
    }

    s_fail++;
    if (s_fail >= 3) {
        s_lock_until_us = esp_timer_get_time() + 30LL * 1000 * 1000;
        ESP_LOGW(TAG, "login locked");
    }
    return false;
}

/* Echec d'authentification hors /login (repli HTTP Basic): meme compteur que
   login() pour que le Basic ne permette pas de contourner le verrouillage. */
void auth_note_failure(void) {
    s_fail++;
    if (s_fail >= 3) {
        s_lock_until_us = esp_timer_get_time() + 30LL * 1000 * 1000;
        ESP_LOGW(TAG, "login locked (basic)");
    }
}

bool auth_locked(void) {
    return esp_timer_get_time() < s_lock_until_us;
}

void auth_logout(void) {
    s_token[0] = 0;
    s_expire_us = 0;
}

bool auth_check_request(httpd_req_t *req) {
    if (s_token[0] == 0) return false;
    if (esp_timer_get_time() > s_expire_us) {
        s_token[0] = 0;
        return false;
    }

    char hdr[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof(hdr)) != ESP_OK) return false;
    const char *p = strstr(hdr, "sid=");
    if (p == NULL) return false;
    /* "sid=" doit bien debuter le cookie: sinon un autre cookie peut leurrer */
    if (p != hdr && p[-1] != ' ' && p[-1] != ';' && p[-1] != ',') return false;
    p += 4;
    for (int i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)p[i])) return false;
    }
    if (strncmp(p, s_token, 32) != 0) return false;
    const char *q = p + 32;
    while (*q == ' ') q++;
    if (*q != '\0' && *q != ';') return false;
    return true;
}

void auth_set_cookie(httpd_req_t *req, char *buffer, size_t size) {
    snprintf(buffer, size, "sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600", s_token);
    httpd_resp_set_hdr(req, "Set-Cookie", buffer);
}
