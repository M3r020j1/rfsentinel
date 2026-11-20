from pathlib import Path
import subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'main/net.c').read_text()
a=s.index('    s_netif = esp_netif_create_default_wifi_sta();');b=s.index('    wifi_init_config_t cfg_init',a)
pre=r'''
#include <assert.h>
#include <string.h>
typedef int esp_err_t;typedef int esp_netif_t;
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static esp_netif_t netif,*s_netif;static const struct {const char *hostname;} config={"rfsentinel-2"},*cfg=&config;
static char dhcp[64],mdns[64];static int mdns_init_result,mdns_set_result,freed;
static esp_netif_t *esp_netif_create_default_wifi_sta(void){return &netif;}
static void esp_netif_create_default_wifi_ap(void){}
static int esp_netif_set_hostname(esp_netif_t*n,const char*v){assert(n==&netif);strcpy(dhcp,v);return 0;}
static int mdns_init(void){return mdns_init_result;}
static int mdns_hostname_set(const char*v){strcpy(mdns,v);return mdns_set_result;}
static void mdns_free(void){freed++;}
static void setup(void){
'''
main=r'''
}
int main(void){setup();assert(!strcmp(dhcp,"rfsentinel-2")&&!strcmp(mdns,dhcp)&&freed==0);
 mdns_set_result=1;setup();assert(freed==1);mdns_set_result=0;mdns_init_result=1;setup();assert(freed==1);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(pre+s[a:b]+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
print('Hostname network setup: same name and mDNS failure paths passed')
