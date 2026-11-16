from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

root = Path(__file__).resolve().parent
headers = {
    'sdkconfig.h': '#define CONFIG_AT_SELF_COMMAND_SUPPORT 1\n',
    'freertos/FreeRTOS.h': '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
typedef unsigned TickType_t;
typedef int portMUX_TYPE;
extern int critical;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x),++critical)
#define portEXIT_CRITICAL(x) ((void)(x),--critical)
#define BIT(n) (1U<<(n))
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define pdFALSE 0
''',
    'freertos/task.h': '#pragma once\nunsigned xTaskGetTickCount(void);\n',
    'freertos/event_groups.h': '''#pragma once
typedef unsigned EventBits_t;
typedef unsigned *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
unsigned xEventGroupSetBits(EventGroupHandle_t,unsigned);
unsigned xEventGroupClearBits(EventGroupHandle_t,unsigned);
unsigned xEventGroupWaitBits(EventGroupHandle_t,unsigned,int,int,unsigned);
''',
    'esp_at_core.h': '',
    'esp_at.h': '''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
void esp_at_port_recv_data_notify(unsigned,unsigned);
''',
}
harness = r'''
#include <stdio.h>
#include <assert.h>
#include "PARENT_SOURCE"
int critical;
static unsigned bits,ticks;
static int mode;
unsigned xTaskGetTickCount(void){return ticks;}
EventGroupHandle_t xEventGroupCreate(void){assert(!critical);return &bits;}
unsigned xEventGroupSetBits(EventGroupHandle_t p,unsigned v){assert(!critical);return *p|=v;}
unsigned xEventGroupClearBits(EventGroupHandle_t p,unsigned v){assert(!critical);unsigned old=*p;*p&=~v;return old;}
unsigned xEventGroupWaitBits(EventGroupHandle_t p,unsigned v,int clear,int all,unsigned wait)
{(void)all;assert(!critical);unsigned old=*p;if(clear)*p&=~v;if(!(old&v))ticks+=wait;return old;}
void esp_at_port_recv_data_notify(unsigned len,unsigned wait)
{
 (void)wait;assert(!critical);assert(at_self_cmd_get_mode());
 assert((unsigned)at_self_cmd_get_data_len()==len);
 uint8_t b[3];unsigned got=0;int n;
 while((n=at_self_cmd_read_data(b,sizeof(b)))>0)got+=n;
 assert(got==len);assert(at_self_cmd_get_data_len()==0);
 if(mode==1)return;
 if(mode==3)assert(esp_at_exe_cmd("AT\r\n","OK",5)==ESP_ERR_INVALID_STATE);
 if(mode==2){at_self_cmd_write_data((uint8_t*)"\r\nER",4);at_self_cmd_write_data((uint8_t*)"ROR\r\n",5);return;}
 at_self_cmd_write_data((uint8_t*)"\r\nO",3);
 assert(at_self_cmd_get_mode());
 at_self_cmd_write_data((uint8_t*)"K\r",2);
 assert(at_self_cmd_get_mode());
 at_self_cmd_write_data((uint8_t*)"\n",1);
}
int main(void)
{
 assert(esp_at_exe_cmd("AT\r\n","\r\nOK\r\n",10)==ESP_OK);
 assert(!at_self_cmd_get_mode());
 mode=3;assert(esp_at_exe_cmd("AT\r\n","\r\nOK\r\n",10)==ESP_OK);
 mode=2;assert(esp_at_exe_cmd("AT\r\n","\r\nOK\r\n",10)==ESP_FAIL);
 mode=1;assert(esp_at_exe_cmd("AT\r\n","\r\nOK\r\n",10)==ESP_ERR_TIMEOUT);
 assert(at_self_cmd_get_mode());
 assert(esp_at_exe_cmd("AT\r\n","\r\nOK\r\n",10)==ESP_ERR_INVALID_STATE);
 at_self_cmd_write_data((uint8_t*)"\r\nERROR\r\n",9);
 assert(!at_self_cmd_get_mode());
 mode=0;assert(esp_at_exe_cmd("AT\r\n","\r\nOK\r\n",10)==ESP_OK);
 assert(esp_at_exe_cmd("AT\r\n","",10)==ESP_ERR_INVALID_ARG);
 assert(!critical);
 puts("Production self-command capture: split response, error, reentrancy, timeout and late completion PASS");
}
'''
with TemporaryDirectory() as directory:
    path = Path(directory)
    for name, content in headers.items():
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    source = Path(__import__('sys').argv[1]).resolve() if len(__import__('sys').argv)>1 else root/'parent_at_self_cmd.c'
    (path/'test.c').write_text(harness.replace('PARENT_SOURCE',source.as_posix()))
    gcc = 'D:/msys64/ucrt64/bin/gcc.exe' if __import__('os').name=='nt' else 'gcc'
    subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror','-I',str(path),str(path/'test.c'),'-o',str(path/'test.exe')],check=True)
    subprocess.run([str(path/'test.exe')],check=True)
