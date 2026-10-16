#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace clock_display {

constexpr std::size_t REGISTER_COUNT = 7;
constexpr std::size_t DIGIT_COUNT = 4;
constexpr std::size_t SEGMENT_COUNT = 7;
constexpr std::size_t OUTPUT_COUNT = REGISTER_COUNT * 8;
using Frame = std::array<uint8_t, REGISTER_COUNT>;
using DigitMasks = std::array<uint8_t, DIGIT_COUNT>;

struct SegmentCommand {
  uint8_t set;
  uint8_t reset;
};

// Rows are D1-D4, columns A-G. Each address is 8 * register_index + QA-QH.
// The schematic mixes SET/RESET polarity; do not derive it from odd/even bits.
constexpr SegmentCommand COMMANDS[DIGIT_COUNT][SEGMENT_COUNT] = {
    {{10, 11}, {12, 13}, {15, 14}, {3, 2}, {7, 6}, {5, 4}, {1, 0}},
    {{28, 29}, {30, 31}, {17, 16}, {21, 20}, {9, 8}, {23, 22}, {19, 18}},
    {{47, 46}, {32, 33}, {35, 34}, {25, 24}, {27, 26}, {39, 38}, {37, 36}},
    {{48, 49}, {50, 51}, {53, 52}, {41, 40}, {45, 44}, {43, 42}, {55, 54}},
};

// A is bit 0, G is bit 6; zero is also the blank hour-tens glyph.
constexpr uint8_t DIGIT_GLYPHS[10] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f,
};

inline bool makePulseFrame(std::size_t address, Frame &frame) {
  // Erase stale commands even when rejecting an invalid output address.
  frame.fill(0);
  if (address >= OUTPUT_COUNT) {
    return false;
  }
  frame[address / 8] = static_cast<uint8_t>(1u << (address % 8));
  return true;
}

inline uint8_t wireByte(const Frame &frame, std::size_t index) {
  // The farthest register is shifted first; each byte itself is MSBFIRST.
  return index < REGISTER_COUNT ? frame[REGISTER_COUNT - 1 - index] : 0;
}

inline bool renderTime(int hour24, int minute, DigitMasks &target) {
  // Reject bad time fields before using them as glyph-table indices.
  if (hour24 < 0 || hour24 > 23 || minute < 0 || minute > 59) {
    return false;
  }
  const int hour12 = hour24 % 12 == 0 ? 12 : hour24 % 12;

  // Hours 1-9 have blank tens; minutes always retain their leading zero.
  target[0] = hour12 >= 10 ? DIGIT_GLYPHS[hour12 / 10] : 0;
  target[1] = DIGIT_GLYPHS[hour12 % 10];
  target[2] = DIGIT_GLYPHS[minute / 10];
  target[3] = DIGIT_GLYPHS[minute % 10];
  return true;
}

template <typename Backend>
bool pulseCommand(Backend &output, std::size_t address) {
  // OE_N must be HIGH before loading or rejecting any command.
  output.disableOutputs();
  Frame command{};
  if (!makePulseFrame(address, command)) {
    output.writeFrame(command);
    output.waitIdle();
    return false;
  }

  // Load and settle with outputs disabled. Only this one command is active.
  output.writeFrame(command);
  output.waitLatchSettle();
  output.enableOutputs();
  output.waitPulse();
  output.disableOutputs();  // First operation after the pulse: OE_N HIGH.

  // Clear every stored command before yielding or waiting between pulses.
  output.writeFrame(Frame{});
  output.waitIdle();
  return true;
}

class DisplayController {
 public:
  template <typename Backend>
  bool apply(const DigitMasks &target, Backend &output, bool pulsesEnabled) {
    // Disabled pulsing or malformed masks must never establish known state.
    lastPulseCount_ = 0;
    if (!pulsesEnabled || !validMasks(target)) {
      output.disableOutputs();
      output.writeFrame(Frame{});
      output.waitIdle();
      return false;
    }

    // Copy the target so a complete update uses a single time snapshot.
    const DigitMasks snapshot = target;
    for (std::size_t digit = 0; digit < DIGIT_COUNT; ++digit) {
      for (std::size_t segment = 0; segment < SEGMENT_COUNT; ++segment) {
        const uint8_t mask = static_cast<uint8_t>(1u << segment);
        if ((known_[digit] & mask) && !((commanded_[digit] ^ snapshot[digit]) & mask)) {
          continue;
        }

        // Unknown segments are commanded even when their requested state is hidden.
        const bool visible = (snapshot[digit] & mask) != 0;
        const SegmentCommand &command = COMMANDS[digit][segment];
        if (!pulseCommand(output, visible ? command.set : command.reset)) {
          return false;
        }

        // Record only completed commands; no mechanical position sensor exists.
        commanded_[digit] = static_cast<uint8_t>(
            (commanded_[digit] & ~mask) | (snapshot[digit] & mask));
        known_[digit] |= mask;
        ++lastPulseCount_;
      }
    }
    output.disableOutputs();
    return true;
  }

  std::size_t lastPulseCount() const { return lastPulseCount_; }

 private:
  static bool validMasks(const DigitMasks &target) {
    // Bit 7 is not a segment and cannot be silently accepted.
    for (uint8_t mask : target) {
      if (mask & 0x80) {
        return false;
      }
    }
    return true;
  }

  // A zero commanded mask alone does not imply a physically blank display.
  DigitMasks commanded_{};
  DigitMasks known_{};
  std::size_t lastPulseCount_ = 0;
};

}  // namespace clock_display
