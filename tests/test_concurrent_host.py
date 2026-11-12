#!/usr/bin/env python3
"""Run production listing and purge against POSIX files with real pthread contention."""
from pathlib import Path
import importlib.util, subprocess, tempfile
p=Path(__file__).with_name('test_journal_host.py')
spec=importlib.util.spec_from_file_location('journal_fixture',p)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
s=m.code
s=s.replace('#include <stdio.h>', '#include <pthread.h>\n#include <stdatomic.h>\n#include <sched.h>\n#include <stdio.h>',1)
a=s.index('static void xSemaphoreTake(');b=s.index('static bool fail_purge_file',a)
s=s[:a]+r'''
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int go, writes, allow_purge, purge_done;
static _Thread_local bool is_purger;
static void xSemaphoreTake(int x,int y){(void)x;(void)y;assert(pthread_mutex_lock(&mutex)==0);}
static void xSemaphoreGive(int x){(void)x;assert(pthread_mutex_unlock(&mutex)==0);}
static void vTaskDelay(int n){(void)n;atomic_store(&go,1);if(atomic_load(&allow_purge) && !is_purger) while(!atomic_load(&purge_done))sched_yield();sched_yield();}
'''+s[b:]
a=s.index('int main(int argc,char **argv)');s=s[:a]+r'''
static void *writer(void *arg){
 (void)arg;while(!atomic_load(&go))sched_yield();
 for(int i=0;i<300;i++){
  xSemaphoreTake(s_store_lock,0);
  char path[180];snprintf(path,sizeof(path),"%s/obs_s0000000120_N.csv",test_dir);
  FILE*f=fopen(path,"a");assert(f);
  assert(fputs("AA:BB:CC:DD:EE:FF,zzzz,OPEN,2024-01-02 12:00:00,1,2412,-42,1,2,0,0,,0,WIFI\n",f)>=0);
  assert(fclose(f)==0);atomic_fetch_add(&writes,1);xSemaphoreGive(s_store_lock);sched_yield();
 }
 return NULL;
}
static void *purger(void *arg){
 (void)arg;is_purger=true;while(!atomic_load(&allow_purge))sched_yield();int removed=0;
 assert(store_purge(1704067200,1704153599,&removed)==ESP_OK);assert(removed>0);
 atomic_store(&purge_done,1);return NULL;
}
int main(int argc,char**argv){
 assert(argc==2);test_dir=argv[1];
 for(int i=0;i<120;i++){
  char path[180];snprintf(path,sizeof(path),"%s/obs_s%010d_N.csv",test_dir,i+1);
  FILE*f=fopen(path,"w");assert(f);fputs("header\nheader\n",f);
  for(int j=0;j<200;j++)fprintf(f,"AA:BB:CC:DD:EE:FF,ssid%05d,OPEN,2024-01-%02d 12:00:00,1,2412,-42,1,2,0,0,,0,WIFI\n",i*200+j,j%2+1);
  fclose(f);
 }
 pthread_t t;char out[16384];rfs_list_opt_t opt={.sort="ssid",.dir="asc"};
 assert(pthread_create(&t,NULL,writer,NULL)==0);
 assert(store_list(1,NULL,&opt,out,sizeof(out))==ESP_OK);
 assert(atomic_load(&writes)>0);assert(pthread_join(t,NULL)==0);assert(atomic_load(&writes)==300);
 atomic_store(&go,0);atomic_store(&allow_purge,0);
 assert(pthread_create(&t,NULL,purger,NULL)==0);
 atomic_store(&allow_purge,1);
 int result=store_list(1,NULL,&opt,out,sizeof(out));
 assert(result==ESP_ERR_INVALID_STATE);
 assert(pthread_join(t,NULL)==0 && atomic_load(&purge_done));
 puts("pthread: 300 concurrent appends progressed during sorted listing; concurrent purge completed");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rfs-',dir='/tmp') as tmp:
 root=Path(tmp);(root/'j').mkdir();(root/'t.c').write_text(s)
 subprocess.run(['cc','-std=c11','-pthread','-Wall','-Wextra','-Werror',str(root/'t.c'),'-o',str(root/'t')],check=True)
 subprocess.run([str(root/'t'),str(root/'j')],check=True,timeout=60)
