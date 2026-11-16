/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_BT_NIMBLE_ENABLED)
void esp_at_link_gpio_init(void);
void esp_at_link_gpio_clear_for_powerdown(void);
bool esp_at_link_gpio_configure(int gpio, bool save);
void esp_at_link_gpio_get_state(int *gpio, bool *high, unsigned *count, bool *fault);
#else
static inline void esp_at_link_gpio_init(void) {}
static inline void esp_at_link_gpio_clear_for_powerdown(void) {}
static inline bool esp_at_link_gpio_configure(int gpio, bool save) { (void)gpio; (void)save; return false; }
static inline void esp_at_link_gpio_get_state(int *gpio, bool *high, unsigned *count, bool *fault)
{ *gpio=-1; *high=false; *count=0; *fault=true; }
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_BT_NIMBLE_ENABLED)
bool esp_at_bootrst_configure(int boot, int reset, bool enabled);
bool esp_at_bootrst_mode(bool bootloader);
void esp_at_bootrst_get(int *boot, int *reset, bool *enabled);
#endif
