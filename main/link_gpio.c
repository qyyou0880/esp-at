/* SPDX-License-Identifier: Apache-2.0 */
#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_BT_NIMBLE_ENABLED)
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_at.h"
#include "esp_at_link_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "soc/rtc.h"
#include "link_gpio_state.h"
#include "air_ble_service.h"
#ifdef CONFIG_AT_BASE_ON_UART
#include "at_uart.h"
extern at_uart_port_pins_t g_uart_port_pin;
#endif

#define LINK_GPIO_NS "LINKGPIO"
#define LINK_GPIO_KEY "gpio"

static const char *TAG = "link-gpio";
static link_gpio_state_t s_state = {.gpio = -1};
static struct ble_gap_event_listener s_listener;
static SemaphoreHandle_t s_lock;
static uint64_t s_owned_pins;
static bool s_listener_registered;
static bool s_powerdown;

static bool link_gpio_drive(int gpio, bool high, void *arg)
{
    (void)arg;
    return gpio_set_level((gpio_num_t)gpio, high ? 1 : 0) == ESP_OK;
}

static bool link_gpio_pin_allowed(int gpio)
{
    /* GPIO0/1 are reserved for the external RTC clock; 4/5/8/9/15 strap;
     * 6/7 default AT UART; 12/13 USB; 16/17 console; >=24 flash/SiP. */
    if (gpio < 2 || gpio > 23 || !GPIO_IS_VALID_OUTPUT_GPIO(gpio) ||
        (gpio >= 4 && gpio <= 9) || gpio == 12 || gpio == 13 ||
        (gpio >= 15 && gpio <= 17)) {
        return false;
    }
#ifdef CONFIG_AT_BASE_ON_UART
    if (gpio == g_uart_port_pin.tx_pin || gpio == g_uart_port_pin.rx_pin ||
        gpio == g_uart_port_pin.cts_pin || gpio == g_uart_port_pin.rts_pin) {
        return false;
    }
#endif
#ifdef CONFIG_ESP_CONSOLE_UART_CUSTOM
    if (gpio == CONFIG_ESP_CONSOLE_UART_TX_GPIO || gpio == CONFIG_ESP_CONSOLE_UART_RX_GPIO) {
        return false;
    }
#endif
    uint64_t mask = 1ULL << gpio;
    return (s_owned_pins & mask) || !esp_gpio_is_reserved(mask);
}

static bool link_gpio_prepare_pin(int gpio, bool high)
{
    if (gpio < 0) {
        return true;
    }
    if (!link_gpio_pin_allowed(gpio)) {
        return false;
    }
    uint64_t mask = 1ULL << gpio;
    if (s_owned_pins & mask) {
        return true; /* Already output; never reset an active pin's direction. */
    }
    /* Load the output latch before enabling the output, avoiding a stale high. */
    if (gpio_set_level(gpio, high ? 1 : 0) != ESP_OK) {
        return false;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        (void)gpio_set_level(gpio, 0);
        return false;
    }
    s_owned_pins |= mask; /* gpio_config reserves the output in pinned IDF 6.1. */
    if (gpio_sleep_sel_dis(gpio) != ESP_OK) {
        (void)gpio_set_level(gpio, 0);
        return false;
    }
    return true;
}

static bool link_gpio_load(int *gpio)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LINK_GPIO_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *gpio = -1;
        return true;
    }
    if (err != ESP_OK) return false;
    int32_t value = -1;
    err = nvs_get_i32(handle, LINK_GPIO_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *gpio = -1;
        return true;
    }
    if (err != ESP_OK || (value != -1 && !link_gpio_pin_allowed(value))) return false;
    *gpio = value;
    return true;
}

