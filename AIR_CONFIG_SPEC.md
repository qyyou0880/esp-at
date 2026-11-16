# SeisTech ESP32-C6 air configuration extension

User explicitly requires retaining full ESP-AT Wi-Fi and all existing AT commands.
Do NOT replace with a BLE-only application or remove the existing LINK feature.
Actual current board has no external 32768Hz source: RTC internal RC and BLE main
40MHz XTAL are required. External-clock support is a separate board profile.

## Operator experience

- C6 starts connectable BLE advertising automatically after boot, with no UART
  commands from STM32. Wi-Fi AT functionality remains available; do not silently
  force Wi-Fi off on every boot. Default Wi-Fi mode may remain whatever ESP-AT uses.
- LINK follows the real BLE connection and defaults to saved GPIO (disabled on
  virgin NVS). Debug GPIO10 may be selected explicitly; never assume GPIO14 is free.
- STM32 only receives raw upgrade bytes. Internal AT startup/configuration commands,
  their OK/ERROR responses, and BLE status strings must never leak onto STM32 UART
  during bridge operation. No parsing of status text as connection state.
- Config and raw data remain independent while the same connection is used.
- Full normal UART AT operation must be retained outside data-bridge mode; control
  air-only mode via a documented local AT command if necessary. Do not consume UART
  bytes from two tasks. Do not make STM32 transmit `BLEINIT` or `BLESPP`.

## Wire contract (E104-compatible subset)

Custom 16-bit service FFF0, FFF1 notify (UART -> BLE), FFF2 write and
write-without-response (BLE -> UART), FFF3 write+notify (air config). Existing
ESP-AT A002/A003 services must not be broken/replaced.

Config commands are ASCII writes to FFF3, at most 96 bytes, accepted with optional
trailing CRLF. A single command may be split across multiple <=20 byte writes:
the current client sends e.g. AT+AUTH=xxxxxx without CRLF. Implement bounded
assembly and a 100ms idle delimiter, not "execute every fragment". The initial
30ms trial split valid authentication fragments on the real BlueZ/C6 link.
Support newline-delimited multiple commands without mixing notifications.
Responses must end CRLF; success is `+OK` or `+OK=<value>`; failure `+ERR=<reason>`.
Never log authentication payloads, passwords or internal commands containing them.

- `AT+AUTH=<6 ASCII alphanumeric>`: authenticate configuration for THIS connection
  only. Public factory default is 123456. Reject all modifying/query config commands
  before authentication (AT/version may be public if documented). Clear auth and
  pending fragments at disconnect; do not reuse a stale command on a new connection.
- `AT+VER?`: identify as `+OK=SEISTECH-C6-AIR-2` (NOT FW-7534).
- `AT+BAUDRATE?`, `AT+BAUDRATE=<absolute>`: AIR-2 absolute baud rate, 80..5000000,
  subject to actual UART driver acceptance. Query reports actual divisor-derived
  rate. Application target for the C6 companion STM32 build is 1000000 8N1.
  This does not change the E104-compatible BAUD code mapping below, or ROM speed.
- `AT+BAUD?`, `AT+BAUD=<code>`: verified mapping 0=2400,1=4800,2=9600,
  3=14400,4=19200,5=38400,6=57600,7=115200; reject unsupported codes rather than
  guessing. Query defaults to code7 on virgin NVS.
- `AT+PARI?`, `AT+PARI=0|1`: 0 none, 1 EVEN (BT52 mapping, NOT ESP-AT parity1).
- `AT+DATABITS?`, `AT+DATABITS=3`: 3 means 8 data bits. Reject other values.
- `AT+LOGMSG=0`: suppress UART status output during bridge mode.
- `AT+TRANMD=1`: transparent data mode; no framing headers or local command parsing
  of binary data. A `+++` binary sequence must not escape raw bridge mode.
