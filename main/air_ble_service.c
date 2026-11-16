/* SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_BT_NIMBLE_ENABLED)
#include "air_ble_service.h"
#include "air_config_parser.h"
#include "esp_at.h"
#include "at_uart.h"
#include "esp_at_link_gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"
#include <string.h>

typedef struct {
    uint16_t conn_handle;
    uint32_t generation;
    uint16_t channel;
    uint8_t data[96];
    uint16_t length;
} air_ble_work_t;

static const char *TAG = "air-ble";
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static air_config_session_t s_session;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_generation;
static uint16_t s_config_val_handle;
static uint16_t s_notify_val_handle;
static struct ble_gap_event_listener s_listener;

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
        s_conn = event->connect.conn_handle;
        air_config_session_disconnect(&s_session, ++s_generation);
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT && event->disconnect.conn.conn_handle == s_conn) {
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        air_config_session_disconnect(&s_session, ++s_generation);
    } else if (event->type == BLE_GAP_EVENT_SUBSCRIBE && event->subscribe.conn_handle == s_conn &&
               event->subscribe.attr_handle == s_notify_val_handle) {
        air_config_set_notify_armed(&s_session, event->subscribe.cur_notify != 0);
    }
    return 0;
}

static void air_reply(const char *response, size_t length, void *arg)
{
    (void)arg;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(response, length);
    if (!om || ble_gatts_notify_custom(s_conn, s_config_val_handle, om) != 0) {
        if (om) os_mbuf_free_chain(om);
        ESP_LOGW(TAG, "configuration notification overflow; session dropped");
        (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static bool apply_uart(const air_uart_config_t *config, void *arg)
{
    (void)arg;
    uart_config_t next;
    at_uart_config_init(&next);
    next.baud_rate = config->baudrate;
    next.data_bits = UART_DATA_8_BITS;
    next.parity = config->parity ? UART_PARITY_EVEN : UART_PARITY_DISABLE;
    if (uart_param_config(at_uart_port_get(), &next) != ESP_OK) return false;
    at_nvs_uart_config_set(&next);
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
    esp_at_restart();
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    air_ble_work_t item;
    struct ble_hs_adv_fields fields = {0};
    fields.name = (uint8_t *)"SeisTech-C6";
    fields.name_len = strlen((char *)fields.name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]){{.u = {.type = BLE_UUID_TYPE_16}, .value = AIR_BLE_SERVICE_UUID16}};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    while (ble_gap_adv_set_fields(&fields) != 0) vTaskDelay(pdMS_TO_TICKS(100));
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    while (ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL) != 0)
        vTaskDelay(pdMS_TO_TICKS(100));
    const air_config_callbacks_t callbacks = {
        .apply_uart = apply_uart, .apply_link = apply_link, .request_reset = request_reset,
        .reply = air_reply, .arg = NULL, .auth_password = AIR_CONFIG_AUTH_DEFAULT,
    };
    for (;;) {
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(AIR_CONFIG_IDLE_DELIMITER_MS)) == pdTRUE) {
            if (!air_config_generation_valid(&s_session, item.generation) || item.conn_handle != s_conn) continue;
            if (item.channel == AIR_BLE_CONFIG_UUID16) {
                (void)air_config_feed(&s_session, item.data, item.length, xTaskGetTickCount() * portTICK_PERIOD_MS, &callbacks);
            } else if (item.channel == AIR_BLE_WRITE_UUID16 && s_session.raw_mode) {
                /* The single UART reader remains at_uart_task; this worker only writes TX. */
                esp_at_port_write_data(item.data, item.length);
            }
        } else if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            (void)air_config_flush_idle(&s_session, xTaskGetTickCount() * portTICK_PERIOD_MS, &callbacks);
        }
    }
}

static int access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; uint16_t channel = (uintptr_t)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !s_queue || ctxt->om->om_len > sizeof(((air_ble_work_t *)0)->data))
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    air_ble_work_t item = {.conn_handle = conn_handle, .generation = s_generation, .channel = channel,
                           .length = ctxt->om->om_len};
    if (os_mbuf_copydata(ctxt->om, 0, item.length, item.data) != 0 || xQueueSend(s_queue, &item, 0) != pdTRUE)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    if (channel == AIR_BLE_NOTIFY_UUID16) air_config_set_notify_armed(&s_session, true);
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(AIR_BLE_SERVICE_UUID16), .characteristics = (struct ble_gatt_chr_def[]) {
        {.uuid = BLE_UUID16_DECLARE(AIR_BLE_NOTIFY_UUID16), .access_cb = access_cb, .arg = (void *)(uintptr_t)AIR_BLE_NOTIFY_UUID16, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_notify_val_handle},
        {.uuid = BLE_UUID16_DECLARE(AIR_BLE_WRITE_UUID16), .access_cb = access_cb, .arg = (void *)(uintptr_t)AIR_BLE_WRITE_UUID16, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid = BLE_UUID16_DECLARE(AIR_BLE_CONFIG_UUID16), .access_cb = access_cb, .arg = (void *)(uintptr_t)AIR_BLE_CONFIG_UUID16, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_config_val_handle},
        {0}
    }},
    {0}
};

bool air_ble_service_register(void)
{
    if (ble_gatts_count_cfg(s_services) != 0 || ble_gatts_add_svcs(s_services) != 0) return false;
    s_queue = xQueueCreate(8, sizeof(air_ble_work_t));
    if (!s_queue || xTaskCreate(worker, "air-ble", 4096, NULL, 5, &s_worker) != pdPASS) return false;
    air_uart_config_t initial = {115200, 0, 3};
    air_config_session_init(&s_session, &initial, ++s_generation);
    if (ble_gap_event_listener_register(&s_listener, gap_event, NULL) != 0) return false;
    return true;
}
bool air_ble_service_uart_active(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_session.raw_mode && s_session.notify_armed;
}

bool air_ble_service_uart_route(const uint8_t *data, size_t length)
{
    if (!air_ble_service_uart_active()) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (!om || ble_gatts_notify_custom(s_conn, s_notify_val_handle, om) != 0) {
        if (om) os_mbuf_free_chain(om);
        (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return true;
}
#else
#include "air_ble_service.h"
bool air_ble_service_register(void) { return true; }
bool air_ble_service_uart_active(void) { return false; }
bool air_ble_service_uart_route(const uint8_t *data, size_t length) { (void)data; (void)length; return false; }
#endif
