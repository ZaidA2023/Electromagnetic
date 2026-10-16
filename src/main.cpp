#include <Arduino.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <time.h>

#include "clock_display.h"

// Credentials stay in an ignored local header; the empty example also builds.
#if __has_include("clock_config.h")
#include "clock_config.h"
#else
#include "clock_config.example.h"
#endif

namespace {

// true: 01:00; false: blank first digit for 1:00. Rebuild after changing.
constexpr bool SHOW_LEADING_HOUR_ZERO = true;

// Existing XIAO pin connections, matching the schematic net names.
constexpr uint8_t DATA_PIN = D8;    // SER_DATA -> DS, pin 14
constexpr uint8_t CLOCK_PIN = D10;  // SR_CLK -> SHCP, pin 11
constexpr uint8_t LATCH_PIN = D9;   // SR_LATCH -> STCP, pin 12
constexpr uint8_t ENABLE_PIN = D7;  // DRV_OE_N -> /OE, pin 13 (active low)

// Preserve the user-tested pulse setting; false keeps all commands disabled.
constexpr bool ENABLE_COIL_PULSES = true;
constexpr uint32_t PULSE_TIME_US = 1000;
constexpr uint32_t DEAD_TIME_MS = 1;
constexpr uint32_t WIFI_RETRY_MS = 30000;
constexpr uint32_t TIME_POLL_MS = 1000;

class RegisterOutputs {
 public:
  void begin() {
    // Arduino core 3 ignores digitalWrite before pinMode. Preload the GPIO
    // latch directly so enabling output mode never drives OE_N LOW at startup.
    gpio_set_level(static_cast<gpio_num_t>(ENABLE_PIN), HIGH);
    pinMode(ENABLE_PIN, OUTPUT);
    disableOutputs();
    pinMode(DATA_PIN, OUTPUT);
    pinMode(CLOCK_PIN, OUTPUT);
    pinMode(LATCH_PIN, OUTPUT);

    // The physical /OE pull-up covers reset; /OE stays HIGH during this clear.
    digitalWrite(DATA_PIN, LOW);
    digitalWrite(CLOCK_PIN, LOW);
    digitalWrite(LATCH_PIN, LOW);
    writeFrame(clock_display::Frame{});
    waitIdle();
  }

  // HIGH disables the 74HC595 parallel outputs (high impedance).
  void disableOutputs() { digitalWrite(ENABLE_PIN, HIGH); }
  void enableOutputs() { digitalWrite(ENABLE_PIN, LOW); }

  void writeFrame(const clock_display::Frame &frame) {
    // Callers disable /OE first. QA is bit 0; register 7 is transmitted first.
    digitalWrite(LATCH_PIN, LOW);
    for (std::size_t i = 0; i < clock_display::REGISTER_COUNT; ++i) {
      shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, clock_display::wireByte(frame, i));
    }
    digitalWrite(LATCH_PIN, HIGH);
  }

  void waitLatchSettle() { delayMicroseconds(1); }
  void waitPulse() { delayMicroseconds(PULSE_TIME_US); }

  // Magnetic segments hold the text while /OE is HIGH and all commands are zero.
  void waitDisplay(uint32_t milliseconds) { delay(milliseconds); }

  void waitIdle() {
    // Guarantee a full coast interval even if delay(1) straddles a tick edge.
    // Yield only after the shared pulse routine has set /OE HIGH and cleared.
    delayMicroseconds(DEAD_TIME_MS * 1000);
    delay(1);
  }
};

RegisterOutputs outputs;
clock_display::DisplayController display;

// Only loop() accesses these or drives registers; Wi-Fi owns no pin callbacks.
const bool NETWORK_CONFIGURED = clock_config::WIFI_SSID[0] != '\0';
bool wasConnected = false;
bool ntpStarted = false;
bool hasValidTime = false;
bool waitingForTime = false;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastTimePollAt = 0;

