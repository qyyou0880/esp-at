/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AIR_BLE_SERVICE_H
#define AIR_BLE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* E104-compatible custom service retained alongside ESP-AT's A002/A003 services. */
#define AIR_BLE_SERVICE_UUID16 0xFFF0
#define AIR_BLE_NOTIFY_UUID16  0xFFF1
#define AIR_BLE_WRITE_UUID16   0xFFF2
#define AIR_BLE_CONFIG_UUID16  0xFFF3

bool air_ble_service_register(void);
void air_ble_service_unregister(void);
void air_ble_service_autostart(void);
bool air_ble_service_uart_active(void);
bool air_ble_service_uart_route(const uint8_t *data, size_t length);

#endif
