#!/usr/bin/env python3
"""Exercise production export with real pthread append and rotation interleavings."""
from pathlib import Path
import importlib.util, subprocess, tempfile
p=Path(__file__).with_name('test_export_host.py')
spec=importlib.util.spec_from_file_location('export_fixture',p)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
s=m.code.replace('#include <stdio.h>', '#include <pthread.h>\n#include <stdatomic.h>\n#include <sched.h>\n#include <stdio.h>',1)
a=s.index('static void xSemaphoreTake(');b=s.index('static int segment_initialize',a)
s=s[:a]+r'''
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static atomic_int go,writes;
static void xSemaphoreTake(int x,int y){(void)x;(void)y;assert(pthread_mutex_lock(&mutex)==0);}
static void xSemaphoreGive(int x){(void)x;assert(pthread_mutex_unlock(&mutex)==0);}
static void taskYIELD(void){sched_yield();}
static void vTaskDelay(int n){(void)n;sched_yield();}
'''+s[b:]
a=s.index('#define RFS_EXPORT_BUF');s=s[:a]+r'''
static void *writer(void*unused){
 (void)unused;while(!atomic_load(&go))sched_yield();
 for(int i=0;i<300;i++){
  xSemaphoreTake(s_store_lock,0);char p[180];snprintf(p,sizeof(p),"%s/%s",test_dir,s_segment_name);
  FILE*f=fopen(p,"a");assert(f);assert(fputs("NEW_RECORD\n",f)>=0);fclose(f);
  atomic_fetch_add(&writes,1);xSemaphoreGive(s_store_lock);sched_yield();
 }
 return NULL;
}
static void *rotate(void*unused){
 (void)unused;while(!atomic_load(&go))sched_yield();xSemaphoreTake(s_store_lock,0);
 char p[180];snprintf(p,sizeof(p),"%s/obs_s0000000001_N.csv",test_dir);
 assert(unlink(p)==0);s_store_rewrite_generation++;xSemaphoreGive(s_store_lock);return NULL;
}
int main(int argc,char**argv){
 assert(argc==2);test_dir=argv[1];s_segment_seq=120;strcpy(s_segment_name,"obs_s0000000120_N.csv");
 for(int i=1;i<=120;i++){
  char p[180];snprintf(p,sizeof(p),"%s/obs_s%010d_N.csv",test_dir,i);
  FILE*f=fopen(p,"w");assert(f);fputs(CSV_HDR,f);
  for(int j=0;j<200;j++)fprintf(f,"record%03d_%03d\n",i,j);fclose(f);
 }
 store_export_t*h=NULL;assert(store_export_open(&h)==ESP_OK);long expected=store_export_size(h),total=0;
 char buf[1024];char *snapshot=malloc((size_t)expected+1);assert(snapshot);size_t got;pthread_t t;assert(pthread_create(&t,NULL,writer,NULL)==0);
 assert(store_export_read(h,buf,sizeof(buf),&got)==ESP_OK && got>0);memcpy(snapshot+total,buf,got);total+=(long)got;atomic_store(&go,1);
 while(atomic_load(&writes)==0)sched_yield();
 do {assert(store_export_read(h,buf,sizeof(buf),&got)==ESP_OK);assert(total+(long)got<=expected);memcpy(snapshot+total,buf,got);total+=(long)got;}while(got);
 assert(pthread_join(t,NULL)==0 && atomic_load(&writes)==300 && total==expected);snapshot[total]=0;assert(!strstr(snapshot,"NEW_RECORD"));free(snapshot);store_export_close(h);
 assert(store_export_open(&h)==ESP_OK);assert(store_export_read(h,buf,sizeof(buf),&got)==ESP_OK && got>0);
 atomic_store(&go,0);assert(pthread_create(&t,NULL,rotate,NULL)==0);atomic_store(&go,1);assert(pthread_join(t,NULL)==0);
 assert(store_export_read(h,buf,sizeof(buf),&got)==ESP_ERR_INVALID_STATE);store_export_close(h);
 puts("pthread: export retained snapshot during 300 appends and detected rotation");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rfs-',dir='/tmp') as tmp:
 root=Path(tmp);(root/'j').mkdir();(root/'t.c').write_text(s)
 subprocess.run(['cc','-std=c11','-pthread','-Wall','-Wextra','-Werror',str(root/'t.c'),'-o',str(root/'t')],check=True)
 subprocess.run([str(root/'t'),str(root/'j')],check=True,timeout=60)
