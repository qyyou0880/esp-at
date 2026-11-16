# Air configuration implementation

The full ESP-AT application remains the firmware entry point. The C6 main component adds an E104-compatible FFF0 service beside the existing ESP-AT BLE services. FFF1 is the notify characteristic, FFF2 is the raw write characteristic, and FFF3 carries bounded ASCII configuration commands. NimBLE callbacks only enqueue bounded work; parsing, UART changes, LINK GPIO changes, and reset ordering run in the worker. The UART AT task remains the sole UART reader.

`main/air_config_parser.c` implements the production parser and session state: 96-byte bounds, CR/LF and 30 ms idle framing, per-connection generation/auth reset, explicit baud/parity/data-bit validation, atomic apply callbacks, and raw mode that leaves `+++` untouched. `main/air_ble_service.c` registers the service during the existing wrapped NimBLE initialization and starts connectable advertising. Existing LINK GPIO validation, persistence, and GAP listener code remains in `main/link_gpio.c`.

Host tests:

```sh
gcc -std=c11 -Wall -Wextra -Werror -I main main/air_config_parser.c tests/air_config_parser_test.c -o /tmp/air_config_parser_test
/tmp/air_config_parser_test
gcc -std=c11 -Wall -Wextra -Werror -I main main/link_gpio_state.c tests/link_gpio_host_test.c -o /tmp/link_gpio_host_test
/tmp/link_gpio_host_test
```

Both tests pass. They cover fragmented authentication, overlong input, failed apply rollback, invalid parameters, raw `+++` preservation, disconnect generation invalidation, and existing LINK lifecycle/persistence behavior.

The C6 build was configured with the installed pinned IDF 6.1 toolchain and `module_config/module_esp32c6_default/sdkconfig.defaults`; the application and bootloader compile and link successfully. The generated application artifacts are in `build-c6b/esp-at.elf`, `build-c6b/esp-at.bin`, and `build-c6b/bootloader/bootloader.bin`. The aggregate factory-image target then fails in the repository's existing esptool invocation because `flash_project_args` is missing; no hardware was flashed. The generated actual configuration is represented by `build-c6b/config/sdkconfig.json` and `build-c6b/config/sdkconfig.cmake` (the repository does not contain a checked-in `sdkconfig.actual`); it selects `CONFIG_RTC_CLK_SRC_INT_RC=y`, `CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL=y`, `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247`, and retains Wi-Fi/AT/LINK symbols.

Hardware behavior, BLE negotiation, and updater transfer integrity remain untested on physical hardware. Build directories are intentionally untracked.
