"""Check WiGLE's bounded reader with real chunking, truncation and read errors."""
from pathlib import Path
import subprocess, tempfile
src=(Path(__file__).resolve().parents[1]/'main/wigle.c').read_text()
a=src.index('static bool json_salvage('); b=src.index('/* Les deux tables',a)
code=src[a:b]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define WIGLE_BODY_MAX 6144
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE -2
#define ESP_ERR_NOT_FOUND -3
typedef int esp_err_t;
typedef void *esp_http_client_handle_t;
static const char *input; static size_t pos, input_len; static int fail;
static int esp_http_client_read(void *c,char *buf,int len) {
 (void)c; if(fail)return -1; if(pos==input_len)return 0;
 size_t n=input_len-pos; if(n>17)n=17; if(n>(size_t)len)n=(size_t)len;
 memcpy(buf,input+pos,n);pos+=n;return (int)n;
}
'''
main=r'''
int main(void) {
 char buf[WIGLE_BODY_MAX+4], large[WIGLE_BODY_MAX+100]; bool truncated;
 input="{\"results\":[{\"name\":\"a\\\"b\"}]}";input_len=strlen(input);pos=0;
 assert(wigle_read_body(NULL,buf,sizeof(buf),&truncated)==ESP_OK && !truncated && !strcmp(buf,input));
 pos=0;assert(wigle_read_body(NULL,buf,sizeof(buf)-1,&truncated)==ESP_ERR_INVALID_SIZE);
 input="";input_len=0;pos=0;assert(wigle_read_body(NULL,buf,sizeof(buf),&truncated)==ESP_ERR_NOT_FOUND);
 fail=1;assert(wigle_read_body(NULL,buf,sizeof(buf),&truncated)==ESP_FAIL);fail=0;
 const char *prefix="{\"results\":[{\"name\":\"ok\"},";size_t n=strlen(prefix);
 memcpy(large,prefix,n);memset(large+n,' ',sizeof(large)-n);input=large;input_len=sizeof(large);pos=0;
 assert(wigle_read_body(NULL,buf,sizeof(buf),&truncated)==ESP_OK && truncated);
 assert(!strcmp(buf,"{\"results\":[{\"name\":\"ok\"}]}"));
 puts("WiGLE bounded reader: chunks, escaping, truncation, empty and I/O errors passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'t.c';c.write_text(pre+code+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(Path(d)/'t')],check=True)
 subprocess.run([str(Path(d)/'t')],check=True)
