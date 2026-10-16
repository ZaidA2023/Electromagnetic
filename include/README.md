# Project headers

PlatformIO makes this directory available to the firmware's include search.

- `clock_display.h` is the Arduino-independent implementation of 12-hour rendering, optional leading hour zero, the complete command mapping, frame serialization, pulse sequencing, retained segment state, and the Hi/blank/ZAId startup greeting. Named `STARTUP_*_HOLD_MS` constants set the text/blank holds; the backend's `waitDisplay` yields only with outputs disabled. Firmware and host tests use the same logic. It allocates no dynamic memory.
- `clock_config.example.h` contains empty, buildable Wi-Fi defaults and the selected U.S. Central daylight saving rule.
- `clock_config.h` is an optional, Git-ignored local copy for the actual network credentials. The implementation selects it when present and falls back to the example otherwise. Never commit credentials or overwrite an existing private configuration.

The hardware mapping and backend contract are specified in [CLOCK_DESIGN.md](../CLOCK_DESIGN.md).
