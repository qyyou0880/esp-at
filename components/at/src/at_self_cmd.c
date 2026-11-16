/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_at_core.h"
#include "esp_at.h"
#include "esp_at_interface.h"

#ifdef CONFIG_AT_SELF_COMMAND_SUPPORT
#define AT_CMD_RESP_BIT                 BIT(0)

typedef struct {
    EventGroupHandle_t status_bits;     /*!< status bits for self command event */
    char *cmd;         /*!< command string from self command */
    int32_t data_len;  /*!< available data length of self command */
    char *resp;        /*!< expected response from self command */
    bool mode;         /*!< self command mode */
    uint8_t match[64];
    size_t match_len;
} at_self_cmd_t;

at_self_cmd_t *s_self_cmd = NULL;
static const char *TAG = "at-self-cmd";
static StaticSemaphore_t s_call_lock_storage;
static StaticSemaphore_t s_state_lock_storage;
static SemaphoreHandle_t s_call_lock;
static SemaphoreHandle_t s_state_lock;

static void at_self_cmd_locks_init(void)
{
    if (!s_call_lock) s_call_lock = xSemaphoreCreateMutexStatic(&s_call_lock_storage);
    if (!s_state_lock) s_state_lock = xSemaphoreCreateMutexStatic(&s_state_lock_storage);
}

bool at_self_cmd_get_mode(void)
{
    if (!s_self_cmd) {
        return false;
    }
    return s_self_cmd->mode;
}

static void at_self_cmd_set_mode(bool mode)
{
    s_self_cmd->mode = mode;
}

int32_t at_self_cmd_read_data(uint8_t *buffer, int32_t buffer_len)
{
    int32_t len = s_self_cmd->data_len < buffer_len ? s_self_cmd->data_len : buffer_len;
    memcpy(buffer, s_self_cmd->cmd + (strlen(s_self_cmd->cmd) - s_self_cmd->data_len), len);
    s_self_cmd->data_len -= len;
    return len;
}

int32_t at_self_cmd_write_data(uint8_t *data, int32_t len)
{
    /* Internal boot commands must never leak responses onto the host UART. */
    int32_t ret = len;
    at_self_cmd_locks_init();
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (!s_self_cmd) { xSemaphoreGive(s_state_lock); return ret; }
    size_t expected_len = strlen(s_self_cmd->resp);
    bool found = false;
    if (expected_len < sizeof(s_self_cmd->match)) {
        size_t total = s_self_cmd->match_len + (size_t)len;
        if (total <= sizeof(s_self_cmd->match)) {
            memcpy(s_self_cmd->match + s_self_cmd->match_len, data, (size_t)len);
            for (size_t i = 0; i + expected_len <= total; ++i) {
                if (memcmp(s_self_cmd->match + i, s_self_cmd->resp, expected_len) == 0) {
                    found = true;
                    break;
                }
            }
            size_t keep = expected_len > 1 ? expected_len - 1 : 0;
            if (keep > total) keep = total;
            memmove(s_self_cmd->match, s_self_cmd->match + total - keep, keep);
            s_self_cmd->match_len = keep;
        } else {
            if (expected_len <= (size_t)len) {
                for (size_t i = 0; i + expected_len <= (size_t)len; ++i) {
                    if (memcmp(data + i, s_self_cmd->resp, expected_len) == 0) {
                        found = true;
                        break;
                    }
                }
            }
            s_self_cmd->match_len = 0;
        }
    }
    if (found) {
        xEventGroupSetBits(s_self_cmd->status_bits, AT_CMD_RESP_BIT);
    }
    xSemaphoreGive(s_state_lock);
    return ret;
}

int32_t at_self_cmd_get_data_len(void)
{
    return s_self_cmd->data_len;
}

static void at_self_cmd_cleanup(void)
{
    at_self_cmd_locks_init();
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_self_cmd) {
        if (s_self_cmd->cmd) {
            free(s_self_cmd->cmd);
            s_self_cmd->cmd = NULL;
        }
        if (s_self_cmd->resp) {
            free(s_self_cmd->resp);
            s_self_cmd->resp = NULL;
        }
        if (s_self_cmd->status_bits) {
            vEventGroupDelete(s_self_cmd->status_bits);
            s_self_cmd->status_bits = NULL;
        }
        free(s_self_cmd);
        s_self_cmd = NULL;
    }
    xSemaphoreGive(s_state_lock);
}

esp_err_t esp_at_exe_cmd(const char *cmd, const char *expected_response, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;

    at_self_cmd_locks_init();
    xSemaphoreTake(s_call_lock, portMAX_DELAY);

    // init
    s_self_cmd = (at_self_cmd_t *)malloc(sizeof(at_self_cmd_t));
    if (!s_self_cmd) {
        xSemaphoreGive(s_call_lock);
        return ESP_ERR_NO_MEM;
    }

    s_self_cmd->status_bits = xEventGroupCreate();
    s_self_cmd->cmd = strdup(cmd);
    s_self_cmd->data_len = strlen(cmd);
    s_self_cmd->resp = strdup(expected_response);
    if (!s_self_cmd->status_bits || !s_self_cmd->cmd || !s_self_cmd->resp) {
        at_self_cmd_cleanup();
        xSemaphoreGive(s_call_lock);
        return ESP_ERR_NO_MEM;
    }
    at_self_cmd_set_mode(true);

    // command notify
    esp_at_port_recv_data_notify(strlen(cmd), portMAX_DELAY);

    // wait for response
    EventBits_t uxBits = xEventGroupWaitBits(s_self_cmd->status_bits, AT_CMD_RESP_BIT, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    if (!(uxBits & AT_CMD_RESP_BIT)) {
        ESP_LOGE(TAG, "<%.*s> cannot get expected response <%s> within %ums", strlen(cmd) - 2, cmd, expected_response, timeout_ms);
        ret = ESP_ERR_TIMEOUT;
    }

    // deinit
    at_self_cmd_set_mode(false);
    at_self_cmd_cleanup();
    xSemaphoreGive(s_call_lock);
    return ret;
}
#endif
