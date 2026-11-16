/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AIR_CONFIG_PARSER_H
#define AIR_CONFIG_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIR_CONFIG_MAX_COMMAND 96
#define AIR_CONFIG_IDLE_DELIMITER_MS 100
#define AIR_CONFIG_AUTH_DEFAULT "123456"
#define AIR_UART_BAUD_MIN 80U
#define AIR_UART_BAUD_MAX 5000000U

typedef struct {
    uint32_t baudrate;
    uint8_t parity; /* 0 = none, 1 = even (BT52 mapping). */
    uint8_t databits; /* Always 3 on the wire (8 data bits). */
} air_uart_config_t;

typedef struct {
    uint32_t generation;
    bool authenticated;
    bool raw_mode;
    bool notify_armed;
    bool log_suppressed;
    air_uart_config_t uart;
    int boot_gpio, reset_gpio;
    bool bootrst_enabled;
    int link_gpio;
    bool link_high;
    unsigned link_count;
    bool link_fault;
    char pending[AIR_CONFIG_MAX_COMMAND + 1];
    size_t pending_len;
    uint32_t last_rx_ms;
    bool discarding;
} air_config_session_t;

typedef bool (*air_config_apply_fn)(const air_uart_config_t *config, void *arg);
typedef bool (*air_config_link_fn)(int gpio, bool save, void *arg);
typedef bool (*air_config_reset_fn)(void *arg);
typedef void (*air_config_reply_fn)(const char *response, size_t length, void *arg);

typedef struct {
    air_config_apply_fn apply_uart;
    air_config_link_fn apply_link;
    bool (*bootrst)(int boot,int reset,int enabled,int mode,void *arg);
    air_config_reset_fn request_reset;
    air_config_reply_fn reply;
    void *arg;
    const char *auth_password; /* NULL selects the factory default. */
} air_config_callbacks_t;

void air_config_session_init(air_config_session_t *session, const air_uart_config_t *initial,
                              uint32_t generation);
void air_config_session_disconnect(air_config_session_t *session, uint32_t next_generation);
bool air_config_generation_valid(const air_config_session_t *session, uint32_t generation);
void air_config_set_link_state(air_config_session_t *session, int gpio, bool high,
                               unsigned count, bool fault);
void air_config_set_notify_armed(air_config_session_t *session, bool armed);
bool air_config_feed(air_config_session_t *session, const uint8_t *data, size_t length,
                     uint32_t now_ms, const air_config_callbacks_t *callbacks);
bool air_config_flush_idle(air_config_session_t *session, uint32_t now_ms,
                           const air_config_callbacks_t *callbacks);

#endif
