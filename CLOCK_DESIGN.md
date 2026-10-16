# Wi-Fi time for the electromagnetic clock

Status: approved clock design implemented, with the user-requested startup greeting and optional leading hour zero added. See STARTUP_DESIGN.md for the greeting amendment; current verification is recorded below. Physical acceptance remains.

## Problem

The existing ESP32-C6 firmware proves that a nominal 1 ms command pulse can flip a magnetic segment, but it alternates two outputs rather than displaying time. The completed clock must obtain time over Wi-Fi and show four digits in 12-hour format using U.S. Central time with automatic daylight saving changes. Seven daisy-chained 74HC595 devices provide 56 outputs, with separate visible/hidden commands for each of the 28 segments. The controller must address the actual wiring, establish the display's unknown state after startup, and turn off drive commands after every pulse.

## Technical Plan

### Confirmed behavior and scope

- Board: Seeed XIAO ESP32-C6 using Arduino. Preserve the environment name; use Seeed's supported C6 platform/board recipe described below to repair the local build configuration.
- Pins remain D8=data, D10=shift clock, D9=latch, D7=active-low output enable.
- D1/D2 display hours and D3/D4 display minutes, ordered left to right.
- Standard seven-segment positions: A=top, B=upper right, C=lower right, D=bottom, E=lower left, F=upper left, G=middle.
- SET makes a segment visible; RESET hides it. The user confirms the existing nominal 1 ms test works.
- Use 12-hour time. Default: blank D1 for hours 1-9; `SHOW_LEADING_HOUR_ZERO` near the top of `src/main.cpp` optionally shows the zero glyph instead. Midnight and noon both show `12:MM` in either mode. There are no mapped AM/PM or colon-control outputs; all 56 outputs are assigned to segments.
- Before Wi-Fi setup, show centered Hi for 1000 ms, physically blank for 250 ms, then ZAId for 2000 ms. This user-confirmed amendment is detailed in [STARTUP_DESIGN.md](STARTUP_DESIGN.md). Skip the greeting and all its holds if coil pulsing is disabled.
- Use the POSIX rule `CST6CDT,M3.2.0,M11.1.0`, matching U.S. Central time with automatic daylight saving changes, as explicitly selected by the user.
- Never activate multiple segment commands together. Preserve a nominal 1000 us pulse and at least 1 ms of disabled time between pulses.
- This work adds firmware and configuration instructions. Uploading to a connected clock is a separate action.

### Architecture

```mermaid
flowchart LR
  S[Hi then blank then ZAId] --> W
  W[Wi-Fi connection] --> N[NTP synchronization]
  N --> T[ESP32 system clock]
  T --> Z[Central local time]
  Z --> D[Four digit segment masks]
  D --> C[Compare with last commanded segments]
  C --> P[One segment pulse]
  P --> R[Seven-byte register frame]
  R --> H[Seven registers and segment drivers]
```

Use the Arduino ESP32 core's `WiFi` and `configTzTime` facilities, with `pool.ntp.org` and `time.nist.gov` as NTP servers. The core manages ongoing synchronization; the application does not implement NTP packets or add a library. Read the current local time without a multi-second wait once per second, render one snapshot, and compare its four masks with the last commanded state. Comparing actual masks handles both forward and backward time corrections, including daylight saving changes, without a fragile minute counter.

Keep pure formatting, the physical mapping, frame construction, and the pulse/display sequencing in one Arduino-independent header. A small backend in `src/main.cpp` supplies pin writes and waits. This lets host tests observe the real sequencing policy without energizing hardware or mocking the entire ESP32 framework.

### Physical output numbering and mapping

Number registers 1-7 along the serial chain: Register_01/U33 is connected directly to SER_DATA; Register_07/U31 is farthest away. Within a register, byte bit 0 represents QA and bit 7 represents QH. Define global output number as `8 * (register_number - 1) + output_index`, with QA index 0 through QH index 7. These are new explicit software addresses, not the numbers used by the existing LSBFIRST test masks.

