"""Exercise the real WiGLE result code with simulated HTTPS and allocation failures."""
from pathlib import Path
import subprocess, tempfile
src = (Path(__file__).resolve().parents[1] / 'main/wigle.c').read_text()
def part(start, end):
    return src[src.index(start):src.index(end, src.index(start))]
normalize = part('static bool wigle_normalize_mac(', '/* Construction du tableau')
rows = part('typedef struct {\n    char mac[24];', '/* --- recherche simple')
search = part('static esp_err_t wigle_search_one(', '/* --- historique detaille')
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#define WIGLE_MAX_ROWS 10
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_ARG -3
typedef int esp_err_t;
static int tls_open, fail_alloc, fail_fetch, free_count;
static void *checked_calloc(size_t a,size_t b) { assert(!tls_open); if(fail_alloc) return NULL; return calloc(a,b); }
#define calloc checked_calloc
static size_t test_strlcpy(char *d,const char *s,size_t n) { size_t z=strlen(s); if(n){size_t k=z<n-1?z:n-1;memcpy(d,s,k);d[k]=0;} return z; }
#undef strlcpy
#define strlcpy test_strlcpy
static void json_escape(const char *s,char *d,size_t n) { strlcpy(d,s,n); }
typedef struct cJSON { struct cJSON *next; int kind; } cJSON;
static cJSON root, array, item;
static cJSON *cJSON_Parse(const char *s) { (void)s; return &root; }
static void cJSON_Delete(cJSON *p) { (void)p; free_count++; }
static cJSON *cJSON_GetObjectItem(cJSON *p,const char *s) { (void)p;(void)s;return &array; }
static int cJSON_IsArray(cJSON *p) { return p==&array; }
#define cJSON_ArrayForEach(i,a) for ((i)=&item; (i); (i)=(i)->next)
static void first_str(const cJSON *i,char *d,size_t n,const char *a,const char *b,const char *c) { (void)i;(void)b;(void)c;strlcpy(d,!strcmp(a,"netid")?"AA:BB:CC:DD:EE:FF":"",n); }
static double first_num(const cJSON *i,const char*a,const char*b,const char*c) {(void)i;(void)a;(void)b;(void)c;return 0;}
static void wigle_heap_log(const char *s) { (void)s; }
static void search_error(char *out,size_t n,const char *code) { snprintf(out,n,"error:%s",code); }
static void enc_mac(const char *v,bool l,char *d,size_t n) {(void)l;strlcpy(d,v,n);}
static esp_err_t wigle_fetch(const char *u,char **body,int *status,bool *truncated,char *out,size_t n) {
 (void)u; tls_open=1; if(fail_fetch){tls_open=0;search_error(out,n,"network");return ESP_FAIL;}
 *body=out;memcpy(*body,"{}",3);*status=200;*truncated=false;tls_open=0;return ESP_OK; }
'''
main = r'''
int main(void) {
 char out[1024], m[18]; int n=99;
 assert(wigle_normalize_mac("9392848485987","08:8A:F1:65:BE:63",m) && !strcmp(m,"08:8A:F1:65:BE:63"));
 assert(wigle_normalize_mac("08:8a:f1:65:be:63",NULL,m) && !strcmp(m,"08:8A:F1:65:BE:63"));
 assert(wigle_normalize_mac("088AF165BE63",NULL,m) && !strcmp(m,"08:8A:F1:65:BE:63"));
 assert(wigle_normalize_mac("1",NULL,m) && !strcmp(m,"00:00:00:00:00:01"));
 assert(wigle_normalize_mac("123456789012","00:1C:BE:99:1A:14",m) && !strcmp(m,"00:1C:BE:99:1A:14"));
 assert(!wigle_normalize_mac("281474976710656",NULL,m)); assert(!wigle_normalize_mac("not-a-mac",NULL,m));
 assert(wigle_search_one("https://example",NULL,"AA",false,out,sizeof(out),&n)==ESP_OK);
 assert(n==1 && strstr(out,"AA:BB:CC:DD:EE:FF"));
 fail_alloc=1; assert(wigle_search_one("https://example",NULL,"AA",false,out,sizeof(out),&n)==ESP_ERR_NO_MEM);
 assert(!strcmp(out,"error:nomem") && n==0 && free_count==2);
 fail_alloc=0;fail_fetch=1;assert(wigle_search_one("https://example",NULL,"AA",false,out,sizeof(out),&n)==ESP_FAIL);
 assert(!strcmp(out,"error:network") && n==0 && free_count==2);
 fail_fetch=0; rows_t r; assert(rows_open(&r));
 for(int i=0;i<12;i++){char t[32];snprintf(t,sizeof(t),"2026-01-%02d",i+1);rows_add(&r,"aa","","",t,"",1,1,0,0);}
 assert(r.n==10 && r.truncated); for(int i=0;i<r.n;i++)assert(strcmp(r.v[i].ts,"2026-01-03")>=0);free(r.v);
 puts("WiGLE: allocation, failures, and ten most recent received results passed");
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d)/'test.c'; c.write_text(pre+normalize+rows+search+main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(Path(d)/'test')],check=True)
    subprocess.run([str(Path(d)/'test')],check=True)
