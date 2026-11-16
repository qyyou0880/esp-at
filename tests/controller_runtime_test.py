"""Execute production air-service code with deterministic driver/RTOS mocks."""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess
import os

root=Path(__file__).resolve().parents[1]
stub=r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef unsigned TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(x) (x)
#define portTICK_PERIOD_MS 1
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
typedef struct queue {unsigned count,first,capacity,size;uint8_t *data;} *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned,unsigned);
int xQueueSend(QueueHandle_t,const void*,unsigned);
int xQueueReceive(QueueHandle_t,void*,unsigned);
void xQueueReset(QueueHandle_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
typedef void *TaskHandle_t;
int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
unsigned xTaskGetTickCount(void);
void vTaskDelay(unsigned);
typedef int uart_port_t;
typedef struct {int baud_rate,data_bits,parity,stop_bits,flow_ctrl;} uart_config_t;
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_PARITY_EVEN 2
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
void at_uart_config_init(uart_config_t*);
bool at_nvs_uart_config_get(uart_config_t*);
bool at_nvs_uart_config_set_checked(const uart_config_t*);
int at_uart_port_get(void);
int uart_get_baudrate(int,uint32_t*);
int uart_get_word_length(int,int*);
int uart_get_parity(int,int*);
int uart_get_stop_bits(int,int*);
int uart_get_hw_flow_ctrl(int,int*);
int uart_param_config(int,const uart_config_t*);
int uart_wait_tx_done(int,unsigned);
int uart_write_bytes(int,const void*,size_t);
int uart_disable_pattern_det_intr(int);
bool esp_at_link_gpio_configure(int,bool);
bool esp_at_bootrst_configure(int,int,bool);
bool esp_at_bootrst_mode(bool);
void esp_at_bootrst_get(int*,int*,bool*);
void esp_at_link_gpio_get_state(int*,bool*,unsigned*,bool*);
void esp_at_restart(void);
int esp_at_exe_cmd(const char*,const char*,unsigned);
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_CONNECT 2
#define BLE_GAP_EVENT_SUBSCRIBE 3
struct ble_gap_event {
 int type;
 struct {struct {uint16_t conn_handle;} conn;} disconnect;
 struct {int status;uint16_t conn_handle;} connect;
 struct {uint16_t conn_handle,attr_handle;bool cur_notify;} subscribe;
};
struct ble_gap_event_listener {int unused;};
struct ble_npl_event {void *event;void(*callback)(struct ble_npl_event*);};
struct ble_npl_event_freertos {bool queued;};
struct ble_npl_eventq {void *eventq;};
struct ble_npl_eventq_freertos {QueueHandle_t q;};
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);
void ble_npl_event_init(struct ble_npl_event*,void(*)(struct ble_npl_event*),void*);
void ble_npl_event_deinit(struct ble_npl_event*);
int ble_gap_event_listener_register(struct ble_gap_event_listener*,int(*)(struct ble_gap_event*,void*),void*);
int ble_gap_event_listener_unregister(struct ble_gap_event_listener*);
int ble_gap_terminate(uint16_t,int);
struct ble_gap_upd_params {unsigned itvl_min,itvl_max,latency,supervision_timeout,min_ce_len,max_ce_len;};
int ble_gap_update_params(uint16_t,const struct ble_gap_upd_params*);
int ble_hs_synced(void);
unsigned ble_att_mtu(uint16_t);
struct os_mbuf {unsigned length;uint8_t data[512];};
#define OS_MBUF_PKTLEN(p) ((p)->length)
struct os_mbuf *ble_hs_mbuf_from_flat(const void*,size_t);
int os_mbuf_copydata(struct os_mbuf*,unsigned,unsigned,void*);
int ble_gatts_notify_custom(uint16_t,uint16_t,struct os_mbuf*);
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_NOTIFY 1
#define BLE_GATT_CHR_F_WRITE 2
#define BLE_GATT_CHR_F_WRITE_NO_RSP 4
#define BLE_UUID16_DECLARE(v) ((const void*)(uintptr_t)(v))
struct ble_gatt_access_ctxt {int op;struct os_mbuf *om;};
struct ble_gatt_chr_def {const void *uuid;int(*access_cb)(uint16_t,uint16_t,struct ble_gatt_access_ctxt*,void*);void*arg;unsigned flags;uint16_t*val_handle;};
struct ble_gatt_svc_def {int type;const void*uuid;struct ble_gatt_chr_def*characteristics;};
int ble_gatts_count_cfg(const struct ble_gatt_svc_def*);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def*);
'''
harness=r'''
#include "PRODUCTION_SOURCE"
#include <stdio.h>
static unsigned ticks=100, tasks, terminations, notify_count;
static uint8_t uart_output[4096],ble_output[4096];
static unsigned uart_length,ble_length;
static uint16_t last_handle;
static uart_config_t actual={115200,3,0,1,0};
static struct ble_npl_eventq hostq;
static struct ble_npl_eventq_freertos hostinner;
QueueHandle_t xQueueCreate(unsigned n,unsigned size){QueueHandle_t q=calloc(1,sizeof(*q));q->capacity=n;q->size=size;q->data=calloc(n,size);return q;}
int xQueueSend(QueueHandle_t q,const void*p,unsigned wait){(void)wait;if(q->count==q->capacity)return 0;memcpy(q->data+((q->first+q->count)%q->capacity)*q->size,p,q->size);q->count++;return 1;}
int xQueueReceive(QueueHandle_t q,void*p,unsigned wait){(void)wait;if(!q->count)return 0;memcpy(p,q->data+q->first*q->size,q->size);q->first=(q->first+1)%q->capacity;q->count--;return 1;}
void xQueueReset(QueueHandle_t q){q->count=q->first=0;}
unsigned uxQueueMessagesWaiting(QueueHandle_t q){return q->count;}
int xTaskCreate(void(*f)(void*),const char*n,unsigned stack,void*a,unsigned priority,TaskHandle_t*h){(void)f;(void)n;(void)stack;(void)a;(void)priority;*h=(void*)(uintptr_t)++tasks;return 1;}
unsigned xTaskGetTickCount(void){return ticks;}
void vTaskDelay(unsigned t){ticks+=t;}
void at_uart_config_init(uart_config_t*c){*c=actual;}
bool at_nvs_uart_config_get(uart_config_t*c){*c=actual;return true;}
bool at_nvs_uart_config_set_checked(const uart_config_t*c){(void)c;return true;}
int at_uart_port_get(void){return 1;}
int uart_get_baudrate(int p,uint32_t*v){(void)p;*v=actual.baud_rate;return 0;}
int uart_get_word_length(int p,int*v){(void)p;*v=actual.data_bits;return 0;}
int uart_get_parity(int p,int*v){(void)p;*v=actual.parity;return 0;}
int uart_get_stop_bits(int p,int*v){(void)p;*v=actual.stop_bits;return 0;}
int uart_get_hw_flow_ctrl(int p,int*v){(void)p;*v=actual.flow_ctrl;return 0;}
int uart_param_config(int p,const uart_config_t*c){(void)p;actual=*c;return 0;}
int uart_wait_tx_done(int p,unsigned t){(void)p;(void)t;return 0;}
int uart_disable_pattern_det_intr(int p){(void)p;return 0;}
int uart_write_bytes(int p,const void*d,size_t n){(void)p;assert(uart_length+n<=sizeof(uart_output));memcpy(uart_output+uart_length,d,n);uart_length+=n;return n;}
bool esp_at_bootrst_configure(int b,int r,bool e){(void)b;(void)r;(void)e;return true;}
bool esp_at_bootrst_mode(bool b){(void)b;return true;}
void esp_at_bootrst_get(int*b,int*r,bool*e){*b=10;*r=11;*e=true;}
bool esp_at_link_gpio_configure(int p,bool save){(void)p;(void)save;return true;}
void esp_at_link_gpio_get_state(int*p,bool*h,unsigned*c,bool*f){*p=10;*h=true;*c=1;*f=false;}
void esp_at_restart(void){assert(0 && "Unexpected reboot in host test");}
int esp_at_exe_cmd(const char*c,const char*r,unsigned t){(void)c;(void)r;(void)t;return 0;}
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void){return &hostq;}
void ble_npl_event_init(struct ble_npl_event*e,void(*cb)(struct ble_npl_event*),void*a){(void)a;static struct ble_npl_event_freertos inner;inner.queued=false;e->event=&inner;e->callback=cb;}
void ble_npl_event_deinit(struct ble_npl_event*e){e->event=NULL;}
int ble_gap_event_listener_register(struct ble_gap_event_listener*l,int(*cb)(struct ble_gap_event*,void*),void*a){(void)l;(void)cb;(void)a;return 0;}
int ble_gap_event_listener_unregister(struct ble_gap_event_listener*l){(void)l;return 0;}
int ble_gap_terminate(uint16_t c,int r){(void)c;(void)r;++terminations;return 0;}
int ble_gap_update_params(uint16_t c,const struct ble_gap_upd_params*p){(void)c;assert(p->itvl_min==6&&p->itvl_max==12);return 0;}
int ble_hs_synced(void){return 1;}
unsigned ble_att_mtu(uint16_t c){(void)c;return 23;}
struct os_mbuf *ble_hs_mbuf_from_flat(const void*d,size_t n){struct os_mbuf*m=malloc(sizeof(*m));m->length=n;memcpy(m->data,d,n);return m;}
int os_mbuf_copydata(struct os_mbuf*m,unsigned off,unsigned n,void*d){memcpy(d,m->data+off,n);return 0;}
int ble_gatts_notify_custom(uint16_t c,uint16_t h,struct os_mbuf*m){(void)c;assert(m->length<=20);assert(ble_length+m->length<sizeof(ble_output));memcpy(ble_output+ble_length,m->data,m->length);ble_length+=m->length;last_handle=h;notify_count++;free(m);return 0;}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def*d){(void)d;return 0;}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def*d){uint16_t h=20;for(struct ble_gatt_chr_def*c=d->characteristics;c->uuid;c++,h++)if(c->val_handle)*c->val_handle=h;return 0;}
static void drain(void){while(uxQueueMessagesWaiting(s_input))worker_step();ticks+=AIR_CONFIG_IDLE_DELIMITER_MS+10;worker_step();worker_step();struct ble_npl_event*e;while(xQueueReceive(hostinner.q,&e,0)){((struct ble_npl_event_freertos*)e->event)->queued=false;e->callback(e);}}
static int send_data(uint16_t conn,uint16_t channel,const void*p,unsigned n){struct os_mbuf m={.length=n};memcpy(m.data,p,n);struct ble_gatt_access_ctxt ctx={1,&m};return access_cb(conn,0,&ctx,(void*)(uintptr_t)channel);}
static void config(const char*c){assert(!send_data(1,AIR_BLE_CONFIG_UUID16,c,strlen(c)));drain();}
static void subscribe(bool enabled){struct ble_gap_event e={.type=BLE_GAP_EVENT_SUBSCRIBE,.subscribe={1,s_raw_handle,enabled}};gap_event(&e,NULL);drain();}
static void disconnect(void){struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={{1}}};gap_event(&e,NULL);}
int main(void)
{
 hostinner.q=xQueueCreate(16,sizeof(struct ble_npl_event*));hostq.eventq=&hostinner;
 assert(air_ble_service_register());assert(tasks==1);
 config("AT+BAUD?");assert(strstr((char*)ble_output,"AUTH_REQUIRED"));assert(!uart_length);
 ble_length=0;memset(ble_output,0,sizeof(ble_output));config("AT+AUTH=123456");
 config("AT+TRANMD=1");config("AT+BAUD?");assert(strstr((char*)ble_output,"+OK=7"));assert(!uart_length);
 subscribe(true);assert(air_ble_service_uart_active());
 config("AT+BOOTRST=10,11,1");config("AT+BOOTMODE=1");assert(!uart_length);
 uint8_t frame[258];for(unsigned i=0;i<sizeof(frame);i++)frame[i]=i;memcpy(frame+30,"+++",3);
 assert(!send_data(1,AIR_BLE_WRITE_UUID16,frame,sizeof(frame)));drain();assert(uart_length==258&&!memcmp(frame,uart_output,258));
 ble_length=0;notify_count=0;assert(air_ble_service_uart_route(frame,sizeof(frame)));drain();assert(ble_length==258&&!memcmp(frame,ble_output,258));assert(notify_count==13&&last_handle==s_raw_handle);
 /* Another legacy/GATT connection cannot steal this owner. */
 assert(send_data(2,AIR_BLE_CONFIG_UUID16,"AT+AUTH=123456",14));
 /* Old queued input and output must not reach a reused connection handle. */
 assert(!send_data(1,AIR_BLE_WRITE_UUID16,frame,sizeof(frame)));
 assert(air_ble_service_uart_route(frame,sizeof(frame)));unsigned before=uart_length;
 disconnect();ble_length=0;memset(ble_output,0,sizeof(ble_output));config("AT+BAUD?");assert(uart_length==before);assert(!strstr((char*)ble_output,"+OK=7"));
 assert(!air_ble_service_uart_active());subscribe(true);assert(air_ble_service_uart_active());
 subscribe(false);assert(!air_ble_service_uart_active());
 /* Reinit reuses queues and worker, and invalidates prior authentication. */
 disconnect();air_ble_service_unregister();assert(air_ble_service_register());assert(tasks==1);
 ble_length=0;memset(ble_output,0,sizeof(ble_output));config("AT+BAUD?");assert(strstr((char*)ble_output,"AUTH_REQUIRED"));
 subscribe(true);for(unsigned i=0;i<16;i++)assert(!send_data(1,AIR_BLE_WRITE_UUID16,frame,sizeof(frame)));
 assert(send_data(1,AIR_BLE_WRITE_UUID16,frame,sizeof(frame)));before=uart_length;drain();assert(terminations&&uart_length==before);
 puts("Production air runtime: 258-byte binary, independent config, MTU split, stale traffic, ownership, reinit and overflow PASS");
}
'''
with TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'stub.h').write_text(stub)
    names=['esp_at.h','at_uart.h','esp_at_link_gpio.h','driver/uart.h',
           'freertos/FreeRTOS.h','freertos/queue.h','freertos/task.h',
           'host/ble_gatt.h','host/ble_att.h','host/ble_hs.h','host/ble_uuid.h',
           'nimble/nimble_port.h','nimble/npl_freertos.h','os/os_mbuf.h']
    for name in names:
        target=path/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text('#include "stub.h"\n')
    (path/'sdkconfig.h').write_text(''.join('#define '+name+' 1\n' for name in ['CONFIG_IDF_TARGET_ESP32C6','CONFIG_BT_NIMBLE_ENABLED','CONFIG_AT_BASE_ON_UART','CONFIG_AT_SELF_COMMAND_SUPPORT']))
    (path/'test.c').write_text(harness.replace('PRODUCTION_SOURCE',(root/'main/air_ble_service.c').as_posix()))
    gcc='D:/msys64/ucrt64/bin/gcc.exe' if os.name=='nt' else 'gcc'
    subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror','-I',str(path),'-I',str(root/'main'),str(path/'test.c'),str(root/'main/air_config_parser.c'),'-o',str(path/'test.exe')],check=True)
    subprocess.run([str(path/'test.exe')],check=True)
