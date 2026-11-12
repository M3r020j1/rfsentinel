"""Exercise detailed lookup versus the fallback summary using the real dispatcher."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'main/wigle.c').read_text(); code=s[s.index('esp_err_t wigle_search_device('):]
pre=r'''
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NO_MEM 4
#define WIGLE_NET_DETAIL_URL "network-detail"
#define WIGLE_BT_DETAIL_URL "bt-detail"
#define WIGLE_NET_URL "network-search"
#define WIGLE_BT_URL "bt-search"
#define ESP_LOGI(...) ((void)0)
#define MALLOC_CAP_INTERNAL 0
static int detail_result,detail_count,summary_count;
static bool mac_is_safe(const char *s){(void)s;return true;}
static void search_error(char *o,size_t n,const char *c){snprintf(o,n,"{\"error\":\"%s\"}",c);}
static void wigle_heap_log(const char *s){(void)s;}
static esp_err_t wigle_detail_one(const char*a,const char*b,const char*c,bool d,bool e,char*out,size_t n,int*rows){(void)a;(void)b;(void)c;(void)d;(void)e;*rows=detail_count;snprintf(out,n,"{\"results\":[%s]}",*rows?"1":"");return detail_result;}
static esp_err_t wigle_search_one(const char*a,const char*b,const char*c,bool d,char*out,size_t n,int*rows){(void)a;(void)b;(void)c;(void)d;*rows=summary_count;snprintf(out,n,"{\"results\":[%s]}",*rows?"1":"");return ESP_OK;}
'''
main=r'''
int main(void){char o[512];detail_result=ESP_FAIL;summary_count=1;
 assert(wigle_search_device("aa:bb", "WIFI", o,sizeof(o))==ESP_OK && strstr(o,"\"summary_only\":true") && strstr(o,"\"detail_failed\":true"));
 detail_result=ESP_ERR_NOT_FOUND;assert(wigle_search_device("aa:bb", "BLE", o,sizeof(o))==ESP_OK && strstr(o,"\"detail_failed\":false"));
 detail_count=1;detail_result=ESP_OK;assert(wigle_search_device("aa:bb", "WIFI", o,sizeof(o))==ESP_OK && !strstr(o,"summary_only"));
 puts("WiGLE: detailed result and explicit summary fallback passed");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'t.c';p.write_text(pre+code+main);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(Path(d)/'t')],check=True);subprocess.run([str(Path(d)/'t')],check=True)
