# Air configuration implementation

The normal ESP-AT application remains intact, including Wi-Fi and the existing A002/A003 BLE services. The C6 main component adds the E104-compatible FFF0 service: FFF1 notification, FFF2 raw write, and FFF3 bounded configuration. The service is registered by the wrapped NimBLE initialization and the AT self-command hook starts BLEINIT=2 automatically after AT startup. Self-command responses are captured and suppressed from the host UART.

FFF3 uses the production parser in main/air_config_parser.c. It supports fragmented commands, CR/LF and idle framing, 96-byte command bounds, embedded-control rejection with idle recovery, per-generation authentication, checked parameter validation, rollback callbacks, real baud-code queries, and configuration parsing after raw mode is armed. FFF1 subscription arms raw mode for that connection; disconnect invalidates queued work and authentication. FFF2 accepts chained mbufs up to 512 bytes, including a 258-byte ROM frame.

The UART task remains the sole reader. While FFF1 is subscribed, RX bytes are sent directly to FFF1 and pattern detection is bypassed; FFF2 writes use direct UART TX and never pass through AT output filtering. GAP callbacks only update generation/connection state and enqueue work. Notification mbufs are transferred to NimBLE exactly once. Reinitialization unregisters the listener and deletes the queue/worker before the NimBLE wrapper deinitializes.

Host tests:

    gcc -std=c11 -Wall -Wextra -Werror -I main main/air_config_parser.c tests/air_config_parser_test.c -o /tmp/air_config_parser_test
    /tmp/air_config_parser_test
    gcc -std=c11 -Wall -Wextra -Werror -I main main/air_config_parser.c tests/controller_air_adversarial_test.c -o /tmp/controller_air_adversarial_test
    /tmp/controller_air_adversarial_test
    gcc -std=c11 -Wall -Wextra -Werror -I main main/link_gpio_state.c tests/link_gpio_host_test.c -o /tmp/link_gpio_host_test
    /tmp/link_gpio_host_test

All three commands pass. They cover fragmented authentication, configuration queries after TRANMD, raw +++ preservation, overlong and embedded-NUL recovery, unsupported baud reporting, negative GPIO disable, failed apply rollback, generation invalidation, and LINK lifecycle behavior.

The source-only checkout keeps SDK/build caches ignored. Physical BLE negotiation, UART transfer integrity, reset reboot, and factory-image merge are untested because no hardware is available. The installed pinned SDK path is /home/acer/.local/share/dual-codex/c6-setup-20261115/environment/project/esp-idf. With the supplied venv and compiler PATH, idf.py reaches dependency resolution but stops because the checkout has no managed cjson component and network access is unavailable; no component was downloaded.
