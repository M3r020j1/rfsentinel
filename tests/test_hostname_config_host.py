#!/usr/bin/env python3
"""Exercise production hostname validation and persistent-write failure paths."""
from pathlib import Path
import subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'main/config.c').read_text()
a=s.index('bool config_hostname_valid(');b=s.index('\nesp_err_t config_set_wifi(',a)
pre=r'''
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef int esp_err_t;typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define CFG_HOSTNAME_MAX 33
#define NVS_NS "rfsentinel"
#define NVS_READWRITE 1
#define K_HOSTNAME "hostname"
static struct {char hostname[64];} s_cfg={"rfsentinel"};
static int failure;static char persisted[64];
static size_t rfs_strlcpy(char*d,const char*s,size_t n){size_t k=strlen(s);if(n){size_t z=k<n-1?k:n-1;memcpy(d,s,z);d[z]=0;}return k;}
#undef strlcpy
#define strlcpy rfs_strlcpy
static int nvs_open(const char*n,int f,int*h){(void)n;(void)f;*h=1;return failure==1?1:0;}
static int nvs_set_str(int h,const char*k,const char*v){(void)h;(void)k;if(failure==2)return 2;strlcpy(persisted,v,sizeof(persisted));return 0;}
static int nvs_commit(int h){(void)h;return failure==3?3:0;}
static void nvs_close(int h){(void)h;}
'''
main=r'''
int main(void){
 assert(config_hostname_valid("rfsentinel") && config_hostname_valid("rfsentinel-2") && config_hostname_valid("a"));
 assert(!config_hostname_valid("") && !config_hostname_valid("-abc") && !config_hostname_valid("abc-") && !config_hostname_valid("ABC") && !config_hostname_valid("abc.local") && !config_hostname_valid("abc_def"));
 char max[34];memset(max,'a',32);max[32]=0;assert(config_hostname_valid(max));max[32]='a';max[33]=0;assert(!config_hostname_valid(max));
 assert(config_set_hostname("rfsentinel-2")==ESP_OK && !strcmp(s_cfg.hostname,"rfsentinel-2") && !strcmp(persisted,"rfsentinel-2"));
 for(failure=1;failure<=3;failure++){assert(config_set_hostname("other")!=ESP_OK);assert(!strcmp(s_cfg.hostname,"rfsentinel-2"));}
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(pre+s[a:b]+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
assert 'load_str(h, K_HOSTNAME, s_cfg.hostname' in s
print('Hostname: validation, NVS save failures and startup load passed')
