#include <Arduino.h>

// 74HC595 connections, matching the schematic net names.
constexpr uint8_t DATA_PIN   = D8;   // SER_DATA -> DS, pin 14
constexpr uint8_t CLOCK_PIN  = D10;  // SR_CLK -> SHCP, pin 11
constexpr uint8_t LATCH_PIN  = D9;   // SR_LATCH -> STCP, pin 12
constexpr uint8_t ENABLE_PIN = D7;   // DRV_OE_N -> /OE, pin 13 (active low)
constexpr uint8_t SHIFT_REGISTER_COUNT = 7;

constexpr uint8_t HBRIDGE_A = 0b00000001;  // Q0: least-significant bit
constexpr uint8_t HBRIDGE_B = 0b00000010;  // Q1: second LSB

// Safety lockout: leave false until the disconnected H-bridge outputs have
// been verified to remain off with a meter or oscilloscope.
constexpr bool ENABLE_COIL_PULSES = true;

// Alfa-Zeta specifies a 1 ms coil pulse. Never hold either drive mask on.
constexpr uint32_t PULSE_TIME_US     = 1000;
constexpr uint32_t FLIP_INTERVAL_MS  = 10000;
// Break-before-make coast interval for the DRV8876:
//   74HC595 latch-to-output: <=265 ns at its slowest rated conditions
//   DRV8876 input-to-output: 650 ns typical
//   DRV8876 output fall:     150 ns typical
// The DRV8876 also inserts 300 ns typical internal MOSFET dead time. TI does
// not specify maxima for its three timing values, so retain a 5 us margin.
constexpr uint32_t DEAD_TIME_MS = 1;

// Shift a complete byte, then expose it at Q0-Q7 on the latch rising edge.
void writeShiftRegister(uint8_t value) {
  digitalWrite(LATCH_PIN, LOW);
  shiftOut(DATA_PIN, CLOCK_PIN, LSBFIRST, value);
  digitalWrite(LATCH_PIN, HIGH);
}

// Force every output off. Keep /OE disabled throughout the idle interval.
void flushOutputs() {
  // Remove the drive command immediately, before modifying the register.
  digitalWrite(ENABLE_PIN, HIGH);

  // Clear every 74HC595 in the seven-device daisy chain, then update all
  // parallel outputs together with a single latch edge.
  digitalWrite(LATCH_PIN, LOW);
  for (uint8_t i = 0; i < SHIFT_REGISTER_COUNT; ++i) {
    shiftOut(DATA_PIN, CLOCK_PIN, LSBFIRST, 0x00);
  }
  digitalWrite(LATCH_PIN, HIGH);

  delay(DEAD_TIME_MS);
}

// Apply one short coil pulse, switch all outputs off, then wait until the
// next 10-second flip interval. The display retains its state magnetically.
void pulseAndWait(uint8_t value) {
  const uint32_t pulseStartedAt = millis();

  // Load the command while the shift-register outputs are still disabled.
  writeShiftRegister(value);
  digitalWrite(ENABLE_PIN, LOW);
  delayMicroseconds(PULSE_TIME_US);
  flushOutputs();

  while (millis() - pulseStartedAt < FLIP_INTERVAL_MS) {
    delay(1);
  }
}

void setup() {
  // Disable the 74HC595 outputs while its internal state is being initialized.
  // A physical pull-up on /OE also keeps the H-bridge off during ESP32 reset.
  digitalWrite(ENABLE_PIN, HIGH);
  pinMode(ENABLE_PIN, OUTPUT);

  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);

  // Establish known idle levels before the first latch operation.
  digitalWrite(DATA_PIN, LOW);
  digitalWrite(CLOCK_PIN, LOW);
  digitalWrite(LATCH_PIN, LOW);

  flushOutputs();
}

void loop() {
  if (!ENABLE_COIL_PULSES) {
    flushOutputs();
    delay(1000);
    return;
  }

  pulseAndWait(HBRIDGE_A);
  pulseAndWait(HBRIDGE_B);
}
