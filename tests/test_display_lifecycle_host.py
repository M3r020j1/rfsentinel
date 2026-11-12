"""Exercise framebuffer lifetime against a simulated asynchronous transfer."""
from pathlib import Path
import subprocess
import tempfile

s = (Path(__file__).resolve().parents[1] / 'main/display.c').read_text()
helpers = s[s.index('static bool lcd_transfer_done('):s.index('static void fill_screen(')]
tick = s[s.index('void display_tick('):s.index('bool display_is_ready(')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
typedef void *esp_lcd_panel_io_handle_t; typedef int esp_lcd_panel_io_event_data_t;
typedef int esp_err_t; typedef int BaseType_t;
#define ESP_OK 0
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define MALLOC_CAP_INTERNAL 1
#define RFS_SCREEN_W 160
#define RFS_SCREEN_H 80
#define RFS_BOOT_HOLD_MS 60000
#define RFS_PIN_LCD_BL 38
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static int token, draws, off_count, draw_error, s_lang; static int64_t now;
static bool s_display_ready=true, s_blanked=false, s_transfer_pending=false;
static uint16_t *s_framebuffer; static void *s_transfer_done=(void*)1, *s_panel=(void*)2;
static int64_t s_boot_us;
static void xSemaphoreGiveFromISR(void *s, BaseType_t *w) {(void)s; token=1; *w=1;}
static int xSemaphoreTake(void *s, int timeout) {(void)s;(void)timeout; if(!token)return 0;token=0;return 1;}
static int esp_lcd_panel_draw_bitmap(void *p,int x,int y,int w,int h,void *f){(void)p;(void)x;(void)y;(void)w;(void)h;(void)f;draws++;return draw_error;}
static int64_t esp_timer_get_time(void) {return now;}
static void esp_lcd_panel_disp_on_off(void *p,bool on){(void)p;(void)on;off_count++;}
static void gpio_set_level(int p,int v){(void)p;(void)v;}
void display_blank(void);
'''
main = r'''
int main(void) {
 s_framebuffer=malloc(25600); assert(s_framebuffer);
 send_frame(); assert(draws==1 && s_transfer_pending);
 assert(!wait_for_frame() && s_framebuffer);
 now=60000000; display_tick();
 assert(s_blanked && off_count==1 && s_framebuffer && s_transfer_pending);
 lcd_transfer_done(0,0,0); display_tick();
 assert(!s_framebuffer && !s_transfer_pending && off_count==1);
 display_tick(); assert(off_count==1);
 s_blanked=false; s_framebuffer=malloc(25600); draw_error=1;
 send_frame(); assert(!s_transfer_pending); display_blank(); assert(!s_framebuffer);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'test.c'
    c.write_text(pre + helpers + tick + main)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(Path(d) / 'test')], check=True)
    subprocess.run([str(Path(d) / 'test')], check=True)
print('Display lifecycle: timeout, completion, one-minute shutdown and transfer failure passed')
