/* SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"
#include "air_ble_service.h"
#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_BT_NIMBLE_ENABLED) && defined(CONFIG_AT_BASE_ON_UART) && defined(CONFIG_AT_SELF_COMMAND_SUPPORT)
#include "air_config_parser.h"
#include "esp_at.h"
#include "at_uart.h"
#include "esp_at_link_gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/npl_freertos.h"
#include "os/os_mbuf.h"
#include <string.h>

#define AIR_SESSION 0xffff
#define AIR_RAW_ON 0xfffe
#define AIR_RAW_OFF 0xfffd
#define AIR_MAX_DATA 512
typedef struct {
    uint16_t conn, channel, length;
    uint32_t generation;
    uint8_t data[AIR_MAX_DATA];
} air_message_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_input, s_output;
static TaskHandle_t s_worker, s_manager;
static struct ble_npl_event s_output_event;
static struct ble_gap_event_listener s_listener;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE, s_raw_handle, s_config_handle;
static uint32_t s_generation;
static bool s_registered, s_raw, s_adv_pending, s_configured;
static bool s_failed;
static uint32_t s_fault_generation;
static bool s_fault;
/* Only the worker reads or writes parser state. Host/UART share guarded metadata. */
static air_config_session_t s_session;
static air_message_t s_context;
static TickType_t s_reset_at;

static bool valid(uint16_t conn, uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    bool yes = s_registered && !s_failed && s_conn == conn && s_generation == generation;
    portEXIT_CRITICAL(&s_lock);
    return yes;
}

