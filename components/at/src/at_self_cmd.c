/* SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_at_core.h"
#include "esp_at.h"

#ifdef CONFIG_AT_SELF_COMMAND_SUPPORT
/* Persistent callback storage is deliberately never freed on a timeout. The AT
 * core can still finish a command after its caller has stopped waiting. A timed
 * out command quarantines this channel until its terminal response arrives. */
#define SELF_COMMAND_MAX 2048
#define SELF_RESPONSE_MAX 128
#define SELF_RESPONSE_BIT BIT(0)
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EventGroupHandle_t s_event;
static bool s_caller;
static struct {
    bool active;
    uint32_t generation, completed;
    esp_err_t result;
    size_t length, offset, expected_length, tail_length;
    char command[SELF_COMMAND_MAX + 1];
    char expected[SELF_RESPONSE_MAX + 1];
    uint8_t tail[SELF_RESPONSE_MAX];
} s;

bool at_self_cmd_get_mode(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s.active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

int32_t at_self_cmd_read_data(uint8_t *buffer, int32_t buffer_len)
{
    if (!buffer || buffer_len <= 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t length = s.active ? s.length - s.offset : 0;
    if (length > (size_t)buffer_len) length = buffer_len;
    memcpy(buffer, s.command + s.offset, length);
    s.offset += length;
    portEXIT_CRITICAL(&s_lock);
    return length;
}

int32_t at_self_cmd_get_data_len(void)
{
    portENTER_CRITICAL(&s_lock);
    int32_t length = s.active ? s.length - s.offset : 0;
    portEXIT_CRITICAL(&s_lock);
    return length;
}

int32_t at_self_cmd_write_data(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) return 0;
    bool completed = false;
    portENTER_CRITICAL(&s_lock);
    for (int32_t i = 0; s.active && i < len; ++i) {
        if (s.tail_length == sizeof(s.tail)) {
            memmove(s.tail, s.tail + 1, --s.tail_length);
        }
        s.tail[s.tail_length++] = data[i];
        bool match = s.tail_length >= s.expected_length &&
            memcmp(s.tail + s.tail_length - s.expected_length,
                   s.expected, s.expected_length) == 0;
        static const char error[] = "\r\nERROR\r\n";
        bool failed = s.tail_length >= sizeof(error) - 1 &&
            memcmp(s.tail + s.tail_length - (sizeof(error) - 1),
                   error, sizeof(error) - 1) == 0;
        if (match || failed) {
            s.result = match ? ESP_OK : ESP_FAIL;
            s.completed = s.generation;
            s.active = false;
            completed = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (completed && s_event) xEventGroupSetBits(s_event, SELF_RESPONSE_BIT);
    /* Internal replies are captured, including bytes after the matching token. */
    return len;
}

esp_err_t esp_at_exe_cmd(const char *command, const char *expected, uint32_t timeout_ms)
{
    if (!command || !expected) return ESP_ERR_INVALID_ARG;
    size_t length = strlen(command), expected_length = strlen(expected);
    if (!length || length > SELF_COMMAND_MAX || !expected_length ||
        expected_length > SELF_RESPONSE_MAX) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_caller || s.active) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_caller = true;
    portEXIT_CRITICAL(&s_lock);
    if (!s_event) s_event = xEventGroupCreate();
    if (!s_event) {
        portENTER_CRITICAL(&s_lock); s_caller = false; portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_event, SELF_RESPONSE_BIT);
    portENTER_CRITICAL(&s_lock);
    uint32_t generation = ++s.generation;
    if (!generation) generation = ++s.generation;
    s.completed = 0;
    memcpy(s.command, command, length + 1);
    memcpy(s.expected, expected, expected_length + 1);
    s.length = length;
    s.offset = 0;
    s.expected_length = expected_length;
    s.tail_length = 0;
    s.active = true;
    portEXIT_CRITICAL(&s_lock);
    esp_at_port_recv_data_notify(length, portMAX_DELAY);
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !timeout) timeout = 1;
    esp_err_t result;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        bool done = s.completed == generation;
        result = s.result;
        portEXIT_CRITICAL(&s_lock);
        if (done) break;
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) { result = ESP_ERR_TIMEOUT; break; }
        xEventGroupWaitBits(s_event, SELF_RESPONSE_BIT, pdTRUE, pdFALSE, timeout - elapsed);
    }
    portENTER_CRITICAL(&s_lock); s_caller = false; portEXIT_CRITICAL(&s_lock);
    /* Do not log command payloads: callers may use this API for credentials. */
    return result;
}
#endif
