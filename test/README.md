# Tests

`clock_display_test.cpp` is a standalone C++11 test program using the same display logic as the firmware. It needs no Arduino runtime, board, network, or test library.

## Run the host checks

From the repository root:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -pedantic -Iinclude test/clock_display_test.cpp -o /tmp/electromagnetic-clock-tests
/tmp/electromagnetic-clock-tests
```

All 16 test groups pass. They cover the full schematic mapping, uniqueness of the 56 outputs, an independent physical serial-chain simulation, stale-register clearing, every minute of the day with and without a leading hour zero, invalid input, startup state, changed-segment updates, blank hour tens, backward time changes, disabled pulses, snapshot consistency, and exact 2026 Central daylight saving boundaries. The requested `1:00`/`01:00` example is checked explicitly.

Greeting checks reconstruct visible Hi, blank, and ZAId from the actual command log using independent schematic labels. They require 28/7/18 commands, holds of 1000/250/2000 ms with outputs disabled and all command latches zero, cached handoff to time in both leading-zero modes, and no greeting pulses/holds under lockout. The host tests establish software ordering; before-Wi-Fi placement is also verified by the source review of `setup()`.

The fake backend executes the real pulse routine and rejects writes while outputs are enabled. It requires the pulse wait to be followed immediately by disabling outputs, checks that only one command is active, and checks zero commands before the idle wait. These tests establish command ordering; they do not measure electrical timing, coil current, or mechanical motion.

## Device builds

Build environment `seeed_xiao_esp32c6` using the pinned vendor platform, Arduino core 3.3.7, and esptool 5.1.2. Both the empty-network build and a temporary build with dummy settings must compile; the latter ensures the Wi-Fi path also links and fits the flash partition. Dummy settings are only for a temporary build copy and must not replace the local private configuration.

The `/OE` startup sequence uses `gpio_set_level` before OUTPUT mode because Arduino core 3.3.7 ignores `digitalWrite` before GPIO registration. It then explicitly disables outputs again before initializing the other pins or clearing registers.

## Physical acceptance still required

After supplying real Wi-Fi settings, use the actual board to complete these checks:

1. Verify OE_N is HIGH before the first startup pulse, during every greeting hold, while waiting for time, and between commands. Check representative QA, QH, and far-end outputs against the schematic.
2. Measure the nominal 1 ms active pulse and at least 1 ms disabled interval with Wi-Fi active. Framework scheduling can stretch software delays; the implementation does not provide a hardware-enforced maximum pulse length.
3. Verify segment SET/RESET direction and the startup order: centered Hi for 1 second, physical blank for 250 ms, ZAId for 2 seconds, then Wi-Fi setup. Verify the name is replaced by correct Central time, a minute/hour change, and both leading-hour-zero settings. Pulse lockout should skip the greeting and every text hold.
4. Disconnect Wi-Fi after time synchronization. Confirm the clock continues updating and reconnects when the network returns, without replaying the greeting. Power cycling while offline should repeat the greeting and leave ZAId visible until valid time is available.

No firmware has been uploaded and no physical acceptance result is claimed. [CLOCK_DESIGN.md](../CLOCK_DESIGN.md) records the review process and actual verification results.
