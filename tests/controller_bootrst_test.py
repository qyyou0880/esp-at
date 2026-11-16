"""Exercise production BOOT/NRST GPIO sequencing and persistence failures."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int nvs_handle_t;typedef int esp_err_t;
#define ESP_OK 0
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define GPIO_MODE_OUTPUT_OD 3
#define GPIO_MODE_INPUT 0
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
#define ESP_AT_PARA_PARSE_RESULT_OK 0
#define ESP_AT_RESULT_CODE_ERROR 1
#define ESP_AT_RESULT_CODE_OK 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
static int s_lock=1;static bool s_powerdown;static uint64_t s_owned_pins;
static struct {int gpio;} s_state={10};
static int levels[32],modes[32],lowpulses,delays;static bool saveok=true;
static unsigned char stored[32];static size_t storedlen;
static int gpio_set_level(int p,int v){assert(p>=0&&p<32);levels[p]=v;if(p==11&&!v)++lowpulses;return 0;}
static int gpio_set_direction(int p,int m){modes[p]=m;return 0;}
static int gpio_config(const gpio_config_t *c){for(int i=0;i<32;i++)if(c->pin_bit_mask&(1ULL<<i))modes[i]=c->mode;return 0;}
static int gpio_sleep_sel_dis(int p){(void)p;return 0;}
static bool link_gpio_pin_allowed(int p){return p==10||p==11||p==14;}
static bool link_gpio_prepare_pin(int p,bool h){return gpio_set_level(p,h)==0;}
static bool link_gpio_state_configure(void *s,int p){(void)s;s_state.gpio=p;return true;}
static void xSemaphoreTake(int s,int t){(void)s;(void)t;}
static void xSemaphoreGive(int s){(void)s;}
static void vTaskDelay(int t){delays+=t;}
static int nvs_open(const char*n,int m,int*h){(void)n;(void)m;*h=1;return 0;}
static int nvs_set_blob(int h,const char*k,const void*v,size_t l){(void)h;(void)k;if(!saveok)return 1;memcpy(stored,v,l);storedlen=l;return 0;}
static int nvs_get_blob(int h,const char*k,void*v,size_t*l){(void)h;(void)k;if(*l!=storedlen)return 1;memcpy(v,stored,*l);return 0;}
static int nvs_commit(int h){(void)h;return saveok?0:1;}
static void nvs_close(int h){(void)h;}
static int esp_at_get_para_as_digit(int n,int32_t*p){(void)n;(void)p;return 1;}
static void esp_at_port_write_data(uint8_t*p,size_t l){(void)p;(void)l;}
#include "SOURCE"
int main(void){
 assert(!esp_at_bootrst_configure(10,10,true));
 assert(!esp_at_bootrst_configure(6,11,true));
 assert(!esp_at_bootrst_mode(true));
 assert(esp_at_bootrst_configure(10,11,true));
 assert(esp_at_bootrst_uart_is_data());
 assert(s_state.gpio==-1&&modes[11]==GPIO_MODE_OUTPUT_OD&&levels[11]==1&&levels[10]==0);
 assert(esp_at_bootrst_mode(true));assert(levels[10]==1&&levels[11]==1&&lowpulses==1&&delays==110);
 assert(esp_at_bootrst_configure(10,11,true));assert(levels[10]==1&&lowpulses==1);
 assert(esp_at_bootrst_mode(false));assert(levels[10]==0&&levels[11]==1&&lowpulses==2);
 assert(!esp_at_bootrst_configure(11,10,true));
 assert(esp_at_bootrst_mode(true));bootrst_release();assert(levels[10]==0&&levels[11]==1);
 saveok=false;assert(!esp_at_bootrst_configure(10,11,false));assert(s_bootrst.enabled);
 saveok=true;assert(esp_at_bootrst_configure(10,11,false));assert(!s_bootrst.enabled&&modes[11]==GPIO_MODE_INPUT);
 assert(!esp_at_bootrst_mode(true));assert(!esp_at_bootrst_uart_is_data());
 assert(esp_at_bootrst_configure(10,11,true));s_bootrst.enabled=false;bootrst_load();assert(s_bootrst.enabled&&levels[10]==0&&levels[11]==1);
 s_powerdown=true;assert(!esp_at_bootrst_mode(true));
 assert(bootrst_setup(0)==1&&bootmode_setup(0)==1);bootrst_query((uint8_t*)"+BOOTRST");
 puts("BOOTRST production GPIO: open drain, pulse order, persistence, validation, release PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code.replace('SOURCE',(root/'main/bootrst.inc').as_posix()))
 gcc='D:/msys64/ucrt64/bin/gcc.exe' if os.name=='nt' else 'gcc'
 subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
