#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

void auth_init(void);
bool auth_login(const char *user, const char *pass);
void auth_logout(void);
bool auth_check_request(httpd_req_t *req);
void auth_set_cookie(httpd_req_t *req, char *buffer, size_t size);

/* Echec d'authentification sur un canal sans session (Basic): alimente le
   meme compteur d'echecs que login() pour qu'on ne puisse pas brute-forcer
   le Basic en evitant la route /login. */
void auth_note_failure(void);
bool auth_locked(void);
