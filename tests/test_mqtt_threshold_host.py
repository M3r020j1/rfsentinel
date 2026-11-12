#!/usr/bin/env python3
"""Exercise production MQTT threshold, cooldown and bounded cache behavior."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'main/mqtt.c').read_text()
def fn(marker):
 a=s.rfind('\n',0,s.index(marker))+1;b=s.index('{',a);i=b+1;d=1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#define OBS_MAC_STR 18
#define OBS_SSID_MAX 33
#define OBS_TS_STR 20
#define MQTT_SEEN_MAX 16
#define portMAX_DELAY 0
#define ESP_LOGW(...) ((void)0)
typedef struct{char mac[18],ssid[33],ts[20];int rssi,is_ble;}s3_obs_t;
typedef struct{bool mqtt_enabled;int mqtt_min_rssi,rediscover_min;char device_id[24],mqtt_topic[64];float lat,lon;}app_config_t;
typedef struct{char mac[OBS_MAC_STR];time_t last_seen;}mq_seen_t;
static void*s_client_lock=(void*)1,*s_client=(void*)1;static bool s_initialized=true;static int lock,published,fail_publish;
static mq_seen_t s_seen[MQTT_SEEN_MAX];static int s_seen_count;
static app_config_t cfg={.mqtt_enabled=true,.mqtt_min_rssi=-60,.rediscover_min=60,.device_id="test",.mqtt_topic="test"};
static const app_config_t*config_get(void){return &cfg;}
static void xSemaphoreTake(void*p,int t){(void)p;(void)t;assert(lock==0);lock++;}
static void xSemaphoreGive(void*p){(void)p;assert(lock==1);lock--;}
static int esp_mqtt_client_publish(void*c,const char*t,const char*p,int n,int q,int r){(void)c;(void)t;(void)p;(void)n;(void)q;(void)r;assert(lock==1);if(fail_publish)return -1;return ++published;}
static bool net_time_utc(char*p,size_t n){(void)p;(void)n;return false;}
static const char*net_time_quality_str(void){return "UNKNOWN";}
'''
code+=fn('static void json_escape(')+fn('void mqtt_on_obs(')
code+=r'''
int main(void){s3_obs_t o={.mac="AA:BB:CC:DD:EE:01",.rssi=-80};mqtt_on_obs(&o);assert(published==0 && !lock);
 o.rssi=-60;mqtt_on_obs(&o);assert(published==1 && !lock);mqtt_on_obs(&o);assert(published==1 && !lock);
 for(int i=2;i<=18;i++){snprintf(o.mac,sizeof(o.mac),"AA:BB:CC:DD:EE:%02X",i);mqtt_on_obs(&o);}
 assert(published==18 && s_seen_count==16 && !lock);mqtt_on_obs(&o);assert(published==18);
 snprintf(o.mac,sizeof(o.mac),"AA:BB:CC:DD:EE:FE");fail_publish=1;mqtt_on_obs(&o);assert(!lock && published==18);
 fail_publish=0;mqtt_on_obs(&o);assert(published==19 && !lock);
 puts("MQTT: threshold, cooldown, full cache, publish failure and lock passed");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-mqtt-',dir='/tmp') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
