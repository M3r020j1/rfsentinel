#!/usr/bin/env python3
"""Host exercise of production BLE calibration callbacks and lifecycle."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'main/scanner.c').read_text()
def fn(marker):
 a=s.rfind('\n',0,s.index(marker))+1;b=s.index('{',a);i=b+1;d=1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_TIMEOUT 5
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define BLE_GAP_EVENT_DISC 1
#define BLE_GAP_EVENT_DISC_COMPLETE 2
#define BLE_OWN_ADDR_PUBLIC 0
#define OBS_SSID_MAX 33
#define OBS_AUTH_MAX 16
#define TS_UNKNOWN 0
#define CALIBRATION_TIMEOUT_MS (15 * 60 * 1000)
#define SCANNER_FIND_MAX 48
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;typedef int ts_quality_t;
typedef struct {bool active,stop_requested,has_sample,radio_error,resume_error,resume_scan;int rssi,minimum;uint32_t samples;int64_t last_sample_us;char mac[18],kind[8];} scanner_calibration_t;
typedef struct {char mac[18];int32_t sums[3];uint32_t counts[3];int rssi;int64_t last_sample_us;} scanner_find_entry_t;
typedef struct {scanner_find_entry_t entries[SCANNER_FIND_MAX];uint32_t count;uint8_t stage;bool overflow;} scanner_find_result_t;
typedef struct {char mac[18],ssid[33],auth[16],ts[20];int rssi,channel,freq,mfgr,is_ble;uint8_t time_quality;} s3_obs_t;
typedef struct {int min_rssi,mqtt_min_rssi,ble_address_filter;} app_config_t;
struct ble_gap_event {int type;struct {int rssi;struct{uint8_t val[6];uint8_t type;}addr;uint8_t data[31];int length_data;}disc;};
struct ble_gap_disc_params {int filter_duplicates,passive,itvl,window;};
static scanner_calibration_t s_cal;static scanner_find_result_t s_find;static bool s_cal_radio_done,s_ble_ready=true,s_ble_failed,s_running;static int s_cal_lock;static uint32_t s_ble_count;static uint64_t s_last_wifi_us,s_last_ble_us;static void*s_task,*s_exit_sem=(void*)1;
static int mqtt_calls,store_calls,presence_calls,deny_alloc,fail_radio,no_sample;static bool watched,excluded;static void(*pending)(void*);static app_config_t cfg={-60,-90,1};
static int s_ble_diag_lock;
static struct {uint32_t public_count,static_count,rpa_count,nrpa_count,unknown_count;} s_ble_diag;
static const app_config_t*config_get(void){return &cfg;}
static int64_t esp_timer_get_time(void){static int64_t t;return ++t;}
static bool store_is_blacklisted(const char*m){(void)m;return excluded;}
static bool presence_matches(const char*m){(void)m;return watched;}
static void presence_on_obs(const s3_obs_t*o){(void)o;presence_calls++;}
static void parse_adv_name(const uint8_t*a,int b,char*c,int n){(void)a;(void)b;(void)c;(void)n;}
static bool net_time_utc_with_quality(char*a,int b,ts_quality_t*c){(void)a;(void)b;(void)c;return false;}
static void mqtt_on_obs(const s3_obs_t*o){(void)o;mqtt_calls++;}
static int store_append_obs(const s3_obs_t*o,bool*a){(void)o;store_calls++;*a=false;return ESP_OK;}
static void scan_loop(void*a){(void)a;}
static int xTaskCreate(void(*f)(void*),const char*n,int st,void*a,int pr,void**handle){(void)st;(void)a;(void)pr;if(deny_alloc)return 0;if(!strcmp(n,"rfs_cal"))pending=f;else if(handle)*handle=(void*)1;return pdPASS;}
static int xSemaphoreTake(void*x,int t){(void)x;(void)t;return pdTRUE;}
static int ble_gap_disc_cancel(void){return 0;}
static int ble_gap_disc_active(void){return 0;}
static void vTaskDelay(int n){(void)n;}
static void vTaskDelete(void*p){(void)p;}
static bool scanner_ble_available(void){return s_ble_ready && !s_ble_failed;}
static int ble_gap_disc(int a,int b,struct ble_gap_disc_params*c,int(*cb)(struct ble_gap_event*,void*),void*arg){
 (void)a;(void)b;assert(c->filter_duplicates==0 && c->passive==1);
 if(fail_radio)return 99;
 if(no_sample){struct ble_gap_event done={.type=BLE_GAP_EVENT_DISC_COMPLETE};cb(&done,arg);return 0;}
 struct ble_gap_event e={.type=BLE_GAP_EVENT_DISC};uint8_t addr[]={0xFF,0xEE,0xDD,0xCC,0xBB,0xAA};memcpy(e.disc.addr.val,addr,6);
 e.disc.rssi=-65;cb(&e,arg);e.disc.rssi=-92;cb(&e,arg);e.disc.rssi=-78;cb(&e,arg);
 e.disc.addr.val[0]=0;e.disc.rssi=-120;cb(&e,arg);
 e.type=BLE_GAP_EVENT_DISC_COMPLETE;cb(&e,arg);return 0;}
'''
code+=(Path(__file__).resolve().parents[1]/'main/ble_address.h').read_text().replace('#pragma once','')
for f in ['static void hex2str(', 'static int gap_event_cb(', 'esp_err_t scanner_start(', 'esp_err_t scanner_stop(', 'bool scanner_is_running(', 'static void calibration_task(', 'esp_err_t scanner_calibration_start(', 'esp_err_t scanner_calibration_stop(', 'void scanner_calibration_status(', 'esp_err_t scanner_find_start(', 'esp_err_t scanner_find_next(', 'void scanner_find_status(']:code+=fn(f)
code+=r'''
int main(void){scanner_calibration_t c;
 assert(scanner_calibration_start("bad","zone")==ESP_ERR_INVALID_ARG);
 s_running=true;s_task=(void*)1;assert(scanner_calibration_start("aa:bb:cc:dd:ee:ff","zone")==ESP_OK);
 assert(scanner_calibration_start("AA:BB:CC:DD:EE:FF","alert")==ESP_ERR_INVALID_STATE);
 assert(!s_running && s_cal.active && pending);pending(NULL);scanner_calibration_status(&c);
 assert(!c.active && c.has_sample && c.minimum==-92 && c.rssi==-78 && c.samples==3 && s_running);
 assert(!mqtt_calls && !store_calls);
 assert(scanner_stop()==ESP_OK);assert(scanner_calibration_start("AA:BB:CC:DD:EE:FF","alert")==ESP_OK);pending(NULL);
 assert(!s_running && s_cal.minimum==-92 && !strcmp(s_cal.kind,"alert"));
 struct ble_gap_event e={.type=BLE_GAP_EVENT_DISC};e.disc.rssi=-80;
 gap_event_cb(&e,NULL);assert(mqtt_calls==1 && store_calls==0);
 watched=true;excluded=true;e.disc.rssi=-120;gap_event_cb(&e,NULL);assert(presence_calls==1 && mqtt_calls==1 && store_calls==0);watched=false;excluded=false;
 e.disc.addr.type=1;e.disc.addr.val[5]=0x40;e.disc.rssi=-40;
 gap_event_cb(&e,NULL);assert(s_ble_diag.rpa_count==1 && mqtt_calls==1 && store_calls==0);
 watched=true;gap_event_cb(&e,NULL);assert(presence_calls==2 && mqtt_calls==1 && store_calls==0);watched=false;
 cfg.ble_address_filter=0;gap_event_cb(&e,NULL);assert(mqtt_calls==2 && store_calls==1);
 cfg.ble_address_filter=1;e.disc.addr.type=0;
 fail_radio=1;assert(scanner_calibration_start("AA:BB:CC:DD:EE:FF","zone")==ESP_OK);pending(NULL);assert(!s_cal.active && s_cal.radio_error && !s_cal.has_sample);fail_radio=0;
 no_sample=1;assert(scanner_calibration_start("AA:BB:CC:DD:EE:FF","zone")==ESP_OK);pending(NULL);assert(!s_cal.active && !s_cal.radio_error && !s_cal.has_sample);no_sample=0;
 assert(scanner_calibration_start("AA:BB:CC:DD:EE:FF","zone")==ESP_OK);assert(scanner_calibration_stop()==ESP_OK);pending(NULL);assert(!s_cal.active && !s_cal.has_sample);
 assert(scanner_find_start()==ESP_OK);
 struct ble_gap_event phase={.type=BLE_GAP_EVENT_DISC,.disc={.rssi=-50}};uint8_t addr[]={0xFF,0xEE,0xDD,0xCC,0xBB,0xAA};memcpy(phase.disc.addr.val,addr,6);gap_event_cb(&phase,&s_cal);
 assert(scanner_find_next()==ESP_OK);phase.disc.rssi=-90;gap_event_cb(&phase,&s_cal);assert(scanner_find_next()==ESP_OK);
 assert(scanner_find_next()==ESP_ERR_INVALID_STATE);pending(NULL);
 scanner_find_result_t result;scanner_find_status(&result);assert(!s_cal.active && result.stage==2 && result.count==2 && result.entries[0].counts[0]==1 && result.entries[0].sums[0]==-50 && result.entries[0].sums[1]==-90 && result.entries[0].counts[2]==3 && result.entries[0].sums[2]==-235);
 assert(scanner_find_start()==ESP_OK);for(int i=0;i<50;i++){phase.disc.addr.val[0]=(uint8_t)i;gap_event_cb(&phase,&s_cal);}scanner_find_status(&result);assert(result.count==SCANNER_FIND_MAX && result.overflow);assert(scanner_calibration_stop()==ESP_OK);pending(NULL);
 deny_alloc=1;assert(scanner_calibration_start("AA:BB:CC:DD:EE:FF","zone")==ESP_ERR_NO_MEM);assert(!s_cal.active);
 puts("calibration: MAC filtering, minimum, scan restoration, no observations, MQTT independence and OOM passed");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-cal-',dir='/tmp') as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
