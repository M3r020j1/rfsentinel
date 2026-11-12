#!/usr/bin/env python3
"""Host tests of the journal listing/purge code with synthetic POSIX files."""
from pathlib import Path
import json, subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'main/store.c').read_text()
def fn(marker):
    a=s.rfind('\n',0,s.index(marker))+1; b=s.index('{',a); i=b+1; depth=1
    while depth:
        depth+=(s[i]=='{')-(s[i]=='}'); i+=1
    return s[a:i]+'\n'
a=s.index('#define RFS_SORT_MAX'); b=s.index('\nesp_err_t store_history(',a)
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#define RFS_MOUNT test_dir
#define RFS_OBS_DIR test_dir
#define RFS_OBS_PREFIX "obs_"
#define RFS_LINE_MAX 256
#define RFS_LIST_PAGE 10
#define RFS_HIST_MAX 10
#define portMAX_DELAY 0
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
typedef int esp_err_t;
typedef struct { const char *sort,*dir,*cursor; time_t from,to; } rfs_list_opt_t;
static const char *test_dir;
static bool s_mounted=true,s_counts_valid;
static int s_store_lock=1,s_seen_count;
static uint32_t s_store_generation,s_store_rewrite_generation;
static void xSemaphoreTake(int x,int y){(void)x;(void)y;}
static void xSemaphoreGive(int x){(void)x;}
static bool rewrite_on_yield;
static void vTaskDelay(int n){(void)n;if(rewrite_on_yield){s_store_rewrite_generation++;rewrite_on_yield=false;}}
static bool fail_purge_file, fail_malloc;
static FILE *test_fopen(const char*p,const char*m){if(fail_purge_file && strstr(p,".purge.tmp"))return NULL;return fopen(p,m);}
static void *test_malloc(size_t n){return fail_malloc ? NULL : malloc(n);}
#define fopen test_fopen
#define malloc test_malloc
'''
for name in ['static time_t rfs_timegm(', 'static void json_escape(', 'static int csv_fields(', 'static int csv_read_record(', 'static bool obs_file(', 'static const char *obs_file_time_quality(', 'static bool observation_in_range(']: code+=fn(name)
code+=s[a:b]+'\n'+fn('esp_err_t store_history(')+fn('esp_err_t store_purge(')
code+=r'''
int main(int argc,char **argv){
 assert(argc==2);test_dir=argv[1];
 for(int file=0;file<120;file++){
  char path[200];snprintf(path,sizeof(path),"%s/obs_s%010d_N.csv",test_dir,file+1);
  FILE*f=fopen(path,"w");assert(f);fputs("header\nheader\n",f);
  for(int j=0;j<200;j++){
   int id=file*200+j;
   fprintf(f,"AA:00:00:00:%02X:%02X,ssid%05d,OPEN,2024-01-%02d 12:00:00,1,2412,-42,1,2,0,0,,0,WIFI\n",id/256,id%256,id,id%2+1);
  }
  assert(fclose(f)==0);
 }
 char out[16384],cursor[64]; rfs_list_opt_t opt={.sort="ssid",.dir="asc"};
 assert(store_list(1,NULL,&opt,out,4096)==ESP_OK);puts(out);
 char *p=strstr(out,"\"next_cursor\":\"");assert(p);p+=strlen("\"next_cursor\":\"");
 char *end=strchr(p,'"');assert(end && end-p<(int)sizeof(cursor));memcpy(cursor,p,(size_t)(end-p));cursor[end-p]=0;
 opt.cursor=cursor;assert(store_list(2,NULL,&opt,out,sizeof(out))==ESP_OK);puts(out);
 opt.cursor=NULL;opt.from=1704153600;opt.to=1704239999;
 assert(store_list(1,NULL,&opt,out,sizeof(out))==ESP_OK);puts(out);
 fail_malloc=true;assert(store_list(1,NULL,&opt,out,sizeof(out))==ESP_ERR_NO_MEM);fail_malloc=false;
 int removed=0;fail_purge_file=true;assert(store_purge(1704067200,1704153599,&removed)==ESP_FAIL && removed==0);fail_purge_file=false;
 assert(store_purge(1704067200,1704153599,&removed)==ESP_OK);assert(removed==12000);
 opt.from=opt.to=0;assert(store_list(1,NULL,&opt,out,sizeof(out))==ESP_OK);puts(out);
 rewrite_on_yield=true;assert(store_list(1,NULL,&opt,out,sizeof(out))==ESP_ERR_INVALID_STATE);
 char path[200];snprintf(path,sizeof(path),"%s/record",test_dir);FILE*f=fopen(path,"w+");assert(f);
 fputs("aa,\"a,\"\"b\"\"\nline\",OPEN,2024-01-01 00:00:00,1,2,3,4,5,6,7,8,9,WIFI\n",f);rewind(f);
 char rec[256];assert(csv_read_record(f,rec,sizeof(rec))==1);assert(strstr(rec,"\nline"));assert(csv_read_record(f,rec,sizeof(rec))==0);fclose(f);
 snprintf(path,sizeof(path),"%s/obs_s9999999999_N.csv",test_dir);f=fopen(path,"w");assert(f);
 fputs("header\nheader\naa,\"keep\nline\",OPEN,2024-01-02 00:00:00,1,2,3,4,5,6,7,8,9,WIFI\naa,remove,OPEN,2024-01-01 00:00:00,1,2,3,4,5,6,7,8,9,WIFI\n",f);fclose(f);
 assert(store_purge(1704067200,1704153599,&removed)==ESP_OK && removed==1);f=fopen(path,"r");assert(f);
 assert(csv_read_record(f,rec,sizeof(rec))==1 && csv_read_record(f,rec,sizeof(rec))==1 && csv_read_record(f,rec,sizeof(rec))==1);
 assert(strstr(rec,"keep\nline") && csv_read_record(f,rec,sizeof(rec))==0);fclose(f);
 snprintf(path,sizeof(path),"%s/obs_s9999999998_N.csv",test_dir);f=fopen(path,"w");assert(f);fputs("header\nheader\n",f);
 for(int i=0;i<60;i++)fprintf(f,"11:22:33:44:55:66,history,OPEN,2025-01-01 %02d:%02d:00,1,2412,-42,1,2,0,4,,3,WIFI\n",i/60,i%60);
 fclose(f);assert(store_history("11:22:33:44:55:66",out,sizeof(out))==ESP_OK);puts(out);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rfs-',dir='/tmp') as t:
 p=Path(t); (p/'j').mkdir();(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 r=subprocess.run([str(p/'test'),str(p/'j')],check=True,text=True,capture_output=True)
 pages=[json.loads(x) for x in r.stdout.splitlines()]
 history=pages.pop(); assert history['truncated'] and len(history['observations'])==10
 assert [r['ts'] for r in history['observations']] == [f'2025-01-01 00:{i:02}:00' for i in range(59,49,-1)]
 assert [[v['ssid'] for v in page['observations']] for page in pages] == [
  [f'ssid{x:05}' for x in range(10)], [f'ssid{x:05}' for x in range(10,20)],
  [f'ssid{x:05}' for x in range(1,20,2)], [f'ssid{x:05}' for x in range(1,20,2)]]
 print('24000 observations: sorted pages, date filtering, purge, multiline and latest local history passed')