The following is a complete transcription of the supplied schematic labels. For Register_07 QD, use `D4_B_RESET_CMD`: the user explicitly corrected the initial image reading and confirmed this label. SET/RESET pairs do not have a uniform order, so the implementation must use the table rather than assume odd/even polarity.

| Register / chip | QA / bit 0 | QB / bit 1 | QC / bit 2 | QD / bit 3 | QE / bit 4 | QF / bit 5 | QG / bit 6 | QH / bit 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 / U33 | D1 G RESET | D1 G SET | D1 D RESET | D1 D SET | D1 F RESET | D1 F SET | D1 E RESET | D1 E SET |
| 2 / U32 | D2 E RESET | D2 E SET | D1 A SET | D1 A RESET | D1 B SET | D1 B RESET | D1 C RESET | D1 C SET |
| 3 / U29 | D2 C RESET | D2 C SET | D2 G RESET | D2 G SET | D2 D RESET | D2 D SET | D2 F RESET | D2 F SET |
| 4 / U34 | D3 D RESET | D3 D SET | D3 E RESET | D3 E SET | D2 A SET | D2 A RESET | D2 B SET | D2 B RESET |
| 5 / U35 | D3 B SET | D3 B RESET | D3 C RESET | D3 C SET | D3 G RESET | D3 G SET | D3 F RESET | D3 F SET |
| 6 / U30 | D4 D RESET | D4 D SET | D4 F RESET | D4 F SET | D4 E RESET | D4 E SET | D3 A RESET | D3 A SET |
| 7 / U31 | D4 A SET | D4 A RESET | D4 B SET | D4 B RESET | D4 C RESET | D4 C SET | D4 G RESET | D4 G SET |

Equivalent global addresses, each cell written `SET / RESET`:

| Segment | D1 | D2 | D3 | D4 |
| --- | --- | --- | --- | --- |
| A | 10 / 11 | 28 / 29 | 47 / 46 | 48 / 49 |
| B | 12 / 13 | 30 / 31 | 32 / 33 | 50 / 51 |
| C | 15 / 14 | 17 / 16 | 35 / 34 | 53 / 52 |
| D | 3 / 2 | 21 / 20 | 25 / 24 | 41 / 40 |
| E | 7 / 6 | 9 / 8 | 27 / 26 | 45 / 44 |
| F | 5 / 4 | 23 / 22 | 39 / 38 | 43 / 42 |
| G | 1 / 0 | 19 / 18 | 37 / 36 | 55 / 54 |

Write every command as exactly seven bytes while output enable is HIGH. Send the farthest register's byte first (7,6,5,4,3,2,1), each with `MSBFIRST`, and latch once after all 56 clocks. This makes stored bit 0 physically reach QA. It intentionally replaces the old LSBFIRST representation; keeping LSBFIRST with the new QA-based addresses would reverse every byte.

For example, D1 E SET is global bit 7: frame byte 0 is `0x80` and the other six bytes are zero. D4 B RESET is global bit 51: frame byte 6 is `0x08` and the other six bytes are zero. A software simulation of seven cascaded registers must confirm both, and all other single-output commands.

### Pulse and retained display state

Each pulse uses this exact order:

1. Set output enable HIGH (disabled).
2. Shift and latch a zero-initialized seven-byte frame with exactly one command bit set.
3. Wait 1 us after latching, while outputs remain disabled.
4. Set output enable LOW; wait a nominal 1000 us using the existing microsecond delay mechanism.
5. Immediately set output enable HIGH, before any logging, network work, or further frame manipulation.
6. Shift and latch seven zero bytes, still disabled.
7. Wait at least 1 ms with outputs disabled before another pulse.

