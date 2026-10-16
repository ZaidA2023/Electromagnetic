# Startup greeting amendment

Status: greeting and leading-hour-zero option implemented, with host tests, independent source review, and both ESP32-C6 builds passing. Firmware upload and physical acceptance remain.

## Problem

The user wants the four-digit clock to show `Hi`, flush the display, and then show `ZAID` at startup. Magnetic segments retain their position after the register commands are cleared, so a visible flush requires RESET commands for any visible segments. The greeting must use the existing single-segment pulse path, keeping OE_N HIGH during every text hold and after every command. Normal Wi-Fi synchronization and time display must resume after the greeting, including when network access is delayed.

## Technical Plan

Use a one-shot greeting in `setup()`, after `outputs.begin()` establishes disabled, zeroed command registers and serial diagnostics start, and before starting Wi-Fi or returning for missing credentials. Run it on every reset regardless of network configuration, provided `ENABLE_COIL_PULSES` is true. Use yielding `delay` calls only after each completed display update, when all command registers are zero and OE_N is HIGH. These are display-retention holds, not longer coil pulses.

```mermaid
flowchart LR
  I[Initialize outputs disabled] --> H[Centered Hi: hold 1000 ms]
  H --> B[Blank all digits: hold 250 ms]
  B --> Z[ZAId: hold 2000 ms]
  Z --> W[Start Wi-Fi and obtain time]
  W --> T[Display current time]
```

Use the established SevSeg seven-segment letter shapes, without adding its library as a dependency. Its font normalizes case: requested `Hi` appears as H and I; uppercase D is approximated by lowercase d. Z has the same shape as 2; I uses the two left vertical segments, distinguishing its placement from the right-sided numeral 1. The hardware has no decimal-point output for a dot over i.

| Glyph | Visible segments | A-through-G mask |
| --- | --- | --- |
| H | B C E F G | `0x76` |
| I | E F | `0x30` |
| Z | A B D E G | `0x5b` |
| A | A B C E F G | `0x77` |
| d | B C D E G | `0x5e` |
| blank | none | `0x00` |

The three target masks in D1-D4 order are `{0x00,0x76,0x30,0x00}`, `{0,0,0,0}`, and `{0x5b,0x77,0x30,0x5e}`. Default holds are respectively 1000, 250, and 2000 ms measured after each completed render. Pulse, coast, and shifting operations add execution time beyond those holds; scheduling can stretch software delays, so no hard maximum wall-clock duration is promised. The first greeting render commands all 28 initially unknown segments. The blank render resets the seven visible H/I segments; all other segments are already known hidden. The name render then sets its 18 visible segments.

The word "flush" therefore means a physically blank four-digit display. The existing register clear still occurs after every segment command, as does nominal 1000 us pulsing and at least 1 ms disabled coast time. Use the same `DisplayController` instance for the greeting and normal time so cached segment state stays accurate; the first clock render changes only the segments needed to replace the name.

The greeting deliberately supersedes two earlier startup rules: unconfigured/offline boot now shows the greeting instead of holding the old physical display, and the first complete 28-segment initialization occurs for Hi rather than for network time. After the final 2-second name hold, leave ZAId visible until valid time is available; no further pulses occur without a valid target. With an empty SSID the name stays visible and the configuration diagnostic is printed. With the pulse switch false, skip all greeting targets and holds, keep outputs disabled, and retain the existing network/time servicing behavior without commanding segments.

Start Wi-Fi only after the greeting, so time polling cannot overwrite any greeting stage and there is no asynchronous display ownership. Seed the existing reconnect/time-poll timestamps after the actual `WiFi.begin`, preserving a full first retry interval. This adds approximately 3.25 seconds plus command execution to the start of networking. Greeting timing is a deliberate startup-only delay; the subsequent Wi-Fi retry and clock loop keep their current behavior.

