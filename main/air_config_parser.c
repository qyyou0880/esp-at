/* SPDX-License-Identifier: Apache-2.0 */
#include "air_config_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void reply(const air_config_callbacks_t *cb, const char *text)
{
    if (cb && cb->reply) cb->reply(text, strlen(text), cb->arg);
}

static bool auth_value(const air_config_callbacks_t *cb, const char **value)
{
    *value = (cb && cb->auth_password) ? cb->auth_password : AIR_CONFIG_AUTH_DEFAULT;
    return strlen(*value) == 6;
}

static bool is_alnum6(const char *s)
{
    if (strlen(s) != 6) return false;
    for (size_t i = 0; i < 6; ++i) if (!isalnum((unsigned char)s[i])) return false;
    return true;
}

static bool parse_decimal(const char *s, int *out)
{
    if (!s || !*s) return false;
    int value = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || value > 1000000) return false;
        value = value * 10 + (*s - '0');
    }
    *out = value;
    return true;
}

static int baud_code(uint32_t baudrate)
{
    static const uint32_t rates[] = {2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200};
    for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); ++i)
        if (rates[i] == baudrate) return i;
    return -1;
}

static bool requires_auth(const char *cmd)
{
    return strncmp(cmd, "AT+VER?", 7) != 0 && strncmp(cmd, "AT+AUTH=", 8) != 0;
}

static bool execute(air_config_session_t *s, char *cmd, const air_config_callbacks_t *cb)
{
    char response[80];
    if (strcmp(cmd, "AT+VER?") == 0) {
        reply(cb, "+OK=SEISTECH-C6-AIR-1\r\n");
        return true;
    }
    if (strncmp(cmd, "AT+AUTH=", 8) == 0) {
        const char *value = cmd + 8;
        const char *expected;
        if (!is_alnum6(value) || !auth_value(cb, &expected) || strcmp(value, expected) != 0) {
            s->authenticated = false;
            reply(cb, "+ERR=AUTH\r\n");
            return false;
        }
        s->authenticated = true;
        reply(cb, "+OK\r\n");
        return true;
    }
    if (requires_auth(cmd) && !s->authenticated) {
        reply(cb, "+ERR=AUTH_REQUIRED\r\n");
        return false;
    }
    if (strcmp(cmd, "AT+BAUD?") == 0) {
        int code = baud_code(s->uart.baudrate);
        if (code < 0) { reply(cb, "+ERR=UNSUPPORTED\r\n"); return false; }
        snprintf(response, sizeof(response), "+OK=%d\r\n", code);
        reply(cb, response);
        return true;
    }
    if (strncmp(cmd, "AT+BAUD=", 8) == 0) {
        int code;
        static const uint32_t rates[] = {2400,4800,9600,14400,19200,38400,57600,115200};
        air_uart_config_t next = s->uart;
        if (!parse_decimal(cmd + 8, &code) || code < 0 || code > 7) goto bad;
        next.baudrate = rates[code];
        if (!cb->apply_uart || !cb->apply_uart(&next, cb->arg)) {
            if (cb->apply_uart) (void)cb->apply_uart(&s->uart, cb->arg);
            goto apply_failed;
        }
        s->uart = next;
        reply(cb, "+OK\r\n");
        return true;
    }
    if (strcmp(cmd, "AT+PARI?") == 0) {
        snprintf(response, sizeof(response), "+OK=%u\r\n", s->uart.parity);
        reply(cb, response);
        return true;
    }
    if (strncmp(cmd, "AT+PARI=", 8) == 0) {
        int value;
        air_uart_config_t next = s->uart;
        if (!parse_decimal(cmd + 8, &value) || (value != 0 && value != 1)) goto bad;
        next.parity = (uint8_t)value;
        if (!cb->apply_uart || !cb->apply_uart(&next, cb->arg)) {
            if (cb->apply_uart) (void)cb->apply_uart(&s->uart, cb->arg);
            goto apply_failed;
        }
        s->uart = next;
        reply(cb, "+OK\r\n");
        return true;
    }
    if (strcmp(cmd, "AT+DATABITS?") == 0) {
        snprintf(response, sizeof(response), "+OK=%u\r\n", s->uart.databits);
        reply(cb, response);
        return true;
    }
    if (strcmp(cmd, "AT+DATABITS=3") == 0) {
        air_uart_config_t next = s->uart;
        next.databits = 3;
        if (!cb->apply_uart || !cb->apply_uart(&next, cb->arg)) {
            if (cb->apply_uart) (void)cb->apply_uart(&s->uart, cb->arg);
            goto apply_failed;
        }
        s->uart = next;
        reply(cb, "+OK\r\n");
        return true;
    }
    if (strcmp(cmd, "AT+LOGMSG=0") == 0) {
        s->log_suppressed = true;
        reply(cb, "+OK\r\n");
        return true;
    }
    if (strcmp(cmd, "AT+TRANMD=1") == 0) {
        s->raw_mode = true;
        reply(cb, "+OK\r\n");
        return true;
    }
    if (strcmp(cmd, "AT+LINKGPIO?") == 0) {
        snprintf(response, sizeof(response), "+OK=%d,%u,%u,%u\r\n", s->link_gpio,
                 s->link_high ? 1U : 0U, s->link_count, s->link_fault ? 1U : 0U);
        reply(cb, response);
        return true;
    }
    if (strncmp(cmd, "AT+LINKGPIO=", 12) == 0) {
        char *comma = strchr(cmd + 12, ',');
        int gpio, save = 0;
        if (comma) *comma++ = '\0';
        if (!parse_decimal(cmd + 12, &gpio)) {
            if (strcmp(cmd + 12, "-1") != 0) goto bad;
            gpio = -1;
        }
        if (comma && (!parse_decimal(comma, &save) || save > 1)) goto bad;
        if (!cb->apply_link || !cb->apply_link(gpio, save != 0, cb->arg)) goto apply_failed;
        s->link_gpio = gpio;
        snprintf(response, sizeof(response), "+OK=%d,%u,%u,%u\r\n", s->link_gpio,
                 s->link_high ? 1U : 0U, s->link_count, s->link_fault ? 1U : 0U);
        reply(cb, response);
        return true;
    }
    if (strcmp(cmd, "AT+RESET") == 0) {
        reply(cb, "+OK\r\n");
        if (!cb->request_reset || !cb->request_reset(cb->arg)) return false;
        return true;
    }
bad:
    reply(cb, "+ERR=PARAM\r\n");
    return false;
apply_failed:
    reply(cb, "+ERR=APPLY\r\n");
    return false;
}

