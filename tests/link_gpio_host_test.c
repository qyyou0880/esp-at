/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "../main/link_gpio_state.h"

typedef struct { int calls; int last_gpio; bool last_high; bool fail; } drive_log_t;
typedef struct { int saved_gpio; bool fail; } nvs_mock_t;
static bool drive(int gpio, bool high, void *arg)
{
    drive_log_t *log = arg;
    ++log->calls; log->last_gpio = gpio; log->last_high = high;
    return !log->fail;
}
static bool save_nvs(int gpio, void *arg)
{
    nvs_mock_t *mock = arg;
    if (mock->fail) return false;
    mock->saved_gpio = gpio;
    return true;
}

static void test_lifecycle(void)
{
    drive_log_t log = {0};
    link_gpio_state_t s;
    link_gpio_state_init(&s, drive, &log);
    assert(s.gpio == -1 && !link_gpio_state_is_high(&s));
    assert(link_gpio_state_configure(&s, 8));
    assert(!link_gpio_state_connect(&s, 1, 7));
    assert(link_gpio_state_active_count(&s) == 0);
    assert(link_gpio_state_connect(&s, 1, 0));
    assert(!link_gpio_state_connect(&s, 1, 0));
    assert(link_gpio_state_connect(&s, 2, 0));
    assert(link_gpio_state_is_high(&s));
    assert(!link_gpio_state_disconnect(&s, 99));
    assert(link_gpio_state_disconnect(&s, 1));
    assert(link_gpio_state_is_high(&s));
    assert(link_gpio_state_disconnect(&s, 2));
    assert(!link_gpio_state_is_high(&s));
    link_gpio_state_reset(&s);
    assert(link_gpio_state_active_count(&s) == 0 && !link_gpio_state_is_high(&s));
}

static void test_fail_closed(void)
{
    drive_log_t log = {.fail = true};
    link_gpio_state_t s;
    link_gpio_state_init(&s, drive, &log);
    assert(!link_gpio_state_configure(&s, 9));
    assert(s.gpio == -1);
    log.fail = false;
    assert(link_gpio_state_configure(&s, 9));
    assert(link_gpio_state_connect(&s, 3, 0));
    log.fail = true;
    assert(!link_gpio_state_disconnect(&s, 3) || link_gpio_state_active_count(&s) == 0);
}

static void test_persistence_rollback(void)
{
    drive_log_t drive_log = {0};
    nvs_mock_t nvs = {.saved_gpio = -1, .fail = false};
    link_gpio_state_t s;
    link_gpio_state_init(&s, drive, &drive_log);
    assert(link_gpio_state_configure_persist(&s, 8, save_nvs, &nvs));
    assert(s.gpio == 8 && nvs.saved_gpio == 8);
    nvs.fail = true;
    assert(!link_gpio_state_configure_persist(&s, 9, save_nvs, &nvs));
    assert(s.gpio == 8);
}

static void test_disabled_then_enabled_with_existing_connection(void)
{
    drive_log_t log = {0};
    link_gpio_state_t s;
    link_gpio_state_init(&s, drive, &log);
    assert(link_gpio_state_connect(&s, 42, 0));
    assert(s.active_count == 1 && !s.link_high && log.calls == 0);
    assert(link_gpio_state_configure(&s, 18));
    assert(s.link_high && log.last_gpio == 18 && log.last_high);
    assert(link_gpio_state_configure(&s, -1));
    assert(!s.link_high && s.active_count == 1 && !log.last_high);
    assert(link_gpio_state_configure(&s, 19));
    assert(s.link_high && log.last_gpio == 19 && log.last_high);
    int calls = log.calls;
    assert(link_gpio_state_configure(&s, 19));
    assert(log.calls == calls); /* same pin never gets a low pulse */
}

static void test_capacity_and_fault_recovery(void)
{
    drive_log_t log = {0};
    link_gpio_state_t s;
    link_gpio_state_init(&s, drive, &log);
    assert(link_gpio_state_configure(&s, 18));
    for (unsigned i = 0; i < LINK_GPIO_MAX_CONNECTIONS; i++) {
        assert(link_gpio_state_connect(&s, (uint16_t)i, 0));
    }
    assert(!link_gpio_state_connect(&s, 1234, 0));
    assert(s.active_count == LINK_GPIO_MAX_CONNECTIONS);
    link_gpio_state_reset(&s);
    log.fail = true;
    assert(!link_gpio_state_connect(&s, 55, 0));
    assert(s.active_count == 1 && s.fault && !s.link_high);
    log.fail = false;
    assert(link_gpio_state_configure(&s, 18));
    assert(s.active_count == 1 && s.link_high && !s.fault);
    log.fail = true;
    assert(link_gpio_state_disconnect(&s, 55));
    assert(s.active_count == 0 && s.fault && s.link_high);
    log.fail = false;
    link_gpio_state_reset(&s);
    assert(!s.link_high && !s.fault);
}

static void test_persist_rollback_while_connected(void)
{
    drive_log_t log = {0};
    nvs_mock_t nvs = {.saved_gpio = 18, .fail = true};
    link_gpio_state_t s;
    link_gpio_state_init(&s, drive, &log);
    assert(link_gpio_state_configure(&s, 18));
    assert(link_gpio_state_connect(&s, 2, 0));
    assert(!link_gpio_state_configure_persist(&s, -1, save_nvs, &nvs));
    assert(s.gpio == 18 && s.link_high && s.active_count == 1 && s.fault);
    assert(log.last_gpio == 18 && log.last_high);
}

int main(void)
{
    test_lifecycle();
    test_fail_closed();
    test_persistence_rollback();
    test_disabled_then_enabled_with_existing_connection();
    test_capacity_and_fault_recovery();
    test_persist_rollback_while_connected();
    puts("link_gpio_host_test: PASS");
    return 0;
}
