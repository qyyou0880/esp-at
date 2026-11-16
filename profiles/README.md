# C6 board clock profiles

The original module defaults target a board with a powered, stable 32768Hz active
clock on GPIO0. Do not use those defaults for the current development board:
the user confirmed on 2026-11-16 that the external clock is not fitted.

`c6-no-ext32k.defaults` selects the internal RC for RTC and the existing 40MHz
main crystal for BLE. It keeps the LINK feature unchanged. Build in a separate
directory and validate the final sdkconfig choices, rather than assuming a
runtime RTC fallback also changes the BLE controller's clock selection.

In the pinned IDF's `components/bt/controller/esp32c6/bt.c`, `ble_rtc_clk_init`
selects EXT32K from `CONFIG_RTC_CLK_SRC_EXT_OSC` without checking the runtime RTC
mux fallback. The user observed `RTCCLK:RC_SLOW` followed by a restart on
`BLEINIT=2`. This identifies an actual clock configuration mismatch; a crash
backtrace or successful board retest is still needed to close the failure.

For the no-external-clock build, `AT+RTCCLK?` reporting RC_SLOW is expected;
that query reports the RTC clock, not the BLE main-crystal clock.
