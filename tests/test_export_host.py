#!/usr/bin/env python3
"""Exercise the actual store export code on host files; not a LittleFS test."""
from pathlib import Path
import subprocess, tempfile

source = (Path(__file__).resolve().parents[1] / 'main/store.c').read_text()
def function(marker):
    start = source.rfind('\n', 0, source.index(marker)) + 1
    brace = source.index('{', start)
    depth, pos = 1, brace + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]

start = source.index('#define EXPORT_NAME_BATCH 16')
end = source.index('\nesp_err_t store_stats(', start)
code = r'''
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
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#define RFS_OBS_DIR test_dir
#define RFS_OBS_PREFIX "obs_"
#define RFS_LINE_MAX 256
#define portMAX_DELAY 0
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_SIZE 5
typedef int esp_err_t;
typedef struct store_export store_export_t;
static const char *test_dir;
static bool s_mounted = true;
static int s_store_lock = 1;
static uint32_t s_segment_seq = 600, s_store_rewrite_generation;
static char s_segment_name[32] = "obs_s0000000600_N.csv";
static const char CSV_HDR[] = "WigleWifi-1.6,apssids=true,latlon=true,lockedToNetwork=false,cellBands=auto\nMAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";
static void xSemaphoreTake(int x,int y){(void)x;(void)y;}
static void xSemaphoreGive(int x){(void)x;}
static void taskYIELD(void){}
static void vTaskDelay(int n){(void)n;}
static int segment_initialize(void){return ESP_OK;}
static bool fail_alloc;
static void *test_calloc(size_t n,size_t size){return fail_alloc ? NULL : calloc(n,size);}
#define calloc test_calloc
'''
code += function('static bool segment_number(') + '\n'
code += function('static bool obs_file(') + '\n'
code += function('static time_t rfs_timegm(') + '\n'
code += function('esp_err_t store_export_record_in_range(') + '\n'
code += source[start:end]
code += r'''
#define RFS_EXPORT_BUF 4096
#define TAG "test"
typedef struct { char data[32768]; size_t used; bool ended, interrupt, send_fail; int status; } httpd_req_t;
static bool req_authorized(httpd_req_t*r){(void)r;return true;}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t*r,char *out,size_t n){(void)r;strlcpy(out,"from=1709164800&to=1709251199",n);return ESP_OK;}
static esp_err_t httpd_query_key_value(const char*q,const char*k,char*out,size_t n){
 (void)q;strlcpy(out,!strcmp(k,"from")?"1709164800":"1709251199",n);return ESP_OK;}
static void send_json(httpd_req_t*r,int code,const char*body){(void)body;r->status=code;}
static void send_err(httpd_req_t*r,const char*e){(void)e;r->status=500;}
static void export_filename(char*out,size_t n){strlcpy(out,"test.csv",n);}
static void httpd_resp_set_type(httpd_req_t*r,const char*v){(void)r;(void)v;}
static void httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){(void)r;(void)k;(void)v;}
static esp_err_t httpd_resp_send_chunk(httpd_req_t*r,const char*b,size_t n){
 if(!b){r->ended=true;return ESP_OK;} assert(r->used+n<sizeof(r->data));
 if(r->send_fail)return ESP_FAIL;
 memcpy(r->data+r->used,b,n);r->used+=n;r->data[r->used]=0;
 if(r->interrupt){s_store_rewrite_generation++;r->interrupt=false;}return ESP_OK;}
static const char *esp_err_to_name(int e){(void)e;return "error";}
static void mock_log(const char *fmt,...){(void)fmt;}
#define ESP_LOGW(tag,...) mock_log(__VA_ARGS__)
#define ESP_LOGI(tag,...) mock_log(__VA_ARGS__)
'''
web=(Path(__file__).resolve().parents[1]/'main/web.c').read_text()
web_start=web.index('static esp_err_t h_export(')
web_end=web.index('\n/* --- time --- */',web_start)
code+=web[web_start:web_end]
code += r'''
static void make_file(int i) {
    char path[256];
    snprintf(path,sizeof(path),"%s/obs_s%010d_N.csv",test_dir,i);
    FILE *f=fopen(path,"w"); assert(f);
    fputs(CSV_HDR,f); fprintf(f,"row%04d\n",i); assert(fclose(f)==0);
}
int main(int argc, char **argv) {
    assert(argc==2); test_dir=argv[1];
    store_export_t *oom_handle=NULL; fail_alloc=true;
    assert(store_export_open(&oom_handle)==ESP_ERR_NO_MEM && !oom_handle); fail_alloc=false;
    bool match = false;
    const char *record = "aa,\"a,\"\"quoted\"\"\nname\",auth,2024-02-29 23:59:59,1,2,3,4,5,6,7,8,9,WIFI\r\n";
    assert(store_export_record_in_range(record,1709164800,1709251199,&match)==ESP_OK && match);
    assert(store_export_record_in_range(record,1709251200,1709337599,&match)==ESP_OK && !match);
    assert(store_export_record_in_range("aa,\"bad\"x,auth,2024-02-29 23:59:59,1,2,3,4,5,6,7,8,9,WIFI\n",0,2000000000,&match)!=ESP_OK);
    assert(store_export_record_in_range("aa,name,auth,2023-02-29 23:59:59,1,2,3,4,5,6,7,8,9,WIFI\n",0,2000000000,&match)!=ESP_OK);
    assert(store_export_record_in_range("aa,name,auth,0,1,2,3,4,5,6,7,8,9,WIFI\n",0,2000000000,&match)==ESP_OK && !match);
    char long_record[450], long_ssid[301];memset(long_ssid,'x',300);long_ssid[300]=0;
    snprintf(long_record,sizeof(long_record),"aa,\"%s\",auth,2024-02-29 23:59:59,1,2,3,4,5,6,7,8,9,WIFI\n",long_ssid);
    assert(store_export_record_in_range(long_record,1709164800,1709251199,&match)==ESP_OK && match);
    char webdir[200];snprintf(webdir,sizeof(webdir),"%s/web",test_dir);assert(mkdir(webdir,0700)==0);
    const char *original_dir=test_dir;test_dir=webdir;
    char webpath[240];snprintf(webpath,sizeof(webpath),"%s/obs_s0000000001_N.csv",test_dir);
    FILE *wf=fopen(webpath,"w");assert(wf);fputs(CSV_HDR,wf);
    for(int i=0;i<130;i++)fputs("aa,\"a,\"\"quoted\"\"\nname\",auth,2024-02-29 23:59:59,1,2,3,4,5,6,7,8,9,WIFI\n",wf);
    fputs("bb,excluded,auth,2024-03-01 00:00:00,1,2,3,4,5,6,7,8,9,WIFI\n",wf);fclose(wf);
    httpd_req_t req={0};assert(h_export(&req)==ESP_OK && req.ended && req.used>4096);
    assert(strstr(req.data,"\"\"quoted\"\"\nname") && !strstr(req.data,"excluded"));
    httpd_req_t interrupted={.interrupt=true};assert(h_export(&interrupted)!=ESP_OK && interrupted.used>0 && !interrupted.ended);
    httpd_req_t send_error={.send_fail=true};assert(h_export(&send_error)!=ESP_OK && !send_error.ended);
    wf=fopen(webpath,"a");assert(wf);fputs("aa,\"",wf);for(int i=0;i<510;i++)fputc('x',wf);fputs("\",auth,2024-02-29 23:59:59,1,2,3,4,5,6,7,8,9,WIFI\n",wf);fclose(wf);
    httpd_req_t oversized={0};assert(h_export(&oversized)!=ESP_OK && !oversized.ended);
    assert(unlink(webpath)==0);assert(rmdir(webdir)==0);test_dir=original_dir;
    for(int i=1;i<=600;i++)make_file(i);
    store_export_t *h=NULL; assert(store_export_open(&h)==ESP_OK);
    long expected=(long)strlen(CSV_HDR)+600*8;
    assert(store_export_size(h)==expected);
    char path[256]; snprintf(path,sizeof(path),"%s/%s",test_dir,s_segment_name);
    FILE *f=fopen(path,"a"); assert(f); fputs("extra\n",f); fclose(f);
    make_file(601); s_segment_seq=601;
    char buf[257]; long total=0; size_t got; int row=0;
    char *all=malloc((size_t)expected+1); assert(all);
    do { assert(store_export_read(h,buf,sizeof(buf),&got)==ESP_OK);
         assert(total+(long)got<=expected); memcpy(all+total,buf,got); total+=(long)got; } while(got);
    assert(total==expected); all[total]=0;
    assert(strncmp(all,CSV_HDR,strlen(CSV_HDR))==0);
    char *p=all+strlen(CSV_HDR); while(*p){char want[16];snprintf(want,sizeof(want),"row%04d\n",++row);assert(!strncmp(p,want,8));p+=8;}
    assert(row==600); free(all); store_export_close(h);
    assert(store_export_open(&h)==ESP_OK); s_store_rewrite_generation++;
    assert(store_export_read(h,buf,sizeof(buf),&got)==ESP_ERR_INVALID_STATE); store_export_close(h);
    assert(store_export_open(&h)==ESP_OK);
    snprintf(path,sizeof(path),"%s/obs_s0000000001_N.csv",test_dir); assert(unlink(path)==0);
    int err=0; do { err=store_export_read(h,buf,sizeof(buf),&got); } while(err==ESP_OK && got);
    assert(err!=ESP_OK); store_export_close(h);
    puts("600 segments: ordered snapshot, concurrent append, rewrite and missing file passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rfs-', dir='/tmp') as tmp:
    root = Path(tmp)
    (root/'journal').mkdir()
    (root/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(root/'test.c'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test'),str(root/'journal')],check=True)