The main task alone owns register writes. No network callback may drive a pin. Disabling /OE makes the register outputs high impedance; it does not itself drive them LOW. This preserves the existing user-tested driver idle arrangement and the shown 10 kOhm /OE pull-up. Loading zero bytes also clears the stored commands before the next pulse. The design does not claim to replace missing driver input biasing or provide a hardware-enforced maximum pulse duration.

Retain the existing `ENABLE_COIL_PULSES` switch, enabled as in the tested firmware. If it is false, keep outputs disabled and do not mark any segment as successfully commanded. The pulse primitive rejects out-of-range command addresses without enabling outputs.

Store both last-commanded masks and known-state masks, each four bytes with bits A-G in positions 0-6. At startup all segments are unknown. The first Hi greeting commands every segment once to establish visible and hidden positions. Blanking, the name, and the clock reuse the same controller/cache and command only changed or still-unknown segments. After each completed pulse, mark only that segment known and update only its commanded bit. State records commands sent, not sensor-confirmed mechanical feedback; this hardware has no feedback channel.

The display update uses a local snapshot of the target masks, visits D1-D4 and A-G in that order, and finishes the bounded sequence with all outputs disabled. A new time snapshot is handled on the next scheduled poll. No long delay remains from the ten-second test loop.

### Startup, connectivity, and private configuration

Initialize /OE HIGH and configure pins before Wi-Fi setup, then load and latch seven zero bytes. Start serial diagnostics at 115200 baud while outputs are disabled. If a private `include/clock_config.h` exists, include it; otherwise include the committed `include/clock_config.example.h`. Both use the same `clock_config` namespace with `WIFI_SSID`, `WIFI_PASSWORD`, and `TIME_ZONE` string constants. The example has an empty SSID/password and the confirmed Central timezone rule. Ignore only the private header in Git; never print the password.

Run the enabled greeting after diagnostics and before checking credentials or starting Wi-Fi. `showStartupGreeting` applies Hi, a blank target, and ZAId, with yielding holds of 1000/250/2000 ms after their completed renders. Registers are zeroed and /OE is HIGH during every hold; no hold extends coil energization. This runs once per reset and never on reconnection. It adds the three holds plus command execution to the start of networking.

An empty SSID means unconfigured: after the greeting, report the required local configuration once, leave the name visible with outputs disabled, and do not attempt a connection or further pulse. An empty password with a nonempty SSID is allowed for an open network. With pulse lockout, skip the greeting/holds entirely and retain the old physical display. A local private header may be populated by the user; no credentials are required in the conversation or committed files.

With credentials present, select station mode and start `WiFi.begin` without a connection-wait loop. On the first observed connected state, call `configTzTime` once with the configured rule and two servers. Once initialized, leave the SNTP service running through disconnects and reconnections. Poll connection state from `loop`; request reconnection at most once every 30 seconds while disconnected, using wrap-safe elapsed-millis arithmetic. No automatic reconnect feature is also enabled by application code, to avoid two competing retry policies.

Use `getLocalTime(&timeinfo, 0)` at a wrap-safe 1000 ms polling interval. With this API, zero is the timeout: the firmware must not use its default multi-second wait. Before a successful read, retain the greeting's name with outputs disabled; the first valid clock target replaces it through the same state cache. There is no clock guess such as 00:00. Once synchronized, the ESP32 system clock continues advancing during a Wi-Fi outage while powered; continue rendering that local time and reconnect in the background. Drift during prolonged outages and loss of time on an ordinary power cycle remain limitations without a battery-backed RTC.

Report configuration status, connection state changes, waiting for the first valid time, and completed display updates while /OE is HIGH. Avoid a diagnostic line on every loop. All waits of significant duration outside the 1 ms pulse occur with outputs disabled and yield to the ESP32 framework.

### Evidence