static void signal_host(void)
{
    /* Pinned IDF NPL's public eventq_put uses portMAX_DELAY. Use its documented
     * FreeRTOS structures here to keep posting bounded during host shutdown. */
    portENTER_CRITICAL(&s_lock);
    if (s_registered) {
        struct ble_npl_event_freertos *event=s_output_event.event;
        struct ble_npl_eventq_freertos *queue=nimble_port_get_dflt_eventq()->eventq;
        if (!event->queued) {
            event->queued=true;
            struct ble_npl_event *pointer=&s_output_event;
            if (xQueueSend(queue->q,&pointer,0) != pdTRUE) {
                event->queued=false;
                s_fault=true; s_fault_generation=s_generation; s_failed=true;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static void fail_session(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    if (s_registered && generation == s_generation) {
        s_fault = true; s_fault_generation = generation; s_failed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    /* Worker posts the fault; never block a GATT callback on the host queue. */
}

static void host_output(struct ble_npl_event *event)
{
    (void)event;
    portENTER_CRITICAL(&s_lock);
    bool fault = s_fault && s_fault_generation == s_generation;
    uint16_t conn = s_conn;
    s_fault = false;
    portEXIT_CRITICAL(&s_lock);
    if (fault && conn != BLE_HS_CONN_HANDLE_NONE)
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    air_message_t item;
    while (xQueueReceive(s_output, &item, 0) == pdTRUE) {
        /* Runs on the same host event queue as GAP disconnect/handle reuse. */
        if (!valid(item.conn, item.generation)) continue;
        size_t mtu = ble_att_mtu(item.conn);
        size_t limit = mtu > 3 ? mtu - 3 : 20;
        for (size_t offset = 0; offset < item.length; ) {
            size_t size = item.length - offset;
            if (size > limit) size = limit;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(item.data + offset, size);
            if (!om || ble_gatts_notify_custom(item.conn, item.channel, om) != 0) {
                /* notify_custom owns om on both success and failure. */
                (void)ble_gap_terminate(item.conn, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            offset += size;
        }
    }
}

static bool output(uint16_t conn, uint32_t generation, uint16_t handle,
                   const uint8_t *data, size_t length)
{
    if (!valid(conn, generation) || !length || length > AIR_MAX_DATA) return false;
    air_message_t item = {.conn=conn,.generation=generation,.channel=handle,.length=length};
    memcpy(item.data, data, length);
    if (xQueueSend(s_output, &item, 0) != pdTRUE) {
        fail_session(generation); return false;
    }
    return true;
}

static bool enqueue(uint16_t conn, uint32_t generation, uint16_t channel,
                    const uint8_t *data, size_t length)
{
    air_message_t item = {.conn=conn,.generation=generation,.channel=channel,.length=length};
    if (length) memcpy(item.data, data, length);
    if (xQueueSend(s_input, &item, 0) == pdTRUE) return true;
    fail_session(generation);
    return false;
}

static bool claim(uint16_t conn, uint32_t *generation)
{
    bool fresh = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_registered || s_failed || (s_conn != BLE_HS_CONN_HANDLE_NONE && s_conn != conn)) {
        portEXIT_CRITICAL(&s_lock); return false;
    }
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        s_conn = conn; ++s_generation; s_raw = false; fresh = true;
    }
    *generation = s_generation;
    portEXIT_CRITICAL(&s_lock);
    return !fresh || enqueue(conn, *generation, AIR_SESSION, NULL, 0);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        portENTER_CRITICAL(&s_lock);
        if (s_conn == event->disconnect.conn.conn_handle) {
            s_conn = BLE_HS_CONN_HANDLE_NONE; ++s_generation; s_raw = false; s_failed = false;
        }
        s_adv_pending = true;
        portEXIT_CRITICAL(&s_lock);
    } else if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status != 0) {
        portENTER_CRITICAL(&s_lock); s_adv_pending = true; portEXIT_CRITICAL(&s_lock);
    } else if (event->type == BLE_GAP_EVENT_SUBSCRIBE &&
               event->subscribe.attr_handle == s_raw_handle) {
        uint32_t generation;
        if (event->subscribe.cur_notify) {
            if (!claim(event->subscribe.conn_handle, &generation)) return 0;
            /* Suppress AT output before CCCD acknowledgement reaches the client. */
            portENTER_CRITICAL(&s_lock); s_raw = true; portEXIT_CRITICAL(&s_lock);
            enqueue(event->subscribe.conn_handle, generation, AIR_RAW_ON, NULL, 0);
        } else {
            portENTER_CRITICAL(&s_lock);
            bool owner = s_conn == event->subscribe.conn_handle;
            generation = s_generation;
            if (owner) s_raw = false;
            portEXIT_CRITICAL(&s_lock);
            if (owner) enqueue(event->subscribe.conn_handle, generation, AIR_RAW_OFF, NULL, 0);
        }
    }
    return 0;
}

static bool current_uart(uart_config_t *config)
{
    at_uart_config_init(config);
    (void)at_nvs_uart_config_get(config);
    uint32_t rate;
    uart_port_t port = at_uart_port_get();
    if (uart_get_baudrate(port, &rate) != ESP_OK ||
        uart_get_word_length(port, &config->data_bits) != ESP_OK ||
        uart_get_parity(port, &config->parity) != ESP_OK ||
        uart_get_stop_bits(port, &config->stop_bits) != ESP_OK ||
        uart_get_hw_flow_ctrl(port, &config->flow_ctrl) != ESP_OK) return false;
    config->baud_rate = rate;
    return true;
}

static void refresh_uart(void)
{
    uart_config_t config;
    if (!current_uart(&config)) return;
    s_session.uart.baudrate = config.baud_rate;
    s_session.uart.parity = config.parity == UART_PARITY_EVEN ? 1 :
        config.parity == UART_PARITY_DISABLE ? 0 : 255;
    s_session.uart.databits = config.data_bits == UART_DATA_8_BITS ? 3 : 255;
}

static void reply(const char *text, size_t length, void *arg)
{
    air_message_t *ctx = arg;
    (void)output(ctx->conn,ctx->generation,s_config_handle,(const uint8_t *)text,length);
}

static bool apply_uart(const air_uart_config_t *config, void *arg)
{
    air_message_t *ctx = arg;
    uart_config_t previous, next;
    if (!valid(ctx->conn,ctx->generation) || !current_uart(&previous) ||
        uart_wait_tx_done(at_uart_port_get(),pdMS_TO_TICKS(1000)) != ESP_OK) return false;
    next = previous;
    next.baud_rate = config->baudrate; next.data_bits = UART_DATA_8_BITS;
    next.parity = config->parity ? UART_PARITY_EVEN : UART_PARITY_DISABLE;
    next.stop_bits = UART_STOP_BITS_1; next.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    if (uart_param_config(at_uart_port_get(),&next) != ESP_OK) return false;
    if (at_nvs_uart_config_set_checked(&next)) return true;
    if (uart_param_config(at_uart_port_get(),&previous) != ESP_OK) fail_session(ctx->generation);
    return false;
}

static bool apply_link(int gpio, bool save, void *arg)
{
    air_message_t *ctx = arg;
    return valid(ctx->conn,ctx->generation) && esp_at_link_gpio_configure(gpio,save);
}

static bool reset(void *arg)
{
    air_message_t *ctx = arg;
    if (!valid(ctx->conn,ctx->generation)) return false;
    s_reset_at = xTaskGetTickCount() + pdMS_TO_TICKS(200);
    if (!s_reset_at) s_reset_at = 1;
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    air_config_callbacks_t callbacks = {.apply_uart=apply_uart,.apply_link=apply_link,
        .request_reset=reset,.reply=reply,.arg=&s_context};
    for (;;) {
        portENTER_CRITICAL(&s_lock); bool fault = s_fault; portEXIT_CRITICAL(&s_lock);
        if (fault || uxQueueMessagesWaiting(s_output)) signal_host();
        air_message_t item;
        if (xQueueReceive(s_input,&item,pdMS_TO_TICKS(10)) == pdTRUE) {
            if (!valid(item.conn,item.generation)) continue;
            s_context.conn = item.conn; s_context.generation = item.generation;
            if (item.channel == AIR_SESSION) {
                air_config_session_init(&s_session,NULL,item.generation);
                refresh_uart();
            } else if (item.channel == AIR_RAW_ON) {
                /* Do not flush here: the first ROM ACK must remain readable. */
                if (uart_disable_pattern_det_intr(at_uart_port_get()) != ESP_OK)
                    fail_session(item.generation);
                air_config_set_notify_armed(&s_session,true);
            } else if (item.channel == AIR_RAW_OFF) {
                air_config_set_notify_armed(&s_session,false);
            } else if (item.channel == AIR_BLE_CONFIG_UUID16) {
                refresh_uart();
                int gpio; bool high, fault; unsigned count;
                esp_at_link_gpio_get_state(&gpio,&high,&count,&fault);
                air_config_set_link_state(&s_session,gpio,high,count,fault);
                air_config_feed(&s_session,item.data,item.length,
                    xTaskGetTickCount()*portTICK_PERIOD_MS,&callbacks);
            } else if (item.channel == AIR_BLE_WRITE_UUID16 && s_session.notify_armed) {
                if (uart_write_bytes(at_uart_port_get(),item.data,item.length) != item.length ||
                    uart_wait_tx_done(at_uart_port_get(),pdMS_TO_TICKS(1000)) != ESP_OK)
                    fail_session(item.generation);
            } else fail_session(item.generation);
        }
        if (valid(s_context.conn,s_context.generation))
            air_config_flush_idle(&s_session,xTaskGetTickCount()*portTICK_PERIOD_MS,&callbacks);
        if (s_reset_at && (int32_t)(xTaskGetTickCount()-s_reset_at) >= 0) esp_at_restart();
    }
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    uint16_t channel = (uintptr_t)arg;
    size_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !length ||
        length > (channel == AIR_BLE_CONFIG_UUID16 ? AIR_CONFIG_MAX_COMMAND : AIR_MAX_DATA))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint32_t generation;
    if (!claim(conn,&generation)) return BLE_ATT_ERR_INSUFFICIENT_RES;
    uint8_t data[AIR_MAX_DATA];
    if (os_mbuf_copydata(ctxt->om,0,length,data) != 0 ||
        !enqueue(conn,generation,channel,data,length)) return BLE_ATT_ERR_INSUFFICIENT_RES;
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=BLE_UUID16_DECLARE(AIR_BLE_SERVICE_UUID16),
     .characteristics=(struct ble_gatt_chr_def[]) {
        {.uuid=BLE_UUID16_DECLARE(AIR_BLE_NOTIFY_UUID16),.access_cb=access_cb,
         .flags=BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_raw_handle},
        {.uuid=BLE_UUID16_DECLARE(AIR_BLE_WRITE_UUID16),.access_cb=access_cb,
         .arg=(void *)(uintptr_t)AIR_BLE_WRITE_UUID16,
         .flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid=BLE_UUID16_DECLARE(AIR_BLE_CONFIG_UUID16),.access_cb=access_cb,
         .arg=(void *)(uintptr_t)AIR_BLE_CONFIG_UUID16,.val_handle=&s_config_handle,
         .flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP|BLE_GATT_CHR_F_NOTIFY},
        {0}}}, {0}};

bool air_ble_service_register(void)
{
    if (!s_input) s_input=xQueueCreate(16,sizeof(air_message_t));
    if (!s_output) s_output=xQueueCreate(32,sizeof(air_message_t));
    if (!s_input || !s_output) return false;
    if (!s_worker && xTaskCreate(worker,"air-uart",6144,NULL,5,&s_worker) != pdPASS) return false;
    if (ble_gatts_count_cfg(s_services) || ble_gatts_add_svcs(s_services)) return false;
    xQueueReset(s_input); xQueueReset(s_output);
    ble_npl_event_init(&s_output_event,host_output,NULL);
    if (ble_gap_event_listener_register(&s_listener,gap_event,NULL)) return false;
    portENTER_CRITICAL(&s_lock);
    s_conn=BLE_HS_CONN_HANDLE_NONE; ++s_generation; s_raw=false;
    s_configured=false; s_adv_pending=true; s_registered=true; s_fault=false; s_failed=false;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

void air_ble_service_unregister(void)
{
    portENTER_CRITICAL(&s_lock);
    s_registered=false; s_raw=false; ++s_generation; s_conn=BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&s_lock);
    (void)ble_gap_event_listener_unregister(&s_listener);
    /* Fixed queues/worker are reused across reinit; no per-reinit allocation. */
}

bool air_ble_service_uart_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active=s_registered && s_conn!=BLE_HS_CONN_HANDLE_NONE && s_raw;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

bool air_ble_service_uart_route(const uint8_t *data, size_t length)
{
    portENTER_CRITICAL(&s_lock);
    uint16_t conn=s_conn; uint32_t generation=s_generation; bool active=s_raw && s_registered;
    portEXIT_CRITICAL(&s_lock);
    return active && output(conn,generation,s_raw_handle,data,length);
}

void air_ble_service_uart_fault(void)
{
    portENTER_CRITICAL(&s_lock); uint32_t generation=s_generation; portEXIT_CRITICAL(&s_lock);
    fail_session(generation);
}

static void manager(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    /* Use the ESP-AT GAP callback, retaining legacy connection/SPP bookkeeping. */
    (void)esp_at_exe_cmd("AT+BLEINIT=2\r\n","\r\nOK\r\n",5000);
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        bool registered=s_registered, configured=s_configured, pending=s_adv_pending;
        uint32_t generation=s_generation;
        portEXIT_CRITICAL(&s_lock);
        if (registered && ble_hs_synced()) {
            if (!configured) {
                bool ok=esp_at_exe_cmd("AT+BLEADVPARAM=50,50,0,0,7,0,,\r\n","\r\nOK\r\n",2000)==ESP_OK &&
                    esp_at_exe_cmd("AT+BLEADVDATA=\"0201060C0953656973546563682D43360303F0FF\"\r\n","\r\nOK\r\n",2000)==ESP_OK;
                portENTER_CRITICAL(&s_lock);
                if (s_registered && generation==s_generation) s_configured=ok;
                portEXIT_CRITICAL(&s_lock);
            } else if (pending) {
                bool ok=esp_at_exe_cmd("AT+BLEADVSTART\r\n","\r\nOK\r\n",2000)==ESP_OK;
                portENTER_CRITICAL(&s_lock);
                if (ok && s_registered && generation==s_generation) s_adv_pending=false;
                portEXIT_CRITICAL(&s_lock);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void air_ble_service_autostart(void)
{
    if (!s_manager) (void)xTaskCreate(manager,"air-start",4096,NULL,4,&s_manager);
}
#else
bool air_ble_service_register(void){return true;}
void air_ble_service_unregister(void){}
void air_ble_service_autostart(void){}
bool air_ble_service_uart_active(void){return false;}
bool air_ble_service_uart_route(const uint8_t *data,size_t length){(void)data;(void)length;return false;}
void air_ble_service_uart_fault(void){}
#endif
