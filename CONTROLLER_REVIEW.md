# Blocking independent review of the first air-service draft

These notes were intentionally authored and supplied by the Windows controller
during the previous task, as was `tests/controller_air_adversarial_test.c`.
They are authorized review inputs, not unexplained user changes. Some findings
refer to an intermediate snapshot; re-check the final source before fixing each.

This draft is NOT functional and must not be described as implemented just because
it compiles. Finish AIR_CONFIG_SPEC.md, preserving full ESP-AT. Specific failures:

1. `s_conn` is never assigned. No GAP connect/disconnect/subscription event handling
   updates owner/auth/generation. Every queued config write is discarded. Connect
   registration must not be confused with "NimBLE init was called".
2. Nothing starts BLEINIT on boot. The service is only registered AFTER someone
   manually calls BLEINIT, which is precisely the user requirement to eliminate.
3. No UART RX -> FFF1 path exists. `s_notify_val_handle` is unused/unassigned.
   The unchanged AT UART task still sends every byte to the AT parser. That is NOT
   a transparent bridge and does NOT preserve arbitrary binary `+++`.
4. Raw UART status/core replies still go to STM32. `esp_at_port_write_data` is not
   a safe raw TX API once output suppression is added; use direct UART data writes
   in the bridge worker, suppress AT-output writes only while raw mode/internal
   boot commands are active.
5. The FFF3 parser stops after AT+TRANMD=1. This confuses the dedicated CONFIG
   characteristic with raw DATA. The existing client then queries BAUD/PARI and
   hangs. FFF3 MUST keep parsing independently while FFF2 stays byte-transparent.
6. A 96-byte buffer is valid for config, NOT raw DATA. Real ROM write frames are
   258 bytes. Use OS_MBUF_PKTLEN for chained mbufs, with separate config/data bounds.
7. `ble_gatts_notify_custom` consumes its mbuf even on failure; freeing again is a
   double-free. Check the pinned API. Resetting immediately before sending +OK also
   cannot work. Schedule reboot after ACK notification is queued/drained.
8. UART persistence currently returns void and errors are ignored. Must use a
   checked NVS API, reflect actual driver settings in queries, and restore previous
   settings if apply/persist fails. Baud queries must NOT silently report code7
   for a real unsupported baud. LINKGPIO=-1 currently risks uninitialized gpio.
9. Embedded NUL currently executes a truncated command. Independent Windows host
   test fails. Reject non-ASCII/control bytes with line/idle-discard recovery.
10. register/deinit cannot allocate a new permanent queue/worker every reinit.
    Generation-tag pending output as well as input; don't notify a reused handle.
11. Build caches/SDK symlinks must be ignored rather than added by the bridge's
    automatic `git add -A`. A source-only return is mandatory.

## Viable integration approach

- Keep existing wrapped NimBLE init and add FFF0 service there, but create a real
  auto-start hook after AT initialization (or the supported ready-before hook) that
  executes BLEINIT=2 through the AT self-command engine. Configure name/advertising
  in a way consistent with the normal AT state. Start advertising again after
  disconnect. Do not unconditionally disable saved Wi-Fi functionality.
- CONFIG_AT_SELF_COMMAND_SUPPORT must actually be enabled. If using self commands,
  suppress their UART output with a scoped flag at the UART write callback. Fix
  existing self-command response matching to be bounded/split-safe, serialize it,
  and keep referenced buffers alive on timeout. Never call it from a GATT callback
  or an AT handler. Do NOT leave these races as an undocumented TODO.
- FFF1 CCCD subscription arms a raw bridge (not AT BLESPP) for the air-session owner.
  Add a UART-task hook before notifying the AT core; it must be the sole reader.
  While armed, route raw UART bytes to FFF1 notifications. Normal AT behavior is
  retained outside raw mode; no one asks STM32 to issue any AT commands.
- GAP callbacks only update short protected owner/generation/subscription state and
  enqueue work, never wait for NVS/UART/self-AT commands. Worker applies config and
  raw TX. Marshal output through host events/queues if needed so a generation check
  and notification cannot race handle reuse. Scope one air bridge owner at a time,
  while not breaking legacy A002 clients' normal AT functionality.
- Mode-switch flushing must not discard the first legitimate ROM ACK: ensure the
  UART reader has completed activation/flush before worker sends first raw UART TX.
- Fail/disconnect on queue/UART/notification overflow, never silently drop bytes.

## Concrete acceptance

Run all original and additional adversarial parser tests. Add routing/session tests
using production state logic (not tests asserting helper assignments). Prove config
queries still work after TRANMD=1, config bytes never reach UART, raw binary is not
parsed, disconnect invalidates queued input/output, reinit cleans up, and a 258-byte
data frame is accepted. Independently compile the complete target and document the
actual source files that implement auto-start and RX routing. No hardware flashing.

An incomplete scaffold with a successful link is still incomplete. Update
IMPLEMENTATION.md to match what is actually implemented and tested.
