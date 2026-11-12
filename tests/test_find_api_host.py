#!/usr/bin/env python3
"""Validate production candidate JSON, bounds and allocation failures on host."""
from pathlib import Path
import subprocess,tempfile,json
s=(Path(__file__).resolve().parents[1]/'main/web.c').read_text();a=s.index('static esp_err_t h_find_get(');b=s.index('\nstatic esp_err_t h_presence_get(',a)
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define ESP_OK 0
#define SCANNER_FIND_MAX 48
typedef int esp_err_t;typedef struct {int status;}httpd_req_t;
typedef struct{char mac[18];int32_t sums[3];uint32_t counts[3];int rssi;int64_t last_sample_us;}scanner_find_entry_t;
typedef struct{scanner_find_entry_t entries[SCANNER_FIND_MAX];uint32_t count;uint8_t stage;bool overflow;}scanner_find_result_t;
static int fail_after=-1;
static void*test_malloc(size_t n){if(fail_after==0)return NULL;if(fail_after>0)fail_after--;return malloc(n);}
#define malloc test_malloc
static bool req_authorized(httpd_req_t*r){(void)r;return true;}
static void send_err(httpd_req_t*r,const char*x){(void)x;r->status=401;}
static void send_json(httpd_req_t*r,int status,const char*j){r->status=status;puts(j);}
static int64_t esp_timer_get_time(void){return 10000000;}
static void scanner_find_status(scanner_find_result_t*s){memset(s,0,sizeof(*s));s->count=48;s->stage=2;s->overflow=true;for(int i=0;i<48;i++){scanner_find_entry_t*e=&s->entries[i];snprintf(e->mac,sizeof(e->mac),"AA:BB:CC:DD:EE:%02X",i);e->counts[0]=2;e->counts[1]=1;e->counts[2]=3;e->sums[0]=-100;e->sums[1]=-90;e->sums[2]=-180;e->rssi=-60;e->last_sample_us=8000000;}}
'''+s[a:b]+r'''
int main(void){httpd_req_t r={0};assert(h_find_get(&r)==0 && r.status==200);fail_after=0;r.status=0;assert(h_find_get(&r)==0 && r.status==500);fail_after=1;r.status=0;assert(h_find_get(&r)==0 && r.status==500);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-find-',dir='/tmp') as t:
 p=Path(t);(p/'t.c').write_text(code);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 out=subprocess.run([str(p/'t')],capture_output=True,text=True,check=True).stdout.splitlines();data=json.loads(out[0]);assert data['stage']==2 and data['overflow'] and len(data['candidates'])==48
 assert data['candidates'][0]['near1']==-50 and data['candidates'][0]['far']==-90 and data['candidates'][0]['near2']==-60 and data['candidates'][0]['age']==2
 assert all(json.loads(x)['error']=='oom' for x in out[1:]);print('candidate API: 48 entries, phase means and allocation failures passed')