Glyph reference: [SevSeg's character map and case handling](https://github.com/DeanIsMe/SevSeg/blob/master/SevSeg.cpp). This design uses only those conventional glyph values and introduces no dependency. Pulse sequencing and the full physical mapping remain specified by [CLOCK_DESIGN.md](CLOCK_DESIGN.md) and implemented in `include/clock_display.h`.

## Alternatives rejected

- **Flush only the shift registers.** Zeroed command registers do not reset a magnetically retained display. Render a blank target through the normal segment controller.
- **Hold a coil enabled for the word's display duration.** The display retains its state after each short command; the long holds must occur with OE_N HIGH and registers cleared.
- **Run time updates during the greeting.** Keeping the one-shot greeting before networking makes all three stages visible without a second state machine or competing display writes.
- **Add an entire alphabet or a display library.** Five fixed glyphs and three target masks satisfy the request with the existing driver.
- **Repeat the greeting on reconnect.** The request is for startup; reconnects continue normal clock servicing.

## Detailed Implementation

| File | Planned change |
| --- | --- |
| `include/clock_display.h` | Add named fixed masks for centered Hi and ZAId, named hold constants, and a templated `showStartupGreeting(DisplayController&, Backend&, bool pulsesEnabled)` after the existing controller class. Render Hi, blank, name through `apply`; after each successful render call `Backend::waitDisplay(uint32_t milliseconds)` with the selected hold. Return false immediately after a rejected render, without further holds or targets. Preserve `pulseCommand`, the command map, and normal time rendering. |
| `src/main.cpp` | Add `RegisterOutputs::waitDisplay(uint32_t)` using yielding `delay`. Call the greeting once in setup after output/serial initialization and the pulse-lockout diagnostic, before the empty-SSID early return or Wi-Fi initialization. Skip the call entirely if pulses are disabled. Report any unexpected rejection while outputs remain disabled. Correct comments that say the first time render establishes all 28 segments. |
| `test/clock_display_test.cpp` | Add independent expected glyph/target checks and greeting-through-driver coverage, without an Arduino runtime. Verify actual command addresses reconstruct Hi, a fully blank display, and ZAId in order, with completed command counts 28/7/18, holds 1000/250/2000 ms, cleared command registers, and disabled outputs at each hold. Verify the subsequent first clock render sends exactly the differences from ZAId, and disabled greeting sends no pulse or hold and does not establish known state. Keep all existing checks. |
| `CLOCK_DESIGN.md` | Record this amendment and its approval; revise the earlier offline/unconfigured startup, first-known-state, and network-start timing statements to match the new request. Preserve the physical pulse contract and prior verification history as history. |
| `README.md`, `src/README.md`, `include/README.md`, `test/README.md` | Describe the greeting, fixed glyph convention, holds, physical blanking, name retention without valid time, startup-before-Wi-Fi order, and current verification results. Do not restore the user's unrelated deletion of `test/README`. |
| `STARTUP_DESIGN.md` | Persist this amendment, the independent review results, human approval, and implementation/verification state. |

The GPIO preload that prevents an OE_N startup glitch stays in place. All greeting renders use the existing one-command-at-a-time implementation, with immediate OE_N HIGH and a seven-byte zero flush after every nominal 1 ms pulse. Holds happen only after `apply` returns with outputs disabled and no stored command. The greeting shares the existing display-state cache with the clock; no reset of that cache occurs between the name and the first time target.

For a host fake backend, add a greeting-specific `waitDisplay` observer that verifies disabled outputs and zero command latches and records the requested hold together with the visible segment state reconstructed independently from the schematic command log. This makes a mistaken register-only blank, wrong stage order, held drive command, or lost cache observable. Do not merely compare the new constants with themselves.

After approval, run the complete strict C++11 host suite and the pinned ESP32-C6 build. Use a temporary project copy with dummy credentials to ensure the configured Wi-Fi/time branch links; never inspect or overwrite the user's local private configuration. No upload is part of this change. Physical confirmation of letter legibility, actual pulse timing, and the greeting-to-time transition remains a hardware acceptance step.

Follow the existing workflow: fresh-context comprehension review, critic review until only trivial points remain, then fresh-context implementation-readiness review. Ask for human approval after recording their results and implement only after it arrives. This is a bounded behavior change, not the workflow's typo/one-line exception.

Review results:

- Local design cross-check: all glyph masks use A-G only; Hi has seven visible segments, the physical blank requires seven RESET commands, and ZAId requires 18 SET commands from blank.
- Fresh-context comprehension review: PASS. The reviewer reconstructed the glyphs, sequence, independent 28/7/18 command counts, disabled holds, blanking, state handoff, and offline behavior. Clarified its one nonblocking wording observation: command count is bounded, but software delays do not guarantee a maximum wall-clock duration.
- Fresh-context critic review: no actionable findings. The reviewer found the sequence, masks, physical blanking, pulse/hold safety, state cache, offline/disabled behavior, and tests consistent with the existing implementation.
- Fresh-context implementation-readiness review: READY. No missing implementation decisions; the reviewer confirmed glyphs, holds, physical blanking, safe pulse reuse, cache handoff, offline/disabled behavior, and meaningful test/build coverage.

Human authorization to implement: on 2026-10-16, after the design reviews, the user renewed the request: "the text with Hi and ZAID did not show up, it should show up before the wifi connects and linger at each word for a little bit, like 1-2 seconds". Implement the reviewed defaults of 1 second for Hi and 2 seconds for ZAId, with the 250 ms visible blank between them.

Implementation status: greeting and leading-hour-zero changes are implemented and verified in software. The existing GPIO preload and pulse shutdown sequence are preserved.

## Implementation verification

- The firmware calls the greeting before both `WiFi.mode`/`WiFi.begin` and the missing-SSID early return. The shared controller retains its state into the clock loop. Text holds yield with outputs disabled and command registers cleared.
- Independent mean source review: no substantive findings. The reviewer confirmed stage order/timings, physical blanking, safe holds, the shared cache, disabled behavior, glyph masks, leading-zero output/serial consistency, and the required comment rule.
- The pinned ESP32-C6 project build succeeds with networking enabled: 42,284 bytes RAM and 956,822 bytes flash.
- A separate temporary build with no private configuration also succeeds: 13,852 bytes RAM and 245,274 bytes flash. This checks that the greeting is included before unconfigured startup returns, without reading or modifying private network settings.
- Expanded host tests: strict C++11 compilation with `-Wall -Wextra -Werror -pedantic` passes; all 16 groups pass. Two new groups reconstruct held letters from independent schematic command logs, verify 28/7/18 commands and 1000/250/2000 ms safe holds, check the name-to-time cache handoff in both leading-zero modes, and ensure disabled greeting leaves positions unknown without pulses or holds. Existing full-day rendering checks cover both padding modes and the explicit `1:00`/`01:00` example.
- Parent review found the added tests meaningful and consistent with the contract; no substantive findings. Final `git diff --check` passes. Private Wi-Fi settings were not inspected or modified.
- No firmware has been uploaded. Electrical timing, mechanical motion, and glyph legibility still require checking on the actual clock.
