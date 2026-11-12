#!/usr/bin/env python3
"""Deterministic host interleavings for the production recount function."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'main/store.c').read_text()
def fn(marker):
 a=s.rfind('\n',0,s.index(marker))+1;b=s.index('{',a);i=b+1;depth=1
 while depth: depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#define RFS_OBS_DIR test_dir
#define RFS_OBS_PREFIX "obs_"
#define RFS_LINE_MAX 256
#define portMAX_DELAY 0
#define ESP_OK 0
#define ESP_FAIL 1
typedef int esp_err_t;
static const char *test_dir;
static int s_store_lock=1,depth,mode,fired;
static bool s_counts_valid,s_recount_active;
static uint32_t s_counts_wifi,s_counts_ble,s_recount_wifi,s_recount_ble,s_store_rewrite_generation,s_segment_seq=2;
static char s_segment_name[32]="obs_s0000000002_N.csv";
static void xSemaphoreTake(int x,int y){(void)x;(void)y;depth++;}
static void xSemaphoreGive(int x){(void)x;assert(depth>0);depth--;}
static void vTaskDelay(int n){
 (void)n;if(depth||fired++)return;
 if(mode==2){s_store_rewrite_generation++;return;}
 char path[200];snprintf(path,sizeof(path),"%s/%s",test_dir,s_segment_name);
 FILE*f=fopen(path,"a");assert(f);fputs("b,b,b,b,b,b,b,b,b,b,b,b,b,BLE\n",f);fclose(f);s_recount_ble++;
 snprintf(path,sizeof(path),"%s/obs_s0000000003_N.csv",test_dir);
 f=fopen(path,"w");assert(f);fputs("h\nh\na,a,a,a,a,a,a,a,a,a,a,a,a,WIFI\n",f);fclose(f);s_segment_seq=3;s_recount_wifi++;
}
static int segment_initialize(void){return ESP_OK;}
'''
for name in ['static bool segment_number(','static int csv_fields(','static int csv_read_record(','static bool obs_file(','static void store_recount_once(']:code+=fn(name)
code+=r'''
static void writefile(int n){char p[200];snprintf(p,sizeof(p),"%s/obs_s%010d_N.csv",test_dir,n);FILE*f=fopen(p,"w");assert(f);fputs("h\nh\na,a,a,a,a,a,a,a,a,a,a,a,a,WIFI\n",f);fclose(f);}
int main(int argc,char**argv){assert(argc==2);test_dir=argv[1];writefile(1);writefile(2);
 mode=1;store_recount_once();assert(s_counts_valid && s_counts_wifi==3 && s_counts_ble==1);
 s_counts_valid=false;mode=2;fired=0;store_recount_once();assert(!s_counts_valid && !s_recount_active);
 puts("recount: concurrent appends and rewrite invalidation passed");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='rfs-',dir='/tmp') as tmp:
 p=Path(tmp);(p/'j').mkdir();(p/'t.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t'),str(p/'j')],check=True)