- `AT+LINKGPIO?`, `AT+LINKGPIO=<gpio>[,<save>]`: preserve current validated pin
  restrictions, existing local AT syntax, connection handling and NVS rules. Air
  reply uses +OK=<gpio>,<link>,<count>,<fault> for the GUI; local command unchanged.
- `AT+RESET`: send success if feasible then reboot/disconnect intentionally.
  Reboot must reload saved UART parameters and automatically advertise again.
- Optional but useful: UART absolute baud command, name config, status/fault query,
  auth password change. Do not add guessed E104 baud mappings.

Raw data does not require config authentication (same as E104 and current updater).
Enabling notifications on FFF1 arms UART raw bridge for the connection. FFF3-only
configuration does NOT require starting AT BLESPP. All active GPIO LINK events must
be true GAP events. Configuration is applied atomically by a worker with no UART
write in flight, persisted with checked NVS errors; +OK cannot acknowledge a failed
apply. Queries reflect actual driver settings. Existing GUI reconnects after RESET
and sends raw data without repeating AUTH; support that.

## Engineering boundaries

- Reuse the existing NimBLE init/deinit wrappers to add a GATT service before host
  starts; confirm the pinned IDF lifecycle. Do not replace normal GAP event callbacks.
- GATT callbacks must not block on UART, NVS, self-AT commands, mutexes that can
  create host-lock inversions, or waiting for their own host task. Use bounded queues
  and worker tasks. Generation-tag queued data/config so disconnect invalidates
  old traffic/auth; avoid replying on a reused connection handle.
- Avoid silent data drops. Report queue/notify/UART overflow and fail/disconnect
  the session so an updater cannot mistake lost bytes for successful transfer.
- UART task interception must ensure one reader; AT core and bridge cannot race to
  consume the same UART FIFO. On raw mode, disable existing UART +++ detection;
  restore normal AT input mode safely when data connection closes.
- `esp_at_exe_cmd` exists under CONFIG_AT_SELF_COMMAND_SUPPORT. Existing self-cmd
  output writes to the physical UART and uses unsafe unbounded strstr on a data
  buffer. If used, add a scoped, serialized capture/suppression path and bounded
  split-response matching; never free command state while core still references it.
  Do not handwave this risk or leak boot initialization OK/ERROR to STM32.
- Default preferred MTU at least247, preferably517 after verifying characteristic
  lengths and buffers support it. Request a valid short BLE interval; actual
  negotiated values must be observable, not merely reported from desired settings.
- Preserve full existing ESP-AT functions. It is acceptable that local UART commands
  are unavailable while UART is in raw bridge mode, just as normal SPP behaves.
- No hardware flashing, sudo, key/permissions changes or downloads/installing SDK.

## Acceptance evidence

Implement host-testable actual production command parser/fragment/session logic
and tests covering auth fragmentation, overlong commands, bad parameters, failed
apply, per-connection auth reset, stale queued commands, binary +++ untouched,
UART baud/parity mapping, reset ordering, configuration/data separation, and raw
bridge disconnect cleanup. Include meaningful failure tests.
Compile and link target firmware against the installed pinned IDF; check actual
sdkconfig is INT_RC + BLE MAIN_XTAL (not EXT_OSC/default LP), command/FFF service
symbols included, LINK symbols retained. Document untested hardware behavior.

SDK available read-only for reuse on Acer:
`/home/acer/.local/share/dual-codex/c6-setup-20261115/environment/project/esp-idf`
Tools: sibling `idf-tools`, Python env `idf-tools/python_env/idf6.1_py3.10_env`.
Do not duplicate hundreds of MB of caches or SDK inside the Git commit. Build in
checkout build directory, use existing tools and an explicit no-external-clock
sdkconfig. Target build may need existing ESP-AT components; preserve source base.

Produce IMPLEMENTATION.md with exact tests/commands, limitations, build artifact
locations and protocol. Commit code and documentation, not artifacts/caches/secrets.
