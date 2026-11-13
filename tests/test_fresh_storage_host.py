#!/usr/bin/env python3
"""Simulate mount failures and prove formatting is permitted only for blank flash."""
from pathlib import Path
import subprocess, tempfile

s=(Path(__file__).resolve().parents[1]/'main/store.c').read_text()
start=s.index('static esp_err_t store_partition_is_blank(')
end=s.index('\nbool store_ready(',start)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
typedef int esp_err_t;
typedef struct {size_t size; bool encrypted;} esp_partition_t;
typedef struct {const char *base_path,*partition_label;bool format_if_mount_failed,grow_on_mount;} esp_vfs_littlefs_conf_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NO_MEM 3
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
#define RFS_MOUNT "/tmp"
#define RFS_OBS_DIR "/tmp"
#define pdPASS 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static int s_store_lock,s_bl_lock,s_bl_loaded,s_mounted;
static uint8_t flash_data[65539]; static esp_partition_t part={sizeof(flash_data),false};
static int calls,formats, fail_read,initial_mount_success, fail_retry, missing;
static int xSemaphoreCreateMutex(void){return 1;}
static void vTaskDelay(int n){(void)n;}
static void store_counts_task(void *arg){(void)arg;}
static int xTaskCreate(void(*f)(void*),const char*n,int s,void*a,int p,void*h){(void)f;(void)n;(void)s;(void)a;(void)p;(void)h;return 1;}
static const esp_partition_t *esp_partition_find_first(int a,int b,const char*c){(void)a;(void)b;(void)c;return missing?NULL:&part;}
static int esp_partition_read_raw(const esp_partition_t*p,size_t o,void*b,size_t n){(void)p;if(fail_read && o>=1024)return ESP_FAIL;memcpy(b,flash_data+o,n);return ESP_OK;}
static int esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *c){
 calls++; if(c->format_if_mount_failed){formats++;return fail_retry?ESP_FAIL:ESP_OK;} return initial_mount_success?ESP_OK:ESP_FAIL;
}
'''+s[start:end]+r'''
static void reset(void){memset(flash_data,0xff,sizeof(flash_data));calls=formats=fail_read=initial_mount_success=fail_retry=missing=0;part.encrypted=false;s_mounted=false;}
int main(void){
 reset();assert(store_init()==ESP_OK && calls==2 && formats==1 && s_mounted);
 reset();initial_mount_success=1;flash_data[0]=0;assert(store_init()==ESP_OK && calls==1 && formats==0);
 reset();flash_data[sizeof(flash_data)-1]=0;assert(store_init()!=ESP_OK && calls==1 && formats==0);
 reset();fail_read=1;assert(store_init()!=ESP_OK && calls==1 && formats==0);
 reset();part.encrypted=true;assert(store_init()!=ESP_OK && calls==1 && formats==0);
 reset();missing=1;assert(store_init()!=ESP_OK && calls==1 && formats==0);
 reset();fail_retry=1;assert(store_init()!=ESP_OK && calls==2 && formats==1 && !s_mounted);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('LittleFS initialization: blank, existing, corrupt, read errors and encrypted partitions passed')
