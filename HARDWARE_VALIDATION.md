# ESP32-C6 air bridge validation — 2026-11-16

Firmware tested: `3a04ade3f8b221e026a6896f69c941adeefbfce6`,
`SEISTECH-C6-AIR-1`. ESP32-C6 rev0.2, main crystal 40MHz, no external 32k
source. Actual SDK configuration uses RTC internal RC and BLE main XTAL.
The full ESP-AT application retains Wi-Fi, HTTP, BLE, OTA and OTA SSL support.
No Wi-Fi mode is forced by the extension; the tested board retained mode 0.

Independent Acer build linked and merged the factory image. The partition table
matches the previous firmware. Windows flashed bootloader/application, then later
application-only corrections, verifying written hashes. NVS and partition table
were preserved. Final application: 1,804,160 bytes, SHA-256
`79cbb765bc429a69a11bc631ca70e3d106fe6501fccd799a982d6e98826df086`.

## Hardware tests passed

- Automatic advertising after boot; Raspberry Pi connects without UART BLEINIT,
  BLESPP or BLEADVSTART commands. FFF0 and original A002/A003 services coexist.
- Per-connection authentication, split authentication writes and independent
  configuration queries. LINK GPIO10 reports connected `10,1,1,0` and normal UART
  query after disconnect reports `10,0,0,0`.
- Air configuration to 38400 8E1, queries after TRANMD, reset, persisted readback;
  restoration to 115200 8N1 and persisted readback after another reset.
- Actual GUI configuration paths select C6, apply and restore UART parameters
  without E104 physical-restart prompts.
- COM12 ordinary UART AT, GMR, CWMODE?, BLEINIT?, RTCCLK?, LINKGPIO? and UART_CUR?
  all return OK. GMR identifies compiled source `3a04ade3f`; BLEINIT? returns 2.
- Physical UART↔BLE tests at 115200 8N1 and 38400 8E1: three 258-byte frames in
  each direction per setting, exact byte comparison including NUL, +++ and 0xFF.
  No extra AT/status/echo bytes entered the raw UART stream.
- Actual BLE MTU247 and 244-byte writes verified. BlueZ sometimes preserves a
  stale characteristic limit of 20 after reconnect. The client opts into the
  verified negotiated-MTU fallback only for the known C6 512-byte FFF2 attribute;
  E104 retains its conservative behavior.

The initial 30ms idle delimiter split valid writes on the real link; it is now
100ms. The client must call Bleak.disconnect even after a peripheral reset to
release its D-Bus connection. A regression test covers that cleanup; repeated
reset/configure/restore passed with the corrected client. Air RESET acknowledges,
closes the BLE link and then reboots.

## Tests and boundaries

Production parser/adversarial/LINK tests compile with GCC warnings as errors.
`controller_runtime_test.py` executes production service routing/session code
against OS/driver mocks. `controller_self_cmd_test.py` tests split responses,
ERROR, concurrent rejection, timeout quarantine and late completion. Windows and
Acer runs passed. The companion Raspberry Pi updater has 35 passing tests on
Windows and Pi; its five 480×320 pages passed clipping checks.

STM32 stayed powered off during these tests. No C6-to-STM32 ROM synchronization,
firmware write, full readback or sustained ROM throughput measurement has yet
been performed. Short-frame UART timings are not an STM32 upgrade benchmark.
External-32k boards, sleep/wake, Wi-Fi association/throughput and every legacy AT
command have not been hardware-tested. Current UART is restored to 115200 8N1
and all test BLE connections are closed.
