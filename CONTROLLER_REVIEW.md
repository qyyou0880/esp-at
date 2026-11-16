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

## Controller follow-up during the explicitly authorized second implementation

These are intentional controller review inputs, not unexplained user edits. The
controller expanded tests/controller_air_adversarial_test.c with a failing test:
after an invalid command WITHOUT CRLF and an idle boundary, a valid no-CRLF command
must work. Current discarding state never recovers because flush_idle returns early.
Track last receive time on discarded bytes; idle should clear discard, not execute
any discarded suffix. Current client does not send CRLF.

The controller also inspected the service intermediate at ~700 seconds; re-check
these before declaring completion:
- Worker calls advertising before NimBLE host sync, then leaves adv_pending false
  on failure. Ensure real automatic advertisement after host sync, not only after
  the first disconnect. Preserve normal AT BLE advertising state consistency.
- air_reply and UART notification race disconnect/reuse between checking s_conn
  and notifying. All output needs generation-tagged host-queue dispatch. Raw UART
  chunks must be segmented to actual negotiated MTU-3 (20 if no exchange), not
  blindly notify 256 bytes and disconnect on a valid MTU23 link.
- Single-owner generation/state changes and unsubscribe must not let other legacy
  connections steal the owner or clear another connection. No shared session data
  races across GAP, worker and UART tasks. Queue overflow must fail a session.
- UART configuration must read actual current driver values, wait for previous TX
  completion and roll back correctly. NVS may not match local AT+UART_CUR changes.
- Reset callback must schedule a delay after its +OK notification; not immediate.
- Ensure CONFIG_AT_SELF_COMMAND_SUPPORT is enabled in the actual target defaults;
  merely putting autostart under an off-by-default ifdef is not implementation.
- Still require production routing/session tests for a 258-byte binary frame,
  stale queued input/output, config-after-TRANMD, unsubscribe/reconnect and raw +++.

Do not stop solely because these two review/test files changed: they were
intentionally supplied and authorized by the Windows controller for this task.

Controller verified build environment just now: rich-click 1.9.9 IS present in
/home/acer/.local/share/dual-codex/c6-setup-20261115/environment/idf-tools/python_env/idf6.1_py3.10_env/bin/python
(pip show confirmed). It is not in bootstrap/bin/python. Use bootstrap only to run
idf_tools.py export; run idf.py with the IDF venv interpreter and exported PATH.
The earlier noext32k build succeeded with that exact existing environment; no
package installs are needed. Reuse existing managed_components from environment/project.

The current self-command diff only removes UART write and bounds one-chunk search.
It does NOT yet serialize concurrent calls, match a split response, or keep callback
state valid after timeout cleanup. Fix these actual lifetime issues as required by
the original review; do not claim scoped/split-safe capture until implemented.

Controller supplied a concrete replacement candidate at /tmp/parent_at_self_cmd.c
and test at /tmp/test_parent_self_cmd.py. It passes Windows GCC -Wall -Wextra -Werror
with split terminal OK, ERROR, reentrant caller rejection, timeout quarantine and
late response recovery. Review and adapt it instead of leaving self-command UAF
and one-chunk matching unresolved. Persistent guarded state eliminates frees while
the core can still reference it; no command payload logging. It expects autostart
to use the full terminal string "\r\nOK\r\n", not a substring "OK".
These /tmp candidate/test inputs are deliberately controller-authored/authorized.
To run Linux host test: python3 /tmp/test_parent_self_cmd.py /tmp/parent_at_self_cmd.c
Reuse existing managed_components AND dependencies.lock from the successful base
environment/project before CMake so the offline sandbox need not fetch registry.

Controller supplied a concrete replacement candidate at /tmp/parent_at_self_cmd.c
and test at /tmp/test_parent_self_cmd.py. It passes Windows GCC -Wall -Wextra -Werror
with split terminal OK, ERROR, reentrant caller rejection, timeout quarantine and
late response recovery. Review and adapt it instead of leaving self-command UAF
and one-chunk matching unresolved. Persistent guarded state eliminates frees while
the core can still reference it; no command payload logging. It expects autostart
to use the full terminal string "\r\nOK\r\n", not a substring "OK".
These /tmp candidate/test inputs are deliberately controller-authored/authorized.
To run Linux host test: python3 /tmp/test_parent_self_cmd.py /tmp/parent_at_self_cmd.c
Reuse existing managed_components AND dependencies.lock from the successful base
environment/project before CMake so the offline sandbox need not fetch registry.

Critical legacy compatibility: direct ble_gap_adv_start(..., NULL, NULL) bypasses
the ESP-AT GAP callback that tracks connections/SPP. A GAP listener for LINK is
not a substitute. Prefer internally executing normal AT BLEADVPARAM, BLEADVDATA,
BLEADVSTART (after BLEINIT has completed and full terminal response captured),
including disconnect restart. This preserves the existing AT connection state
and A002/A003 behavior. Do not merely add raw GAP advertising with a NULL callback.
Controller candidate now also explicitly clears completed-generation before reuse.
