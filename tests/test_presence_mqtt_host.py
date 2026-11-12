#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,json
s=(Path(__file__).resolve().parents[1]/'main/mqtt.c').read_text()
def fn(mark):
 a=s.rfind('\n',0,s.index(mark))+1;b=s.index('{',a);i=b+1;d=1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define OBS_SSID_MAX 33
#define OBS_TS_STR 20
#define CFG_DEVID_MAX 24
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef struct{char mac[18],ssid[33],ts[20];int rssi,is_ble;}s3_obs_t;
typedef struct{bool mqtt_enabled;char device_id[24];}app_config_t;
static app_config_t cfg={true,"unit\"01"};static void*s_client_lock=(void*)1,*s_client=(void*)1;static bool s_initialized=true;static int fail_lock,fail_queue;
static const app_config_t*config_get(void){return &cfg;}
static int xSemaphoreTake(void*p,int t){(void)p;(void)t;return !fail_lock;}
static void xSemaphoreGive(void*p){(void)p;}
static bool net_time_utc(char*p,size_t n){(void)p;(void)n;return false;}
static const char*net_time_quality_str(void){return "UNKNOWN";}
static int esp_mqtt_client_enqueue(void*c,const char*topic,const char*payload,int len,int qos,int retain,bool store){(void)c;(void)len;assert(!strcmp(topic,"/rfsentinel/presence") && qos==1 && !retain && store);if(fail_queue)return -1;puts(payload);return 1;}
'''+fn('static void json_escape(')+fn('bool mqtt_presence_publish(')+r'''
int main(void){s3_obs_t o={.mac="AA:BB:CC:DD:EE:FF",.ssid="x\"y",.is_ble=1,.rssi=-119};assert(mqtt_presence_publish(&o,"label\"é","/rfsentinel/presence"));o.rssi=127;assert(mqtt_presence_publish(&o,"", "/rfsentinel/presence"));cfg.mqtt_enabled=false;assert(!mqtt_presence_publish(&o,"", "/rfsentinel/presence"));cfg.mqtt_enabled=true;fail_lock=1;assert(!mqtt_presence_publish(&o,"", "/rfsentinel/presence"));fail_lock=0;fail_queue=1;assert(!mqtt_presence_publish(&o,"", "/rfsentinel/presence"));return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-pmq-',dir='/tmp') as tmp:
 p=Path(tmp);(p/'t.c').write_text(code);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 r=subprocess.run([str(p/'t')],check=True,capture_output=True,text=True);a,b=map(json.loads,r.stdout.splitlines());assert a['event']=='presence' and a['rssi']==-119 and a['label']=='label"é' and a['device_id']=='unit"01';assert b['rssi'] is None;print('presence MQTT: independent topic, valid JSON, unknown RSSI and queue errors passed')
