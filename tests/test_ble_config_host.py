from pathlib import Path
import subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'main/config.c').read_text()
a=s.index('esp_err_t config_set_ble_address_filter('); b=s.index('\nesp_err_t config_set_options(',a)
assert 'load_u8(h, K_BLE_FILTER, &s_cfg.ble_address_filter, RFS_BLE_FILTER_STABLE)' in s
code=r'''
#include <assert.h>
#include <stdint.h>
typedef int esp_err_t; typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define RFS_BLE_FILTER_PUBLIC 2
#define NVS_NS "rfsentinel"
#define NVS_READWRITE 1
#define K_BLE_FILTER "ble_filter"
static struct {uint8_t ble_address_filter;} s_cfg={1};
static int failure, stored;
static int nvs_open(const char *a,int b,int *h){(void)a;(void)b;*h=1;return failure==1?1:0;}
static int nvs_set_u8(int h,const char*k,uint8_t v){(void)h;(void)k;if(failure==2)return 2;stored=v;return 0;}
static int nvs_commit(int h){(void)h;return failure==3?3:0;}
static void nvs_close(int h){(void)h;}
'''+s[a:b]+r'''
int main(void){
 assert(config_set_ble_address_filter(2)==0 && s_cfg.ble_address_filter==2 && stored==2);
 assert(config_set_ble_address_filter(3)==ESP_ERR_INVALID_ARG && s_cfg.ble_address_filter==2);
 for(failure=1;failure<=3;failure++){assert(config_set_ble_address_filter(0)!=0);assert(s_cfg.ble_address_filter==2);}
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d); (p/'t.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
print('BLE filter configuration: default, validation and NVS errors passed')
