/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef LINK_GPIO_MAX_CONNECTIONS
#define LINK_GPIO_MAX_CONNECTIONS 4
#endif

typedef bool (*link_gpio_drive_fn_t)(int gpio, bool high, void *arg);
typedef bool (*link_gpio_save_fn_t)(int gpio, void *arg);

typedef struct {
    int gpio;
    uint16_t handles[LINK_GPIO_MAX_CONNECTIONS];
    uint8_t active_count;
    bool link_high;
    bool fault;
    link_gpio_drive_fn_t drive;
    void *drive_arg;
} link_gpio_state_t;

void link_gpio_state_init(link_gpio_state_t *state, link_gpio_drive_fn_t drive, void *arg);
bool link_gpio_state_configure(link_gpio_state_t *state, int gpio);
bool link_gpio_state_configure_persist(link_gpio_state_t *state, int gpio, link_gpio_save_fn_t save, void *arg);
bool link_gpio_state_connect(link_gpio_state_t *state, uint16_t handle, int status);
bool link_gpio_state_disconnect(link_gpio_state_t *state, uint16_t handle);
void link_gpio_state_reset(link_gpio_state_t *state);
bool link_gpio_state_is_high(const link_gpio_state_t *state);
uint8_t link_gpio_state_active_count(const link_gpio_state_t *state);

