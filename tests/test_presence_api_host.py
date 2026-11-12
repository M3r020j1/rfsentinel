#!/usr/bin/env python3
from pathlib import Path
import json,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'main/web.c').read_text()
def fn(mark):
 a=s.rfind('\n',0,s.index(mark))+1;b=s.index('{',a);i=b+1;d=1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
code=r'''
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define ESP_OK 0
#define PRESENCE_MAX 32
#define PRESENCE_TOPIC_MAX 64
#define PRESENCE_LABEL_MAX 32
typedef int esp_err_t;typedef struct{int status;}httpd_req_t;
typedef struct{char mac[18],label[32];}presence_item_t;
typedef struct{char topic[64];uint16_t absence_s,count;presence_item_t items[32];}presence_config_t;
static int fail_after=-1;static void*test_malloc(size_t n){if(fail_after==0)return NULL;if(fail_after>0)fail_after--;return malloc(n);}
#define malloc test_malloc
static bool req_authorized(httpd_req_t*r){(void)r;return true;}
static void send_err(httpd_req_t*r,const char*s){(void)s;r->status=401;}
static void send_json(httpd_req_t*r,int status,const char*s){r->status=status;puts(s);}
static int presence_get(presence_config_t*p){memset(p,0,sizeof(*p));strcpy(p->topic,"/rfsentinel/presence");p->absence_s=120;p->count=32;for(int i=0;i<32;i++){snprintf(p->items[i].mac,18,"AA:BB:CC:DD:EE:%02X",i);memset(p->items[i].label,'"',31);}return 0;}
'''+fn('static void json_escape(')+fn('static esp_err_t h_presence_get(')+r'''
int main(void){httpd_req_t r={0};assert(h_presence_get(&r)==0 && r.status==200);fail_after=0;assert(h_presence_get(&r)==0 && r.status==500);fail_after=1;assert(h_presence_get(&r)==0 && r.status==500);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-papi-',dir='/tmp') as tmp:
 p=Path(tmp);(p/'t.c').write_text(code);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 lines=subprocess.run([str(p/'t')],capture_output=True,text=True,check=True).stdout.splitlines();j=json.loads(lines[0]);assert len(j['items'])==32 and j['items'][0]['label']=='"'*31 and j['absence_s']==120
 assert all(json.loads(x)['error']=='oom' for x in lines[1:]);print('presence API: 32 labels, escaping and allocation failures passed')
