# Firmware source

`main.cpp` runs the approved clock controller on a Seeed XIAO ESP32-C6. D8 supplies serial data, D10 the shift clock, D9 the latch clock, and D7 the active-low register output enable. Seven 74HC595 devices share these controls.

`RegisterOutputs` initializes the idle state and supplies pin writes and timing to the shared logic in `include/clock_display.h`. Its full-frame writer sends seven bytes farthest first using MSBFIRST, then latches once. Every pulse begins with OE_N HIGH, enables it LOW for nominally 1000 us, immediately disables it HIGH, and clears all seven registers. The idle wait includes 1000 us followed by a scheduler yield. Actual pulse width under Wi-Fi scheduling remains a physical acceptance check.

`setup()` disables outputs before configuring serial or Wi-Fi. It selects the private header if present, otherwise the buildable example. Empty SSIDs keep the controller idle. Configured Wi-Fi uses one explicit reconnect policy, with 30 seconds between attempts.

`serviceWifi()` starts the core NTP service once on first connection. `serviceTime()` polls local time once per second with a zero timeout, then applies one snapshot in 12-hour Central time. Time updates continue from the system clock after a network loss. All Wi-Fi calls and diagnostics occur outside the enabled pulse interval; there are no application callbacks that drive pins.

The first render establishes all 28 segment positions. Retained command masks suppress unchanged segments afterward. The user confirmed the original 1 ms hardware test; the replacement clock firmware has not been uploaded or tested on the physical display. See [CLOCK_DESIGN.md](../CLOCK_DESIGN.md) for the complete contract and verification record.
