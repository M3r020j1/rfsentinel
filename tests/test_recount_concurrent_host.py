#!/usr/bin/env python3
"""Exercise recount with actual pthread contention and append accounting."""
from pathlib import Path
import importlib.util,subprocess,tempfile
p=Path(__file__).with_name('test_recount_host.py')
spec=importlib.util.spec_from_file_location('recount_fixture',p)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
s=m.code.replace('#include <stdio.h>','#include <pthread.h>\n#include <stdatomic.h>\n#include <sched.h>\n#include <stdio.h>',1)
s=s.replace('static int s_store_lock=1,depth,mode,fired;','static int s_store_lock=1;')
a=s.index('static void xSemaphoreTake(');b=s.index('static int segment_initialize',a)
s=s[:a]+r'''
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static atomic_int go,writes,rewrite_done;
static _Thread_local int depth;
static bool do_rewrite;
static void xSemaphoreTake(int x,int y){(void)x;(void)y;assert(pthread_mutex_lock(&mutex)==0);depth++;}
static void xSemaphoreGive(int x){(void)x;depth--;assert(pthread_mutex_unlock(&mutex)==0);}
static void vTaskDelay(int n){(void)n;atomic_store(&go,1);if(do_rewrite && !depth)while(!atomic_load(&rewrite_done))sched_yield();sched_yield();}
'''+s[b:]
a=s.index('static void writefile(');s=s[:a]+r'''
static void *append_worker(void*arg){
 (void)arg;while(!atomic_load(&go))sched_yield();
 for(int i=0;i<300;i++){
  xSemaphoreTake(s_store_lock,0);char path[180];snprintf(path,sizeof(path),"%s/%s",test_dir,s_segment_name);
  FILE*f=fopen(path,"a");assert(f);fputs("b,b,b,b,b,b,b,b,b,b,b,b,b,BLE\n",f);fclose(f);
  if(s_recount_active)s_recount_ble++;if(s_counts_valid)s_counts_ble++;
  atomic_fetch_add(&writes,1);xSemaphoreGive(s_store_lock);sched_yield();
 }return NULL;
}
static void *rewrite_worker(void*arg){
 (void)arg;while(!atomic_load(&go))sched_yield();xSemaphoreTake(s_store_lock,0);
 s_store_rewrite_generation++;xSemaphoreGive(s_store_lock);atomic_store(&rewrite_done,1);return NULL;
}
int main(int argc,char**argv){
 assert(argc==2);test_dir=argv[1];s_segment_seq=120;strcpy(s_segment_name,"obs_s0000000120_N.csv");
 for(int i=1;i<=120;i++){
  char p[180];snprintf(p,sizeof(p),"%s/obs_s%010d_N.csv",test_dir,i);FILE*f=fopen(p,"w");assert(f);fputs("h\nh\n",f);
  for(int j=0;j<200;j++)fputs("a,a,a,a,a,a,a,a,a,a,a,a,a,WIFI\n",f);fclose(f);
 }
 pthread_t t;assert(pthread_create(&t,NULL,append_worker,NULL)==0);store_recount_once();
 assert(pthread_join(t,NULL)==0 && atomic_load(&writes)==300);
 assert(s_counts_valid && s_counts_wifi==24000 && s_counts_ble==300);
 s_counts_valid=false;atomic_store(&go,0);do_rewrite=true;assert(pthread_create(&t,NULL,rewrite_worker,NULL)==0);
 store_recount_once();assert(pthread_join(t,NULL)==0);assert(!s_counts_valid && !s_recount_active);
 puts("pthread: recount included 300 concurrent writes and rejected a rewrite");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rfs-',dir='/tmp') as tmp:
 root=Path(tmp);(root/'j').mkdir();(root/'t.c').write_text(s)
 subprocess.run(['cc','-std=c11','-pthread','-Wall','-Wextra','-Werror',str(root/'t.c'),'-o',str(root/'t')],check=True)
 subprocess.run([str(root/'t'),str(root/'j')],check=True,timeout=60)