void air_config_session_init(air_config_session_t *s, const air_uart_config_t *initial, uint32_t generation)
{
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    s->uart.baudrate = 115200;
    s->uart.databits = 3;
    if (initial) s->uart = *initial;
    s->link_gpio = -1;
}

void air_config_session_disconnect(air_config_session_t *s, uint32_t next_generation)
{
    s->generation = next_generation;
    s->authenticated = false;
    s->raw_mode = false;
    s->notify_armed = false;
    s->pending_len = 0;
    s->last_rx_ms = 0;
    s->discarding = false;
}

bool air_config_generation_valid(const air_config_session_t *s, uint32_t generation)
{
    return s && s->generation == generation;
}

void air_config_set_link_state(air_config_session_t *s, int gpio, bool high, unsigned count, bool fault)
{
    s->link_gpio = gpio; s->link_high = high; s->link_count = count; s->link_fault = fault;
}

void air_config_set_notify_armed(air_config_session_t *s, bool armed)
{
    s->notify_armed = armed;
    /* FFF1 subscription is the updater's explicit raw-mode handshake. */
    if (armed) s->raw_mode = true;
    else s->raw_mode = false;
}

bool air_config_flush_idle(air_config_session_t *s, uint32_t now_ms, const air_config_callbacks_t *cb)
{
    if (s->discarding) {
        if (now_ms - s->last_rx_ms >= AIR_CONFIG_IDLE_DELIMITER_MS) {
            s->discarding = false;
            s->pending_len = 0;
        }
        return false;
    }
    if (!s->pending_len || now_ms - s->last_rx_ms < AIR_CONFIG_IDLE_DELIMITER_MS) return false;
    if (s->pending_len > AIR_CONFIG_MAX_COMMAND) {
        s->pending_len = 0;
        return false;
    }
    s->pending[s->pending_len] = '\0';
    s->pending_len = 0;
    return execute(s, s->pending, cb);
}

bool air_config_feed(air_config_session_t *s, const uint8_t *data, size_t length,
                     uint32_t now_ms, const air_config_callbacks_t *cb)
{
    if (!s || !data || !length) return false;
    bool handled = false;
    for (size_t i = 0; i < length; ++i) {
        uint8_t ch = data[i];
        if (ch == '\r' || ch == '\n') {
            if (s->discarding) { s->discarding = false; s->pending_len = 0; continue; }
            if (s->pending_len > AIR_CONFIG_MAX_COMMAND) {
                s->pending_len = 0;
                continue;
            }
            if (!s->pending_len) continue;
            s->pending[s->pending_len] = '\0';
            handled |= execute(s, s->pending, cb);
            s->pending_len = 0;
            continue;
        }
        if (s->discarding) continue;
        if (s->raw_mode && s->pending_len == 0 && ch != 'A') continue;
        if (ch < 0x20 || ch > 0x7e) {
            s->pending_len = 0;
            s->discarding = true;
            reply(cb, "+ERR=PARAM\r\n");
            handled = true;
            continue;
        }
        if (s->pending_len >= AIR_CONFIG_MAX_COMMAND) {
            /* Keep discarding this line so its tail cannot become a command. */
            s->pending_len = AIR_CONFIG_MAX_COMMAND + 1;
            s->discarding = true;
            reply(cb, "+ERR=TOO_LONG\r\n");
            handled = true;
            continue;
        }
        s->pending[s->pending_len++] = (char)ch;
        s->last_rx_ms = now_ms;
    }
    return handled;
}
