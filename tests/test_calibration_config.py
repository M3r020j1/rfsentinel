#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'main/config.c').read_text();a=s.index('esp_err_t config_set_rssi_threshold(');b=s.index('\nesp_err_t config_set_mqtt(',a)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;typedef int nvs_handle_t;
#define ESP_OK 0
#define NVS_NS "rfsentinel"
#define K_MQTT_RSSI "mq_rssi"
#define K_MIN_RSSI "min_rssi"
#define NVS_READWRITE 1
static struct {int8_t min_rssi,mqtt_min_rssi;} s_cfg={-95,-60};
static int failure;static char written_key[32];static int written_value;
static int nvs_open(const char*n,int f,nvs_handle_t*h){(void)n;(void)f;*h=1;return failure==1?1:0;}
static int nvs_set_i8(nvs_handle_t h,const char*k,int8_t v){(void)h;strcpy(written_key,k);written_value=v;return failure==2?2:0;}
static int nvs_commit(nvs_handle_t h){(void)h;return failure==3?3:0;}
static void nvs_close(nvs_handle_t h){(void)h;}
'''+s[a:b]+r'''
int main(void){assert(config_set_rssi_threshold(false,-88)==0 && s_cfg.min_rssi==-88 && s_cfg.mqtt_min_rssi==-60 && !strcmp(written_key,K_MIN_RSSI) && written_value==-88);
 assert(config_set_rssi_threshold(true,-70)==0 && s_cfg.mqtt_min_rssi==-70 && s_cfg.min_rssi==-88 && !strcmp(written_key,K_MQTT_RSSI));
 for(failure=1;failure<=3;failure++){assert(config_set_rssi_threshold(false,-99)!=0);assert(s_cfg.min_rssi==-88);}
 puts("calibration configuration: independent persistence and NVS errors passed");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-cal-',dir='/tmp') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
