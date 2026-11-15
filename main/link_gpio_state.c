/* SPDX-License-Identifier: Apache-2.0 */
#include "link_gpio_state.h"

#include <string.h>

static bool set_level(link_gpio_state_t *state, bool high)
{
    high = high && state->gpio >= 0;
    if (state->gpio < 0 || (state->link_high == high && !state->fault)) {
        state->link_high = high;
        return true;
    }
    if (state->drive && !state->drive(state->gpio, high, state->drive_arg)) {
        state->fault = true;
        return false;
    }
    state->link_high = high;
    state->fault = false;
    return true;
}

void link_gpio_state_init(link_gpio_state_t *state, link_gpio_drive_fn_t drive, void *arg)
{
    memset(state, 0, sizeof(*state));
    state->gpio = -1;
    state->drive = drive;
    state->drive_arg = arg;
}

bool link_gpio_state_configure(link_gpio_state_t *state, int gpio)
{
    int old_gpio = state->gpio;
    bool desired_high = gpio >= 0 && state->active_count != 0;
    if (old_gpio == gpio) {
        return set_level(state, desired_high);
    }
    if (state->drive && gpio >= 0 && !state->drive(gpio, desired_high, state->drive_arg)) {
        state->fault = true;
        return false;
    }
    if (state->drive && old_gpio >= 0 && !state->drive(old_gpio, false, state->drive_arg)) {
        state->fault = true;
        if (gpio >= 0) {
            (void)state->drive(gpio, false, state->drive_arg);
        }
        return false;
    }
    state->gpio = gpio;
    state->link_high = desired_high;
    state->fault = false;
    if (gpio < 0) {
        state->link_high = false;
    }
    return true;
}

bool link_gpio_state_configure_persist(link_gpio_state_t *state, int gpio, link_gpio_save_fn_t save, void *arg)
{
    int old_gpio = state->gpio;
    if (!link_gpio_state_configure(state, gpio)) {
        return false;
    }
    if (save && !save(gpio, arg)) {
        (void)link_gpio_state_configure(state, old_gpio);
        state->fault = true;
        return false;
    }
    return true;
}

bool link_gpio_state_connect(link_gpio_state_t *state, uint16_t handle, int status)
{
    if (status != 0 || handle == UINT16_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < state->active_count; ++i) {
        if (state->handles[i] == handle) {
            return false;
        }
    }
    if (state->active_count >= LINK_GPIO_MAX_CONNECTIONS) {
        return false;
    }
    state->handles[state->active_count++] = handle;
    if (!set_level(state, true)) {
        return false;
    }
    return true;
}

bool link_gpio_state_disconnect(link_gpio_state_t *state, uint16_t handle)
{
    uint8_t found = LINK_GPIO_MAX_CONNECTIONS;
    for (uint8_t i = 0; i < state->active_count; ++i) {
        if (state->handles[i] == handle) {
            found = i;
            break;
        }
    }
    if (found == LINK_GPIO_MAX_CONNECTIONS) {
        return false;
    }
    for (uint8_t i = found + 1; i < state->active_count; ++i) {
        state->handles[i - 1] = state->handles[i];
    }
    --state->active_count;
    if (state->active_count == 0) {
        (void)set_level(state, false);
    }
    return true;
}

void link_gpio_state_reset(link_gpio_state_t *state)
{
    state->active_count = 0;
    memset(state->handles, 0, sizeof(state->handles));
    (void)set_level(state, false);
}

bool link_gpio_state_is_high(const link_gpio_state_t *state) { return state->link_high; }
uint8_t link_gpio_state_active_count(const link_gpio_state_t *state) { return state->active_count; }

