# Air configuration implementation

The normal ESP-AT application remains intact, including Wi-Fi and the existing A002/A003 BLE services. The C6 main component adds the E104-compatible FFF0 service: FFF1 notification, FFF2 raw write, and FFF3 bounded configuration. The service is registered by the wrapped NimBLE initialization and the AT self-command hook starts BLEINIT=2 automatically after AT startup. Self-command responses are captured and suppressed from the host UART.

FFF3 uses the production parser in main/air_config_parser.c. It supports fragmented commands, CR/LF and idle framing, 96-byte command bounds, embedded-control rejection with idle recovery, per-generation authentication, checked parameter validation, rollback callbacks, real baud-code queries, and configuration parsing after raw mode is armed. FFF1 subscription arms raw mode for that connection; disconnect invalidates queued work and authentication. FFF2 accepts chained mbufs up to 512 bytes, including a 258-byte ROM frame.

The UART task is the sole raw reader; normal AT input callbacks return no bytes while FFF1 is armed. Raw TX bypasses AT output filtering. Binary +++ cannot escape the bridge. The worker serializes configuration and UART TX, waits for TX completion before changing parameters, reads the actual driver configuration, and checks NVS writes. Failed apply restores previous UART settings and attempts NVS rollback; unrecoverable rollback faults the session.

GAP/GATT callbacks enqueue bounded work without waiting for UART/NVS. Ownership is claimed by an FFF client, so a legacy connection cannot steal it. Input and output both carry connection generations. Notifications execute on the host event queue, split by negotiated MTU, and transfer mbuf ownership once. Overflow faults/disconnects the session. Fixed queues and one worker are reused through reinitialization; listeners and the NPL event are released on deinit.

Automatic advertising uses the original ESP-AT commands and GAP callback, retaining A002/A003 bookkeeping. BLEINIT runs once after AT initialization; advertising is configured, started and restarted after disconnect. Manual BLEINIT=0 remains usable. Internal capture uses permanent guarded storage, recognizes split responses, rejects concurrent calls, and quarantines timed-out work until its late terminal response. Command payloads are never logged. UART AT output is suppressed while raw and briefly during disconnect; normal AT is available outside the bridge.

IDF is pinned to 4dbfecac7e3b196d3f6826b3db2fdd5a23708e0d. Its standard NPL event posting blocks indefinitely, so the service uses its FreeRTOS queue structures for a zero-timeout post. Recheck this shim before changing IDF.

Host tests:

    gcc -std=c11 -Wall -Wextra -Werror -I main main/air_config_parser.c tests/air_config_parser_test.c -o /tmp/air_config_parser_test
    /tmp/air_config_parser_test
    gcc -std=c11 -Wall -Wextra -Werror -I main main/air_config_parser.c tests/controller_air_adversarial_test.c -o /tmp/controller_air_adversarial_test
    /tmp/controller_air_adversarial_test
    gcc -std=c11 -Wall -Wextra -Werror -I main main/link_gpio_state.c tests/link_gpio_host_test.c -o /tmp/link_gpio_host_test
    /tmp/link_gpio_host_test

Additional tests execute actual production code against deterministic OS/driver mocks:

    python tests/controller_runtime_test.py
    python tests/controller_self_cmd_test.py

The runtime test covers a 258-byte binary frame with zero bytes and +++, config after TRANMD, MTU23 splitting, rejected second owners, stale queued input/output after handle reuse, auth reset, unsubscribe, fixed-resource reinit and overflow. The capture test covers split OK, ERROR, concurrent rejection, timeout quarantine and late completion. These tests do not prove radio timing or physical UART behavior.

The Windows controller independently compiled source 68b7c613 with the installed pinned IDF on Acer, including the linked ELF and factory merge. Actual configuration/symbols confirmed full Wi-Fi AT, self commands, RTC internal RC, BLE main XTAL and LINK/air-service inclusion. Subsequent cleanup changes require another build before flashing. Hardware auto-advertising, air configuration, UART integrity and STM32 ROM operation remain pending until actually tested. The earlier worker's dependency-resolution limitation was resolved by using the existing environment/cached components; it was not evidence that Acer's environment was broken.