- Existing behavior: `src/main.cpp` and `platformio.ini`; user confirms four digits, standard positions, visible SET, left-to-right numbering, and successful 1 ms hardware pulses.
- Schematic transcription: user attachments `IMG_0589.png` (registers 1-2), `IMG_5017.png` (3-4), `IMG_4792.png` (5-6), `IMG_3705.png` (7), plus the user's explicit correction of Register_07 QD to D4 B RESET. The two mapping tables above are the durable implementation reference.
- [TI SN74HC595 datasheet](https://www.ti.com/lit/ds/symlink/sn74hc595.pdf): serial chain, storage latch, output naming, and high-impedance /OE behavior.
- [Espressif shiftOut implementation](https://raw.githubusercontent.com/espressif/arduino-esp32/master/cores/esp32/wiring_shift.c): emitted bit order.
- [Espressif SimpleTime example](https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Time/SimpleTime/SimpleTime.ino): Wi-Fi and timezone-aware network time facilities.
- [Arduino timezone definitions](https://raw.githubusercontent.com/esp8266/Arduino/master/cores/esp8266/TZ.h): the America/Chicago POSIX rule.
- [Seeed's XIAO ESP32C6 PlatformIO instructions](https://wiki.seeedstudio.com/xiao_esp32c6_with_platform_io/) and [vendor board manifest](https://raw.githubusercontent.com/Seeed-Studio/platform-seeedboards/main/boards/seeed-xiao-esp32-c6.json): an Arduino-capable platform and the `XIAO_ESP32C6` variant.

## Alternatives rejected

- **Keep sending one byte per command.** This does not explicitly replace the entire seven-register state. A complete frame makes the addressed output and every inactive output deterministic.
- **Derive commands from adjacent-bit polarity.** The schematic changes SET/RESET ordering for several segments; a direct table avoids reversing coils.
- **Keep LSBFIRST with QA as bit 0.** This reverses the intended physical address within each byte. Preserving the old test representation would also work if every frame byte were explicitly reversed, but adds needless translation.
- **Pulse all changed segments at once.** The existing hardware test establishes single-segment operation; simultaneous coil current has not been established. Sequential pulses preserve the tested operating pattern.
- **Refresh every segment every minute, or RESET everything before rendering.** Magnetic retention allows changed-segment updates. A first render already commands each unknown segment to its target, avoiding a redundant full-clear cycle.
- **Use a fixed UTC-6 offset.** The user chose automatic U.S. daylight saving changes. A POSIX rule expresses both offsets and transition dates.
- **Block until Wi-Fi or NTP is available.** Network failures should leave the outputs idle and allow the controller to recover. Once time is known, a network outage must not freeze display updates.
- **Hardcode Wi-Fi credentials in the firmware source.** An ignored local configuration header allows builds and review without committing a password.
- **Add a web setup portal, RTC hardware, AM/PM output, or a new timing peripheral.** These require additional user experience or hardware decisions. The requested clock can use the existing framework and tested pulse mechanism; Wi-Fi pulse timing still needs measurement on the physical board.
- **Keep the installed Espressif32 6.5.0 platform for the device build.** Its installed board definitions do not include `seeed_xiao_esp32c6`. Seeed documents its own platform and an Arduino-capable C6 manifest, so use that explicit recipe while preserving the physical board and pin aliases.

## Detailed Implementation

### Files created or changed

| File | Change and reason |
| --- | --- |
| `src/main.cpp` | Replace the alternating pulse demo with early idle initialization, the hardware backend, private configuration selection, nonblocking Wi-Fi/NTP servicing, local-time polling, and display updates. Keep the existing pins, pulse switch, nominal 1000 us timing, and 1 ms idle interval. Correct obsolete LSBFIRST/Q0 comments and remove the ten-second test interval. |
| `platformio.ini` | Keep environment `seeed_xiao_esp32c6` and framework `arduino`; use Seeed's platform URL and board identifier specified below. Set serial monitor speed to 115200. This fixes the locally missing C6 board support without changing the physical target. |
| `include/clock_display.h` | Add Arduino-independent segment masks, command addresses, seven-byte frame helpers, 12-hour rendering, pulse sequencing, and retained command-state tracking. This is the single logic implementation used by firmware and host tests. |
| `include/clock_config.example.h` | Add buildable empty network defaults and the confirmed Central timezone. Document copying this file to `clock_config.h` locally. |
| `include/clock_config.h` | User-owned optional local credentials, ignored by Git. Never synthesize a real SSID/password or overwrite an existing private header. |
| `.gitignore` | Ignore `/include/clock_config.h` explicitly. |
| `test/clock_display_test.cpp` | Add host tests using a fake pulse backend and a serial-chain simulator. These tests exercise the shared logic without a connected board. |
| `src/README.md` | Bootstrap the source directory documentation now; update it after implementation to describe the actual clock loop and backend. |
| `include/README.md` | Bootstrap current contents now; later document the shared logic and private configuration. |
| `lib/README.md` | Bootstrap the empty library directory; record that no extra libraries are needed. |
| `test/README.md` | Bootstrap current test status now; later provide the exact host-test command and hardware acceptance procedure. |
| `.vscode/README.md` | Bootstrap editor configuration documentation. No editor-setting changes. |
| `README.md` | Bootstrap the project summary now; after implementation add configuration, build, serial diagnostics, startup, time behavior, and limitations. |
| `CLOCK_DESIGN.md` | Persist this design, independent review findings, resolved decisions, and human approval state so a future session can resume from it. |

### Build environment

Read-only inspection found PlatformIO Core 6.2.0 at `/Users/azaid/.platformio/penv/bin/platformio`, outside PATH. The installed `/Users/azaid/.platformio/platforms/espressif32/platform.json` reports platform version 6.5.0; its `boards` directory has no `seeed_xiao_esp32c6.json`. No device build has been attempted during the design phase. The existing hardware test's successful operation is accepted independently of this local installation.

After approval, change the platform to `https://github.com/Seeed-Studio/platform-seeedboards.git` and the board identifier to `seeed-xiao-esp32-c6`, exactly as documented by Seeed. Retain `[env:seeed_xiao_esp32c6]` so the project's environment name is stable, retain `framework = arduino`, and set `monitor_speed = 115200`. The vendor manifest selects variant `XIAO_ESP32C6`, supports Arduino, and enables USB serial. Use the same D7-D10 board aliases; do not substitute raw GPIO numbers or another MCU.

Implementation build fix: the first device build reused the installed esptool 4.5.1 and rejected the vendor builder's esptool 5 command-line options. Explicitly select the vendor manifest's `tool-esptoolpy` archive (`https://github.com/pioarduino/esptool/releases/download/v5.1.2/esptool.zip`) in the project's `platform_packages` to avoid the incompatible cached package. This is a project dependency selection, not a global package edit or a change of board/platform.

Resolve the vendor platform revision during dependency installation, record it together with the Arduino core version in the final verification record, and pin the successfully built platform to that commit in the platform URL before the final build. This makes the resulting checked-in configuration reproducible without guessing a revision from a remote branch listing. Installation can require network/package-cache access. If dependencies cannot be fetched or built, report the precise limitation and preserve the source; do not replace the platform or board again without updating the design. Do not upload firmware as part of a build.

### Shared logic and backend contract

Use fixed-size C++11-compatible types in a `clock_display` namespace, without dynamic allocation or Arduino includes:

- Four digit masks, one byte per digit. A-G occupy bits 0-6. Standard digits 0-9 use masks `0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f`; blank is zero.
- A `SegmentCommand` holds a SET address and a RESET address. The 4 by 7 table must match both transcription tables above. Every address 0-55 must appear exactly once across all entries.
- A `Frame` holds seven bytes in nearest-to-farthest register order. A single-command constructor first zeroes all seven bytes, validates the address, then sets only its addressed bit. Invalid addresses return failure and cannot enable outputs.
- A wire-byte accessor selects frame bytes in reverse register order. The hardware backend writes them with MSBFIRST; tests simulate the corresponding serial bit sequence and latch.
- A time-rendering helper accepts a valid 24-hour hour and minute and produces four target masks. Compute hour12 as `hour24 % 12`, replacing zero with 12. Its optional `showLeadingHourZero` argument defaults to false: D1 is blank below hour 10, or the zero glyph when true. Minutes always have two digits. Invalid hour/minute inputs return failure rather than indexing a digit table.
- A backend-independent pulse routine owns the seven-step sequence specified above. Its backend provides disable, write-frame, latch-settle wait, enable, pulse wait, and idle wait operations. Hardware methods implement the defined nominal waits and frame transfer; fake methods record their calls. No network or diagnostics operation belongs in this routine.
- A small display controller holds four last-commanded masks and four known-state masks, initialized to zero. For each A-G bit that is unknown or differs, choose SET or RESET, run the pulse routine, and update that bit only on success. If pulsing is disabled or any command fails, leave the uncommanded bits unknown and return without enabling more outputs. A completed unchanged render emits no pulses.

The ESP32 hardware backend lives in `src/main.cpp`. `writeFrame` always lowers the latch, shifts exactly seven bytes in the specified order, and raises the latch once. Its caller has already disabled outputs; it must not change output-enable state itself. All initial and final clear frames follow the same full-frame path. The 1 us settle wait is after the latch edge and before enable.

Startup refinement from inspecting Arduino core 3.3.7: its `digitalWrite` ignores a pin until `pinMode` registers it as GPIO. Preload the /OE output latch using `gpio_set_level` from `driver/gpio.h` with the existing D7 alias, configure it as OUTPUT, and immediately call `disableOutputs` again. This satisfies the specified HIGH-before-output-mode startup behavior and avoids relying on a pre-pinMode Arduino write that has no effect. The physical pull-up continues to cover reset.

Select the private header with a compiler-supported include-existence check, falling back to the example. Both define only the three string constants specified above in the same namespace. The committed example must build without being copied. An unconfigured build shows the enabled greeting and then stays idle, as specified; use a clear serial message rather than a compile failure or a fabricated network.

Keep setup and each loop operation small, with comments explaining physical ordering, bit order, state uncertainty, and retry behavior. Under the user's required later code review, flag executable stretches longer than ten lines without a useful comment, as well as substantive correctness issues.

### Main-loop sequence

1. During setup, initialize pins and load the idle frame, then start diagnostics and complete the enabled startup greeting. Only afterward, if configured, select station mode, explicitly disable framework auto-reconnect with `WiFi.setAutoReconnect(false)`, and call `WiFi.begin` once. Seed retry timestamps afterward so the first begin attempt receives its full 30-second window. Unconfigured startup returns only after the greeting; lockout skips all greeting stages/holds.
2. On every loop pass with outputs idle, observe connection changes. Initialize timezone/NTP only on the first connected observation. For a disconnected configured device, retry with `WiFi.reconnect` only when 30 seconds have elapsed since the last explicit attempt; measure intervals using unsigned subtraction.
3. If configured and NTP has been started, at most once per 1000 ms call `getLocalTime(&timeinfo, 0)`. A failed read leaves the physical display and command-state cache alone. A successful read supplies one complete hour/minute snapshot for rendering.
4. Apply that target through the shared display controller. The greeting already initialized all 28 segments; clock updates send only the changes needed to replace the prior target. The pulse switch is checked before commanding or updating known state. Return with /OE HIGH.
5. Emit any pending diagnostics while disabled, and yield briefly with `delay(1)`. Polling and retries never use an indefinite wait loop. If unconfigured, skip network/time/render work but continue yielding with /OE HIGH.

Time may change asynchronously while a target is applied; finish the small bounded update and use the newest time on the next poll. The backend has no failure-reporting mechanism for a missed mechanical flip. A reset discards all known-state cache entries, forcing a complete target render after time is available again.

### Validation after approval

Host tests must cover meaningful failure modes rather than mirror loops:

- Verify the complete expected SET/RESET table, address range, and uniqueness. Check nonuniform polarity cases explicitly: D1 A, D2 B, D3 B, and D4 B RESET at bit 51.
- Simulate the physical 56-stage chain for every one of the 56 command addresses. Confirm exactly the intended physical output is active after seven MSBFIRST bytes sent farthest first. Verify no stale outputs survive a new frame and that a clear frame sets all stored bits to zero.
- Verify digit glyphs and time rendering across all 1440 minute values, including `00:00 -> 12:00`, `09:59 -> blank-9:59`, `10:00`, `12:00`, `12:59 -> 13:00` becoming `12:59 -> blank-1:00` on the display, `23:59`, and invalid inputs. D1's blank mask must be zero.
- Verify first render commands all 28 segments, an identical second render commands none, and a changed render commands exactly the changed segments with the correct SET/RESET addresses. Test a target blank D1, a backward time change, a disabled pulse switch, and rejected addresses. Unknown state must not be marked known when no pulse occurred.
- Have a fake backend assert disable-before-load, complete frame, settle-before-enable, one active command per pulse, disable-immediately-after-pulse-wait, clear-before-idle-wait, and disabled final state. No callback or diagnostic belongs between enable and disable.
- Use the example timezone string with host `tzset`/`localtime_r` to check winter and summer offsets and 2026 spring/fall transition boundaries. This validates the chosen rule on the host; device behavior remains part of hardware acceptance.

Compile and run with the available host compiler, for example `c++ -std=c++11 -Wall -Wextra -Werror -pedantic -Iinclude test/clock_display_test.cpp -o /tmp/electromagnetic-clock-tests` followed by executing that binary. Then use `/Users/azaid/.platformio/penv/bin/platformio run -e seeed_xiao_esp32c6` to build the approved configuration and repeat once after pinning its resolved vendor revision. An unavailable package or toolchain is a reported build limitation, never evidence of a successful device build.

Hardware acceptance requires the actual board and valid local Wi-Fi configuration: inspect serial connection/time status; verify the 56-bit clock/latch sequence and a representative QA/QH/far-end command; measure nominal pulse width and disabled intervals with Wi-Fi active; verify correct digits and SET/RESET direction; observe a minute/hour change; and disconnect Wi-Fi to confirm continued updates and eventual recovery. Software waits can be stretched by framework scheduling, so measurements must not be described as a hardware-enforced pulse-width bound. Do not claim these checks were performed without physical evidence.

### Review and approval gate

The baseline README files were reviewed through the user's approval before implementation and now describe the implemented clock. The independent comprehension test receives only this design and its referenced files; it must reconstruct mapping, time format, startup state, pulse ordering, and offline behavior. A separate critic must identify actionable defects or ambiguities; revise this document and repeat until only trivial observations remain. A fresh readiness reviewer must determine whether an implementer can proceed without guessing and distinguish pending human approval/configuration/hardware checks from missing technical design.

Record the review outcomes here, then request approval of this design and baseline documentation. No firmware or test implementation may begin until that human approval is received, as explicitly required by the user's workflow.

Review record:

- Fresh-context comprehension review: PASS. The reviewer reconstructed physical addressing, both frame examples, retained state, pulse ordering, and network behavior without a blocking ambiguity.
- Fresh-context critic, round 1: only a minor leading-zero notation inconsistency in a validation example. Corrected it to blank D1. No substantive findings.
- After those reviews, local toolchain inspection identified the missing installed C6 board definition. Added the explicit vendor build-environment plan and revision-pinning requirement; final independent review covers this addition.
- Fresh-context critic, round 2: PASS. No substantive contradictions or actionable gaps, including in the vendor build-environment plan.
- Fresh-context implementation-readiness review: READY. The reviewer found no unresolved product, hardware, or architectural decision; mapping, serialization, time handling, state, backend contracts, build recipe, and validation are specified.
- Documentation cross-check: both mapping tables agree, and the 56 SET/RESET addresses use every physical output exactly once. This verifies the transcription's internal consistency, not physical wiring or firmware execution.

Human approval: received on 2026-10-16. The user explicitly instructed: "okay then yes implement the changes" after confirming that OE_N must be disabled after each command.

Execution status: firmware, configuration convention, host tests, and documentation are implemented. Verification and code-review results are recorded below. No device upload has been performed or is authorized by this approval.

## Initial implementation verification

- Host tests: strict C++11 compilation with `-Wall -Wextra -Werror -pedantic` succeeds; all 14 test groups pass. Coverage includes all 56 physical command addresses, seven-register shift simulation, exact pulse-operation ordering, every minute of the day, unknown/known state, disabled and invalid requests, target snapshots, and 2026 Central DST boundaries.
- Device toolchain: PlatformIO Core 6.2.0, Seeed platform commit `024c190fef52da02ebae5c09d47c101e0ff0762a`, Arduino ESP32 3.3.7, esptool 5.1.2. The platform URL is pinned to the resolved and tested commit. No global platform source/package has been manually edited.
- Empty-network firmware: pinned build succeeds after the OE_N startup refinement, using 13,836 bytes RAM and 244,576 bytes flash.
- Configured Wi-Fi path: a temporary project copy with dummy network settings also builds after the startup refinement, using 42,284 bytes RAM and 956,582 bytes flash (73.0% of the configured application partition). This compiles/links the network path without changing local private settings, connecting to a network, or uploading.
- Private configuration: created an empty local copy only because no private header existed; the Git ignore rule is verified. The user must fill in actual Wi-Fi details before building/uploading for operation. The fallback example also compiled successfully before the startup refinement.
- Mean source review, round 1: identified the ineffective pre-pinMode Arduino write as P1. The GPIO-latch preload and post-pinMode HIGH fix resolve it; the reviewer rechecked the latest source and reported no substantive findings or comment-length violations. Review noted that the core may perform one internal initial-connection retry even with automatic reconnection disabled; application-initiated retries remain limited to one per 30 seconds.
- Independent mean review of the final source and tests: no substantive correctness issues. Two P3 observations concerned qualifying application-initiated Wi-Fi retries and adding comments to the fake backend's load/enable/idle invariants; both are corrected. The reviewer confirmed the boot fix against the actual GPIO implementation and found the tests use independent expectations. Repeated review reached only trivial issues, as required by the workflow.
- Whitespace check: `git diff --check` passes. Firmware upload, real Wi-Fi synchronization, electrical timing, and mechanical display acceptance remain unperformed.

### Leading hour zero option

The user requested a top-of-file flag selecting `1:00` versus `01:00`. This is a small formatting amendment handled under the workflow's trivial-change exception. `SHOW_LEADING_HOUR_ZERO` defaults to false and is passed to the existing renderer; serial diagnostics use the same choice. The output mapping and pulse sequencing are unchanged. Existing rendering checks cover all 1440 minutes in both modes, the exact requested example, and invalid input with padding enabled. The expanded 14-group suite and ESP32-C6 build passed before integrating the greeting; verification of the combined changes is recorded in STARTUP_DESIGN.md.

### Current greeting and formatting verification

The combined implementation passes all 16 strict C++11 host groups, an independent mean source review, the configured ESP32-C6 build, and a separate no-credentials build. The greeting completes in setup before Wi-Fi setup or the unconfigured return, and every text hold occurs with outputs disabled. The top-level leading-zero flag controls both the physical time masks and serial formatting. See [STARTUP_DESIGN.md](STARTUP_DESIGN.md) for full results and memory sizes. No firmware has been uploaded; actual display operation and electrical timing remain physical acceptance work.
