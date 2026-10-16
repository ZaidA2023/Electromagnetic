# Firmware source

`main.cpp` runs the approved clock controller on a Seeed XIAO ESP32-C6. D8 supplies serial data, D10 the shift clock, D9 the latch clock, and D7 the active-low register output enable. Seven 74HC595 devices share these controls.

`RegisterOutputs` initializes the idle state and supplies pin writes and timing to the shared logic in `include/clock_display.h`. Its full-frame writer sends seven bytes farthest first using MSBFIRST, then latches once. Every pulse begins with OE_N HIGH, enables it LOW for nominally 1000 us, immediately disables it HIGH, and clears all seven registers. The idle wait includes 1000 us followed by a scheduler yield. Actual pulse width under Wi-Fi scheduling remains a physical acceptance check.

`setup()` disables outputs before configuring serial or Wi-Fi. It selects the private header if present, otherwise the buildable example. Before Wi-Fi or the empty-SSID return, it runs the greeting through the shared controller: centered Hi for 1000 ms, physically blank for 250 ms, then ZAId for 2000 ms. `waitDisplay` yields with outputs disabled after each completed render. Pulse lockout skips the greeting and its holds. Empty SSIDs leave the name visible with no further commands. Configured Wi-Fi uses one explicit reconnect policy, with 30 seconds between attempts.

`serviceWifi()` starts the core NTP service once on first connection. `serviceTime()` polls local time once per second with a zero timeout, then applies one snapshot in 12-hour Central time. Time updates continue from the system clock after a network loss. All Wi-Fi calls and diagnostics occur outside the enabled pulse interval; there are no application callbacks that drive pins.

`SHOW_LEADING_HOUR_ZERO` near the top of `main.cpp` selects `01:00` when true and a blank first digit for `1:00` when false. The default is false; physical glyphs and serial diagnostics use the same option.

The first Hi render establishes all 28 segment positions. The same controller and retained command masks are reused through blanking, the name, and the first time update, suppressing unchanged segments. The user confirmed the original 1 ms hardware test; the replacement clock firmware has not been uploaded or tested on the physical display. See [CLOCK_DESIGN.md](../CLOCK_DESIGN.md) and [STARTUP_DESIGN.md](../STARTUP_DESIGN.md) for the complete contract and verification records.
