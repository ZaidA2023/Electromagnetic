# Electromagnetic clock

This PlatformIO Arduino project targets the Seeed XIAO ESP32-C6. Seven daisy-chained 74HC595 shift registers provide 56 command outputs for four magnetic seven-segment digits, ordered D1 through D4 from left to right. Every segment has a SET command that makes it visible and a RESET command that hides it. The display retains its position after the pulse ends.

The firmware connects to Wi-Fi, synchronizes the ESP32 clock using NTP, and displays 12-hour U.S. Central time with automatic daylight saving changes. Hours 1-9 have a blank first digit; minutes always have two digits. Midnight and noon show 12. There are no mapped AM/PM or colon outputs.

## Wi-Fi configuration

Edit the local `include/clock_config.h` file and fill in `WIFI_SSID` and `WIFI_PASSWORD`. A blank password is supported for an open network. The file is ignored by Git. If it is missing in a new checkout, copy `include/clock_config.example.h` to that filename first; preserve any existing private configuration.

Keep `TIME_ZONE` set to `CST6CDT,M3.2.0,M11.1.0` for the selected Central timezone. The example has empty network credentials so a checkout builds without revealing or inventing credentials. With an empty SSID, outputs stay disabled and the serial monitor explains how to configure the network.

## Coil commands

OE_N is **HIGH while idle** and throughout register loading. Each command loads all 56 bits, enables only one command with OE_N LOW for a nominal 1 ms, immediately returns OE_N HIGH, and clears all seven registers. A full 1 ms coast wait precedes an additional scheduler yield before another pulse. On the first valid time, all 28 segments are commanded to establish their position; subsequent updates pulse only changed segments.

The existing wiring uses D8=data, D10=clock, D9=latch, D7=OE_N. Register frames travel farthest first using MSBFIRST; software bit 0 is QA. The exact SET/RESET table, including the user-confirmed D4 B RESET on Register_07 QD, is in [CLOCK_DESIGN.md](CLOCK_DESIGN.md).

The `ENABLE_COIL_PULSES` constant in `src/main.cpp` preserves the existing enabled test setting; setting it false prevents pulses. Disabling /OE makes the register pins high impedance, as in the working test circuit. Software delays are nominal timing; actual pulse width with Wi-Fi active still needs measurement on the board.

## Startup and network outages

The controller disables outputs and clears the command registers before starting Wi-Fi. It waits for valid time before moving any segments, preserving the physical display position during an offline startup. Wi-Fi connection attempts run in the background with explicit retries every 30 seconds. NTP uses `pool.ntp.org` and `time.nist.gov`; diagnostics use 115200 baud and never print the password.

After synchronization, the system clock continues driving updates during a Wi-Fi outage while power remains on. Long outages can accumulate clock drift. An ordinary power cycle loses time and waits for synchronization again; there is no battery-backed RTC in this design.

## Build and monitor

Use the environment `seeed_xiao_esp32c6` in PlatformIO. The project pins Seeed's Arduino-capable platform revision and its compatible esptool package. The physical target is unchanged: XIAO ESP32-C6, Arduino core 3.3.7.

```sh
pio run -e seeed_xiao_esp32c6
pio device monitor -b 115200
```

On this computer PlatformIO is available at `/Users/azaid/.platformio/penv/bin/platformio` if `pio` is not on PATH. Building does not upload to the board. Upload and physical operation should follow the hardware checks in [test/README.md](test/README.md).

## Repository contents

- [src](src/README.md): hardware output adapter and Wi-Fi/time controller.
- [include](include/README.md): display logic, mapping, and configuration convention.
- [lib](lib/README.md): currently no project libraries.
- [test](test/README.md): host logic tests and hardware acceptance checks.
- [.vscode](.vscode/README.md): editor extension recommendations.
- `platformio.ini`: pinned Seeed platform, board `seeed-xiao-esp32-c6`, Arduino framework.

## Design workflow

The user approved the design and baseline READMEs and authorized implementation after fresh-context comprehension, critic, and readiness reviews. Follow [CLOCK_DESIGN.md](CLOCK_DESIGN.md) for implementation contracts, review outcomes, and verification status. The referenced `elephant-goldfish-paper.md` is not present in this repository; the workflow instructions provided in the conversation govern the work. No firmware upload has been performed.