static bool link_gpio_save_cb(int gpio, void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open(LINK_GPIO_NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_i32(handle, LINK_GPIO_KEY, gpio);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void esp_at_link_gpio_init(void)
{
    if (s_lock) return;
    link_gpio_state_init(&s_state, link_gpio_drive, NULL);
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        s_state.fault = true;
        ESP_LOGE(TAG, "mutex allocation failed; LINK disabled");
        return;
    }
    int configured = -1;
    if (!link_gpio_load(&configured) || !link_gpio_prepare_pin(configured, false) ||
        !link_gpio_state_configure(&s_state, configured)) {
        (void)link_gpio_state_configure(&s_state, -1);
        s_state.fault = true;
        ESP_LOGE(TAG, "saved configuration invalid/unavailable; LINK disabled");
    }
}

static uint8_t link_gpio_query(uint8_t *cmd_name)
{
    if (!s_lock) return ESP_AT_RESULT_CODE_ERROR;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int pin = s_state.gpio;
    unsigned level = s_state.link_high, count = s_state.active_count, fault = s_state.fault;
    xSemaphoreGive(s_lock);
    uint8_t buffer[ESP_AT_BUF_ON_STACK_SIZE];
    snprintf((char *)buffer, sizeof(buffer), "%s:%d,%u,%u,%u\r\n", cmd_name, pin, level, count, fault);
    esp_at_port_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t link_gpio_test(uint8_t *cmd_name)
{
    uint8_t buffer[ESP_AT_BUF_ON_STACK_SIZE];
    snprintf((char *)buffer, sizeof(buffer), "%s:<gpio=-1|allowed GPIO>[,<save=0|1>]\r\n", cmd_name);
    esp_at_port_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t link_gpio_setup(uint8_t para_num)
{
    int32_t gpio, save = 0;
    if (!s_lock || (para_num != 1 && para_num != 2) ||
        esp_at_get_para_as_digit(0, &gpio) != ESP_AT_PARA_PARSE_RESULT_OK ||
        (para_num == 2 && esp_at_get_para_as_digit(1, &save) != ESP_AT_PARA_PARSE_RESULT_OK) ||
        save < 0 || save > 1) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = !s_powerdown && (gpio == -1 || link_gpio_pin_allowed(gpio));
    if (ok) ok = link_gpio_prepare_pin(gpio, s_state.active_count != 0);
    if (ok) ok = link_gpio_state_configure_persist(&s_state, gpio, save ? link_gpio_save_cb : NULL, NULL);
    if (!ok && gpio >= 0 && gpio <= 30 && gpio != s_state.gpio && (s_owned_pins & (1ULL << gpio))) {
        (void)gpio_set_level(gpio, 0);
    }
    xSemaphoreGive(s_lock);
    return ok ? ESP_AT_RESULT_CODE_OK : ESP_AT_RESULT_CODE_ERROR;
}

bool esp_at_link_gpio_configure(int gpio, bool save)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = !s_powerdown && (gpio == -1 || link_gpio_pin_allowed(gpio));
    if (ok) ok = link_gpio_prepare_pin(gpio, s_state.active_count != 0);
    if (ok) ok = link_gpio_state_configure_persist(&s_state, gpio, save ? link_gpio_save_cb : NULL, NULL);
    xSemaphoreGive(s_lock);
    return ok;
}

static uint8_t rtc_clock_query(uint8_t *cmd_name)
{
    soc_rtc_slow_clk_src_t source = rtc_clk_slow_src_get();
    const char *name = "UNKNOWN";
    switch (source) {
    case SOC_RTC_SLOW_CLK_SRC_OSC_SLOW: name = "EXT_OSC"; break;
    case SOC_RTC_SLOW_CLK_SRC_XTAL32K: name = "EXT_CRYS"; break;
    case SOC_RTC_SLOW_CLK_SRC_RC_SLOW: name = "RC_SLOW"; break;
    case SOC_RTC_SLOW_CLK_SRC_RC32K: name = "RC32K"; break;
    default: break;
    }
    uint8_t buffer[ESP_AT_BUF_ON_STACK_SIZE];
    /* This is the selected runtime mux and its nominal frequency, not a frequency measurement. */
    snprintf((char *)buffer, sizeof(buffer), "%s:\"%s\",%lu\r\n", cmd_name, name,
             (unsigned long)rtc_clk_slow_freq_get_hz());
    esp_at_port_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static const esp_at_cmd_t s_cmd[] = {
    {"+LINKGPIO", link_gpio_test, link_gpio_query, link_gpio_setup, NULL},
    {"+RTCCLK", NULL, rtc_clock_query, NULL, NULL},
};

static int link_gpio_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_powerdown) {
        if (event->type == BLE_GAP_EVENT_CONNECT) {
            (void)link_gpio_state_connect(&s_state, event->connect.conn_handle, event->connect.status);
        } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
            (void)link_gpio_state_disconnect(&s_state, event->disconnect.conn.conn_handle);
        }
    }
    xSemaphoreGive(s_lock);
    return 0;
}

bool esp_at_link_gpio_cmd_register(void)
{
    return s_lock && esp_at_custom_cmd_array_register(s_cmd, sizeof(s_cmd) / sizeof(s_cmd[0]));
}
ESP_AT_CMD_SET_FIRST_INIT_FN(esp_at_link_gpio_cmd_register, 28);

esp_err_t __real_nimble_port_init(void);
esp_err_t __real_nimble_port_deinit(void);
esp_err_t __wrap_nimble_port_init(void)
{
    if (!s_lock) return ESP_ERR_NO_MEM;
    esp_err_t rc = __real_nimble_port_init();
    if (rc != ESP_OK) return rc;
    /* GAP's listener list has just been initialized; the host task is not running yet. */
    s_listener_registered = false;
    if (ble_gap_event_listener_register(&s_listener, link_gpio_gap_event, NULL) != 0) {
        (void)__real_nimble_port_deinit();
        ESP_LOGE(TAG, "GAP listener registration failed");
        return ESP_FAIL;
    }
    if (!air_ble_service_register()) {
        (void)ble_gap_event_listener_unregister(&s_listener);
        (void)__real_nimble_port_deinit();
        ESP_LOGE(TAG, "air BLE service registration failed");
        return ESP_FAIL;
    }
    s_listener_registered = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    link_gpio_state_reset(&s_state);
    s_powerdown = false;
    xSemaphoreGive(s_lock);
    return rc;
}

esp_err_t __wrap_nimble_port_deinit(void)
{
    esp_at_link_gpio_clear_for_powerdown();
    air_ble_service_unregister();
    /* Core stops the NimBLE host before deinit. Unregister before dynamic GAP storage is freed. */
    if (s_listener_registered) {
        (void)ble_gap_event_listener_unregister(&s_listener);
        s_listener_registered = false;
    }
    esp_err_t rc = __real_nimble_port_deinit();
    if (rc != ESP_OK && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.fault = true;
        xSemaphoreGive(s_lock);
    }
    return rc;
}

void esp_at_link_gpio_clear_for_powerdown(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_powerdown = true; /* Ignore a pending CONNECT racing a restart/deep sleep. */
    link_gpio_state_reset(&s_state);
    xSemaphoreGive(s_lock);
}
#endif