void serviceWifi(uint32_t now) {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    // Every diagnostic happens outside the enabled coil-pulse interval.
    Serial.println(connected ? "Wi-Fi connected." : "Wi-Fi disconnected; retrying in background.");
    wasConnected = connected;
  }

  if (connected && !ntpStarted) {
    // Start the core's periodic NTP service once; leave it running on outages.
    configTzTime(clock_config::TIME_ZONE, "pool.ntp.org", "time.nist.gov");
    ntpStarted = true;
    waitingForTime = true;
    Serial.println("Waiting for network time; coil outputs remain disabled.");
  }

  // Wrap-safe timing limits our explicit reconnect calls to once per 30 seconds.
  // The core can also retry once internally while establishing a connection.
  if (!connected && static_cast<uint32_t>(now - lastWifiAttemptAt) >= WIFI_RETRY_MS) {
    lastWifiAttemptAt = now;
    WiFi.reconnect();
    Serial.println("Retrying Wi-Fi connection.");
  }
}

void serviceTime(uint32_t now) {
  // Keep time moving through Wi-Fi outages after the first synchronization.
  if (!ntpStarted || static_cast<uint32_t>(now - lastTimePollAt) < TIME_POLL_MS) {
    return;
  }
  lastTimePollAt = now;
  struct tm localTime {};

  // A zero timeout avoids the core's default multi-second NTP wait.
  if (!getLocalTime(&localTime, 0)) {
    if (!waitingForTime) {
      Serial.println("Waiting for valid time; holding the display position.");
      waitingForTime = true;
    }
    return;
  }

  // The startup greeting establishes segment state; the clock reuses that cache.
  waitingForTime = false;
  if (!hasValidTime) {
    Serial.println("Local time available (U.S. Central with daylight saving).");
    hasValidTime = true;
  }

  clock_display::DigitMasks target{};
  if (!clock_display::renderTime(localTime.tm_hour, localTime.tm_min, target,
                                 SHOW_LEADING_HOUR_ZERO)) {
    Serial.println("Invalid time fields; coil outputs remain disabled.");
    return;
  }

  // The shared routine immediately disables /OE after each nominal 1 ms pulse.
  const bool completed = display.apply(target, outputs, ENABLE_COIL_PULSES);
  if (!completed || display.lastPulseCount() == 0) {
    return;
  }
  const int hour12 = localTime.tm_hour % 12 == 0 ? 12 : localTime.tm_hour % 12;
  Serial.printf(SHOW_LEADING_HOUR_ZERO ? "Clock: %02d:%02d (%u segment commands).\n"
                                      : "Clock: %2d:%02d (%u segment commands).\n",
                hour12, localTime.tm_min,
                static_cast<unsigned int>(display.lastPulseCount()));
}

}  // namespace

void setup() {
  // Disable and clear outputs before serial initialization or any network work.
  outputs.begin();
  Serial.begin(115200);
  Serial.println("Electromagnetic clock starting; OE_N HIGH (outputs disabled).");
  if (!ENABLE_COIL_PULSES) {
    Serial.println("Coil pulsing is disabled by ENABLE_COIL_PULSES.");
  }

  // Finish every greeting stage before any Wi-Fi setup or missing-SSID return.
  if (ENABLE_COIL_PULSES) {
    Serial.println("Startup greeting: Hi, blank, ZAId.");
    if (!clock_display::showStartupGreeting(display, outputs, ENABLE_COIL_PULSES)) {
      Serial.println("Startup greeting rejected; outputs remain disabled.");
    }
  }

  // Missing credentials are a buildable idle configuration, never a fake network.
  if (!NETWORK_CONFIGURED) {
    Serial.println("Copy include/clock_config.example.h to include/clock_config.h and set Wi-Fi credentials.");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  // Do not wait for a connection; give the first attempt a full retry window.
  WiFi.begin(clock_config::WIFI_SSID, clock_config::WIFI_PASSWORD);
  lastWifiAttemptAt = millis();
  lastTimePollAt = lastWifiAttemptAt;
  Serial.println("Connecting to Wi-Fi in the background.");
}

void loop() {
  if (NETWORK_CONFIGURED) {
    // All network calls are outside the shared, bounded pulse sequence.
    const uint32_t now = millis();
    serviceWifi(now);
    serviceTime(now);
  }
  delay(1);  // /OE stays HIGH while idle, including an unconfigured startup.
}
