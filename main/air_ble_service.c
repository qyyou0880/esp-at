/* SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_BT_NIMBLE_ENABLED)
#include "air_ble_service.h"
#include "air_config_parser.h"
#include "esp_at.h"
#include "at_uart.h"
#include "esp_at_link_gpio.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"
#include <string.h>

#define AIR_EVENT 0xffffU
#define AIR_NOTIFY_OFF 0xfffeU
#define AIR_RAW_MAX 512U
typedef struct {
    uint16_t conn_handle;
    uint32_t generation;
    uint16_t channel;
    uint16_t length;
    uint8_t data[AIR_RAW_MAX];
} air_ble_work_t;

static const char *TAG = "air-ble";
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static air_config_session_t s_session;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t s_generation;
static volatile bool s_adv_pending;
static volatile bool s_reset_pending;
static uint16_t s_config_val_handle;
static uint16_t s_notify_val_handle;
static struct ble_gap_event_listener s_listener;
static bool s_registered;

static void enqueue_event(uint16_t conn, uint32_t generation, uint16_t event)
{
    air_ble_work_t item = {.conn_handle = conn, .generation = generation, .channel = event};
    if (!s_queue || xQueueSend(s_queue, &item, 0) != pdTRUE)
        ESP_LOGW(TAG, "BLE session event queue full");
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
        uint32_t generation = ++s_generation;
        s_conn = event->connect.conn_handle;
        enqueue_event(s_conn, generation, AIR_EVENT);
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT &&
               event->disconnect.conn.conn_handle == s_conn) {
        uint32_t generation = ++s_generation;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_adv_pending = true;
        enqueue_event(event->disconnect.conn.conn_handle, generation, AIR_EVENT);
    } else if (event->type == BLE_GAP_EVENT_SUBSCRIBE &&
               event->subscribe.conn_handle == s_conn &&
               event->subscribe.attr_handle == s_notify_val_handle) {
        enqueue_event(event->subscribe.conn_handle, s_generation,
                      event->subscribe.cur_notify ? AIR_BLE_NOTIFY_UUID16 : AIR_NOTIFY_OFF);
    }
    return 0;
}

static void air_reply(const char *response, size_t length, void *arg)
{
    (void)arg;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(response, length);
    if (!om) { (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM); return; }
    if (ble_gatts_notify_custom(conn, s_config_val_handle, om) != 0)
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

static bool apply_uart(const air_uart_config_t *config, void *arg)
{
    (void)arg;
    uart_config_t previous, next;
    at_uart_config_init(&previous);
    (void)at_nvs_uart_config_get(&previous);
    uint32_t actual_baud = 0;
    if (uart_get_baudrate(at_uart_port_get(), &actual_baud) == ESP_OK) previous.baud_rate = actual_baud;
    next = previous;
    next.baud_rate = config->baudrate;
    next.data_bits = UART_DATA_8_BITS;
    next.parity = config->parity ? UART_PARITY_EVEN : UART_PARITY_DISABLE;
    if (uart_param_config(at_uart_port_get(), &next) != ESP_OK) return false;
    if (!at_nvs_uart_config_set_checked(&next)) {
        (void)uart_param_config(at_uart_port_get(), &previous);
        (void)at_nvs_uart_config_set_checked(&previous);
        return false;
    }
    return true;
}

static bool apply_link(int gpio, bool save, void *arg)
{
    (void)arg;
    return esp_at_link_gpio_configure(gpio, save);
}

static bool request_reset(void *arg)
{
    (void)arg;
    s_reset_pending = true;
    return true;
}

static void start_advertising(void)
{
    static const char name[] = "SeisTech-C6";
    static ble_uuid16_t uuid = {.u = {.type = BLE_UUID_TYPE_16}, .value = AIR_BLE_SERVICE_UUID16};
    struct ble_hs_adv_fields fields = {0};
    fields.name = (uint8_t *)name;
    fields.name_len = sizeof(name) - 1;
    fields.name_is_complete = 1;
    fields.uuids16 = &uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_set_fields(&fields) == 0 &&
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, NULL, NULL) == 0)
        s_adv_pending = false;
}

static void worker(void *arg)
{
    (void)arg;
    const air_config_callbacks_t callbacks = {
        .apply_uart = apply_uart, .apply_link = apply_link, .request_reset = request_reset,
        .reply = air_reply, .arg = NULL, .auth_password = AIR_CONFIG_AUTH_DEFAULT,
    };
    start_advertising();
    for (;;) {
        air_ble_work_t item;
        if (s_adv_pending && s_conn == BLE_HS_CONN_HANDLE_NONE) start_advertising();
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(AIR_CONFIG_IDLE_DELIMITER_MS)) == pdTRUE) {
            if (item.channel == AIR_EVENT) {
                if (item.generation == s_generation)
                    air_config_session_disconnect(&s_session, item.generation);
                continue;
            }
            if (item.channel == AIR_NOTIFY_OFF) {
                if (item.generation == s_generation) air_config_set_notify_armed(&s_session, false);
                continue;
            }
            if (!air_config_generation_valid(&s_session, item.generation) ||
                item.conn_handle != s_conn) continue;
            if (item.channel == AIR_BLE_NOTIFY_UUID16) {
                air_config_set_notify_armed(&s_session, true);
            } else if (item.channel == AIR_BLE_CONFIG_UUID16) {
                (void)air_config_feed(&s_session, item.data, item.length,
                                      xTaskGetTickCount() * portTICK_PERIOD_MS, &callbacks);
            } else if (item.channel == AIR_BLE_WRITE_UUID16 && s_session.raw_mode &&
                       s_session.notify_armed) {
                if (uart_write_bytes(at_uart_port_get(), (const char *)item.data, item.length) != item.length)
                    (void)ble_gap_terminate(item.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            (void)air_config_flush_idle(&s_session, xTaskGetTickCount() * portTICK_PERIOD_MS, &callbacks);
        }
        if (s_reset_pending) {
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_at_restart();
            s_reset_pending = false;
        }
    }
}

static int access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    uint16_t channel = (uintptr_t)arg;
    size_t length = OS_MBUF_PKTLEN(ctxt->om);
    size_t limit = channel == AIR_BLE_CONFIG_UUID16 ? AIR_CONFIG_MAX_COMMAND : AIR_RAW_MAX;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !s_queue || !length || length > limit)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    air_ble_work_t item = {.conn_handle = conn_handle, .generation = s_generation,
                           .channel = channel, .length = length};
    if (os_mbuf_copydata(ctxt->om, 0, length, item.data) != 0 ||
        xQueueSend(s_queue, &item, 0) != pdTRUE)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(AIR_BLE_SERVICE_UUID16),
     .characteristics = (struct ble_gatt_chr_def[]) {
        {.uuid = BLE_UUID16_DECLARE(AIR_BLE_NOTIFY_UUID16), .access_cb = access_cb,
         .arg = (void *)(uintptr_t)AIR_BLE_NOTIFY_UUID16, .flags = BLE_GATT_CHR_F_NOTIFY,
         .val_handle = &s_notify_val_handle},
        {.uuid = BLE_UUID16_DECLARE(AIR_BLE_WRITE_UUID16), .access_cb = access_cb,
         .arg = (void *)(uintptr_t)AIR_BLE_WRITE_UUID16,
         .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid = BLE_UUID16_DECLARE(AIR_BLE_CONFIG_UUID16), .access_cb = access_cb,
         .arg = (void *)(uintptr_t)AIR_BLE_CONFIG_UUID16,
         .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
         .val_handle = &s_config_val_handle},
        {0}
    }},
    {0}
};

bool air_ble_service_register(void)
{
    if (s_registered) return true;
    if (ble_gatts_count_cfg(s_services) != 0 || ble_gatts_add_svcs(s_services) != 0) return false;
    s_queue = xQueueCreate(16, sizeof(air_ble_work_t));
    if (!s_queue || xTaskCreate(worker, "air-ble", 6144, NULL, 5, &s_worker) != pdPASS) return false;
    air_uart_config_t initial = {115200, 0, 3};
    uart_config_t actual;
    at_uart_config_init(&actual);
    if (at_nvs_uart_config_get(&actual)) {
        initial.baudrate = actual.baud_rate;
        initial.parity = actual.parity == UART_PARITY_EVEN;
        initial.databits = actual.data_bits == UART_DATA_8_BITS ? 3 : actual.data_bits;
    }
    uint32_t actual_baud = 0;
    if (uart_get_baudrate(at_uart_port_get(), &actual_baud) == ESP_OK) initial.baudrate = actual_baud;
    air_config_session_init(&s_session, &initial, ++s_generation);
    if (ble_gap_event_listener_register(&s_listener, gap_event, NULL) != 0) return false;
    s_registered = true;
    return true;
}

void air_ble_service_unregister(void)
{
    if (!s_registered) return;
    (void)ble_gap_event_listener_unregister(&s_listener);
    s_registered = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    ++s_generation;
    if (s_worker) { vTaskDelete(s_worker); s_worker = NULL; }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
}

bool air_ble_service_uart_active(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_session.raw_mode && s_session.notify_armed;
}

bool air_ble_service_uart_route(const uint8_t *data, size_t length)
{
    uint16_t conn = s_conn;
    if (!air_ble_service_uart_active() || !data || !length) return false;
    size_t chunk_size = ble_att_mtu(conn);
    chunk_size = chunk_size > 3 ? chunk_size - 3 : 20;
    while (length) {
        size_t chunk = length < chunk_size ? length : chunk_size;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, chunk);
        if (!om || ble_gatts_notify_custom(conn, s_notify_val_handle, om) != 0) {
            (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return false;
        }
        data += chunk;
        length -= chunk;
    }
    return true;
}

#if defined(CONFIG_AT_SELF_COMMAND_SUPPORT)
static void autostart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(100));
    (void)esp_at_exe_cmd("AT+BLEINIT=2\r\n", "OK", 3000);
    vTaskDelete(NULL);
}
#endif

void air_ble_service_autostart(void)
{
#if defined(CONFIG_AT_SELF_COMMAND_SUPPORT)
    (void)xTaskCreate(autostart_task, "air-ble-start", 4096, NULL, 4, NULL);
#endif
}

#else
#include "air_ble_service.h"
bool air_ble_service_register(void) { return true; }
void air_ble_service_unregister(void) {}
void air_ble_service_autostart(void) {}
bool air_ble_service_uart_active(void) { return false; }
bool air_ble_service_uart_route(const uint8_t *data, size_t length) { (void)data; (void)length; return false; }
#endif
