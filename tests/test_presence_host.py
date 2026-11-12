#!/usr/bin/env python3
"""Exercise production presence persistence and absence/retry logic."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];src=(root/'main/presence.c').read_text();src='\n'.join(line for line in src.splitlines() if not line.startswith('#include'))
head=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#define PRESENCE_MAX 32
#define PRESENCE_TOPIC_MAX 64
#define PRESENCE_LABEL_MAX 32
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_NVS_NOT_FOUND 5
#define NVS_READWRITE 0
#define portMAX_DELAY 0
#define pdTRUE 1
typedef int esp_err_t;typedef int nvs_handle_t;typedef void *SemaphoreHandle_t;
typedef struct{char mac[18],label[32];}presence_item_t;
typedef struct{char topic[64];uint16_t absence_s,count;presence_item_t items[32];}presence_config_t;
typedef struct{char mac[18];int rssi,is_ble;char ssid[33],ts[20];}s3_obs_t;
static presence_config_t persisted;static bool has_blob,fail_write,fail_publish;static int sends;static int64_t now_us;
static void *xSemaphoreCreateMutex(void){return (void*)1;}
static void vSemaphoreDelete(void *p){(void)p;}
static int xSemaphoreTake(void *p,int t){(void)p;(void)t;return 1;}
static void xSemaphoreGive(void *p){(void)p;}
static int64_t esp_timer_get_time(void){return now_us;}
static int nvs_open(const char*n,int f,int*h){(void)n;(void)f;*h=1;return 0;}
static int nvs_get_blob(int h,const char*k,void*p,size_t*n){(void)h;(void)k;if(!has_blob)return ESP_ERR_NVS_NOT_FOUND;assert(*n==sizeof(persisted));memcpy(p,&persisted,sizeof(persisted));return 0;}
static int nvs_set_blob(int h,const char*k,const void*p,size_t n){(void)h;(void)k;assert(n==sizeof(persisted));if(fail_write)return 9;memcpy(&persisted,p,n);has_blob=true;return 0;}
static int nvs_commit(int h){(void)h;return 0;}
static void nvs_close(int h){(void)h;}
static bool mqtt_presence_publish(const s3_obs_t*o,const char*l,const char*t){(void)o;(void)l;assert(!strcmp(t,"/rfsentinel/presence"));if(fail_publish)return false;sends++;return true;}
'''
main=r'''
int main(void){presence_config_t cfg;assert(presence_init()==ESP_OK);assert(presence_get(&cfg)==ESP_OK && cfg.absence_s==120 && cfg.count==0);
 assert(presence_add("aa:bb:cc:dd:ee:ff","phone")==ESP_OK);assert(presence_matches("AA:BB:CC:DD:EE:FF"));
 assert(presence_add("AA:BB:CC:DD:EE:FF","new")==ESP_OK);assert(presence_get(&cfg)==ESP_OK && cfg.count==1 && !strcmp(cfg.items[0].label,"new"));
 assert(presence_add("invalid","")==ESP_ERR_INVALID_ARG);assert(presence_settings("bad/#",120)==ESP_ERR_INVALID_ARG);assert(presence_settings("/rfsentinel/presence",29)==ESP_ERR_INVALID_ARG);
 s3_obs_t o={.mac="AA:BB:CC:DD:EE:FF",.rssi=-120,.is_ble=1};now_us=1000000;presence_on_obs(&o);assert(sends==1);
 now_us=110000000;presence_on_obs(&o);assert(sends==1);now_us=229000000;presence_on_obs(&o);assert(sends==1);
 now_us=350000000;presence_on_obs(&o);assert(sends==2);
 now_us=500000000;fail_publish=true;presence_on_obs(&o);assert(sends==2);now_us=503000000;fail_publish=false;presence_on_obs(&o);assert(sends==2);now_us=506000000;presence_on_obs(&o);assert(sends==3);
 fail_write=true;assert(presence_remove(o.mac)!=ESP_OK && presence_matches(o.mac));fail_write=false;
 assert(presence_remove(o.mac)==ESP_OK && !presence_matches(o.mac));presence_on_obs(&o);assert(sends==3);
 assert(presence_init()==ESP_OK && presence_get(&cfg)==ESP_OK && cfg.count==0);
 assert(presence_settings("/custom/presence",600)==ESP_OK);
 memset(&s_config,0,sizeof(s_config)); s_lock=NULL;
 assert(presence_init()==ESP_OK && presence_get(&cfg)==ESP_OK);
 assert(!strcmp(cfg.topic,"/custom/presence") && cfg.absence_s==600);
 puts("presence: all RSSI, absence transition, retries, deduplication and NVS failure passed");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-pres-',dir='/tmp') as tmp:
 p=Path(tmp);(p/'test.c').write_text(head+src+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
