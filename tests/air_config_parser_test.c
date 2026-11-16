/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/air_config_parser.h"

typedef struct { char out[512]; size_t len; bool apply_ok; int applies; bool reset; } mock_t;
static void out(const char *s, size_t n, void *arg) { mock_t *m = arg; if (m->len + n < sizeof(m->out)) { memcpy(m->out + m->len, s, n); m->len += n; m->out[m->len] = 0; } }
static bool apply_uart(const air_uart_config_t *c, void *arg) { mock_t *m = arg; (void)c; ++m->applies; return m->apply_ok; }
static bool apply_link(int gpio, bool save, void *arg) { (void)gpio; (void)save; return ((mock_t *)arg)->apply_ok; }
static bool reset(void *arg) { ((mock_t *)arg)->reset = true; return true; }
static air_config_callbacks_t cb(mock_t *m) { return (air_config_callbacks_t){.apply_uart=apply_uart,.apply_link=apply_link,.request_reset=reset,.reply=out,.arg=m}; }

int main(void)
{
    air_config_session_t s; air_uart_config_t initial = {115200, 0, 3}; mock_t m = {.apply_ok = true};
    air_config_session_init(&s, &initial, 1); air_config_callbacks_t c = cb(&m);
    air_config_feed(&s, (const uint8_t *)"AT+AUTH=123", strlen("AT+AUTH=123"), 0, &c);
    assert(!s.authenticated); air_config_feed(&s, (const uint8_t *)"456", 3, 10, &c); air_config_flush_idle(&s, 10+AIR_CONFIG_IDLE_DELIMITER_MS, &c); assert(s.authenticated);
    m.len = 0; air_config_feed(&s, (const uint8_t *)"AT+BAUD=6\r\n", strlen("AT+BAUD=6\r\n"), 50, &c); assert(s.uart.baudrate == 57600);
    m.apply_ok = false; air_config_feed(&s, (const uint8_t *)"AT+PARI=1\r\n", strlen("AT+PARI=1\r\n"), 60, &c); assert(s.uart.parity == 0 && strstr(m.out, "+ERR=APPLY"));
    m.apply_ok = true; m.len = 0; char long_cmd[112]; memset(long_cmd, 'X', 110); long_cmd[110] = '\r'; long_cmd[111] = '\n'; air_config_feed(&s, (uint8_t *)long_cmd, sizeof(long_cmd), 70, &c); assert(strstr(m.out, "+ERR=TOO_LONG"));
    m.len = 0; air_config_feed(&s, (const uint8_t *)"AT+TRANMD=1\r\n", strlen("AT+TRANMD=1\r\n"), 80, &c); assert(s.raw_mode);
    m.len = 0; air_config_feed(&s, (const uint8_t *)"AT+BAUD?\r\n", strlen("AT+BAUD?\r\n"), 90, &c); assert(strstr(m.out, "+OK=6"));
    air_config_set_notify_armed(&s, true); assert(s.raw_mode && s.notify_armed);
    m.len = 0; air_config_feed(&s, (const uint8_t *)"AT+PARI?\r\n", strlen("AT+PARI?\r\n"), 100, &c); assert(strstr(m.out, "+OK=0"));
    air_config_session_disconnect(&s, 2); assert(!s.authenticated && !air_config_generation_valid(&s, 1));
    m.len = 0; air_config_feed(&s, (const uint8_t *)"AT+BAUD?\r\n", strlen("AT+BAUD?\r\n"), 90, &c); assert(strstr(m.out, "AUTH_REQUIRED"));
    m.len = 0; air_config_feed(&s, (const uint8_t *)"AT+RESET\r\n", strlen("AT+RESET\r\n"), 100, &c); assert(!strstr(m.out, "+OK"));
    puts("air_config_parser_test: PASS"); return 0;
}
