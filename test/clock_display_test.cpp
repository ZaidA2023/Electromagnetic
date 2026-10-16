// Standalone host checks: no Arduino runtime, connected board, or test library.
#include "clock_config.example.h"
#include "clock_display.h"

#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using clock_display::DigitMasks;
using clock_display::DisplayController;
using clock_display::Frame;

std::string context;

void check(bool passed, const char *expression, int line) {
  if (!passed) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + expression +
                             (context.empty() ? "" : " [" + context + "]"));
  }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

// Independent oracle: physical QA-QH labels transcribed from the approved
// schematic table, rather than a second copy of COMMANDS' numeric addresses.
const char *const SCHEMATIC[7][8] = {
    {"D1_G_RESET", "D1_G_SET", "D1_D_RESET", "D1_D_SET",
     "D1_F_RESET", "D1_F_SET", "D1_E_RESET", "D1_E_SET"},
    {"D2_E_RESET", "D2_E_SET", "D1_A_SET", "D1_A_RESET",
     "D1_B_SET", "D1_B_RESET", "D1_C_RESET", "D1_C_SET"},
    {"D2_C_RESET", "D2_C_SET", "D2_G_RESET", "D2_G_SET",
     "D2_D_RESET", "D2_D_SET", "D2_F_RESET", "D2_F_SET"},
    {"D3_D_RESET", "D3_D_SET", "D3_E_RESET", "D3_E_SET",
     "D2_A_SET", "D2_A_RESET", "D2_B_SET", "D2_B_RESET"},
    {"D3_B_SET", "D3_B_RESET", "D3_C_RESET", "D3_C_SET",
     "D3_G_RESET", "D3_G_SET", "D3_F_RESET", "D3_F_SET"},
    {"D4_D_RESET", "D4_D_SET", "D4_F_RESET", "D4_F_SET",
     "D4_E_RESET", "D4_E_SET", "D3_A_RESET", "D3_A_SET"},
    {"D4_A_SET", "D4_A_RESET", "D4_B_SET", "D4_B_RESET",
     "D4_C_RESET", "D4_C_SET", "D4_G_RESET", "D4_G_SET"},
};

std::size_t schematicAddress(std::size_t digit, std::size_t segment, bool set) {
  const std::string label = "D" + std::to_string(digit + 1) + "_" +
                            static_cast<char>('A' + segment) +
                            (set ? "_SET" : "_RESET");
  // Walk the physical labels, so wrong command-table indices cannot define
  // the expected pulse order or hide a reversed SET/RESET pair.
  for (std::size_t chip = 0; chip < 7; ++chip) {
    for (std::size_t output = 0; output < 8; ++output) {
      if (label == SCHEMATIC[chip][output]) {
        return chip * 8 + output;
      }
    }
  }
  throw std::runtime_error("Missing schematic label " + label);
}

Frame expectedFrame(std::size_t address) {
  Frame expected{};
  CHECK(address < 56);
  expected[address / 8] = static_cast<uint8_t>(1u << (address % 8));
  return expected;
}

// Model serial flip-flops, not byte reversal: SER enters U33 QA on every
// rising edge; QH cascades to the next chip. Latching is a separate operation.
class PhysicalChain {
 public:
  PhysicalChain() { dirty(); }

  void dirty() {
    shifting_.fill(true);
    stored_.fill(true);
  }

  void send(const Frame &frame) {
    const std::size_t before = clocks_;
    for (std::size_t byte = 0; byte < 7; ++byte) {
      const uint8_t serial = clock_display::wireByte(frame, byte);
      // The hardware contract is MSBFIRST, independent of frame addressing.
      for (int bit = 7; bit >= 0; --bit) {
        clockBit((serial & (1u << bit)) != 0);
      }
    }
    CHECK(clocks_ - before == 56);
    stored_ = shifting_;  // One latch edge after the complete transfer.
  }

  std::size_t activeCount() const {
    std::size_t count = 0;
    for (bool active : stored_) {
      count += active ? 1 : 0;
    }
    return count;
  }

  std::size_t activeAddress() const {
    CHECK(activeCount() == 1);
    for (std::size_t address = 0; address < 56; ++address) {
      if (stored_[address]) {
        return address;
      }
    }
    throw std::runtime_error("No active physical output");
  }

 private:
  void clockBit(bool serial) {
    // Advance every stage, including each chip's QH-to-SER connection.
    for (std::size_t stage = 55; stage > 0; --stage) {
      shifting_[stage] = shifting_[stage - 1];
    }
    shifting_[0] = serial;
    ++clocks_;
  }

  std::array<bool, 56> shifting_{};
  std::array<bool, 56> stored_{};
  std::size_t clocks_ = 0;
};

enum class Event { Disable, Write, Settle, Enable, PulseWait, IdleWait };

struct RecordedEvent {
  Event kind;
  Frame frame;
};

// This fake runs the real pulse routine. Its initial dirty, enabled state
// makes even the first load/rejection prove that disableOutputs happened first.
struct Backend {
  PhysicalChain chain;
  bool enabled = true;
  std::vector<RecordedEvent> events;
  std::vector<std::size_t> addresses;

  void record(Event event, const Frame &frame = Frame{}) {
    // No backend operation may intervene between pulse wait and OE disable.
    CHECK(events.empty() || events.back().kind != Event::PulseWait ||
          event == Event::Disable);
    events.push_back(RecordedEvent{event, frame});
  }

  void disableOutputs() {
    record(Event::Disable);
    enabled = false;
  }

  void writeFrame(Frame frame) {
    // Loading must never expose intermediate serial data to enabled drivers.
    CHECK(!enabled);
    record(Event::Write, frame);
    chain.send(frame);
  }

  void waitLatchSettle() {
    CHECK(!enabled);
    CHECK(!events.empty() && events.back().kind == Event::Write);
    CHECK(chain.activeCount() == 1);
    record(Event::Settle);
  }

  void enableOutputs() {
    // Only a complete, latched, settled single-command frame may be enabled.
    CHECK(!enabled);
    CHECK(!events.empty() && events.back().kind == Event::Settle);
    CHECK(chain.activeCount() == 1);
    record(Event::Enable);
    addresses.push_back(chain.activeAddress());
    enabled = true;
  }

  void waitPulse() {
    // The trace recorder requires OE disable to be the very next operation.
    CHECK(enabled);
    CHECK(!events.empty() && events.back().kind == Event::Enable);
    record(Event::PulseWait);
  }

  void waitIdle() {
    // Yielding is allowed only after disabling and clearing the whole chain.
    CHECK(!enabled);
    CHECK(chain.activeCount() == 0);
    CHECK(!events.empty() && events.back().kind == Event::Write);
    record(Event::IdleWait);
  }

  void clearHistory() {
    // Preserve physical state; resetting the trace must not hide a stale output.
    CHECK(!enabled);
    CHECK(chain.activeCount() == 0);
    events.clear();
    addresses.clear();
  }
};

void checkSafe(const Backend &backend) {
  CHECK(!backend.enabled);
  CHECK(backend.chain.activeCount() == 0);
}

void checkPulseTrace(const Backend &backend,
                     const std::vector<std::size_t> &addresses,
                     bool controllerFinish) {
  const Event sequence[] = {Event::Disable, Event::Write, Event::Settle,
                            Event::Enable, Event::PulseWait, Event::Disable,
                            Event::Write, Event::IdleWait};
  CHECK(backend.addresses == addresses);
  CHECK(backend.events.size() == addresses.size() * 8 +
                                    (controllerFinish ? 1u : 0u));
  // Exact event matching catches extra writes, missing waits, early enable,
  // delayed OE disable, or a nonzero frame left in the storage latches.
  for (std::size_t pulse = 0; pulse < addresses.size(); ++pulse) {
    for (std::size_t step = 0; step < 8; ++step) {
      CHECK(backend.events[pulse * 8 + step].kind == sequence[step]);
    }
    CHECK(backend.events[pulse * 8 + 1].frame == expectedFrame(addresses[pulse]));
    CHECK(backend.events[pulse * 8 + 6].frame == Frame{});
  }
  if (controllerFinish) {
    // Even an unchanged controller update must finish with OE disabled.
    CHECK(backend.events.back().kind == Event::Disable);
  }
  checkSafe(backend);
}

void checkRejectedTrace(const Backend &backend) {
  CHECK(backend.addresses.empty());
  CHECK(backend.events.size() == 3);
  CHECK(backend.events[0].kind == Event::Disable);
  CHECK(backend.events[1].kind == Event::Write);
  CHECK(backend.events[1].frame == Frame{});
  CHECK(backend.events[2].kind == Event::IdleWait);
  checkSafe(backend);
}

uint8_t expectedGlyph(char digit) {
  // Visual segment membership is independent of the implementation's hex masks.
  const char *const segments[] = {"ABCDEF", "BC", "ABDEG", "ABCDG", "BCFG",
                                  "ACDFG", "ACDEFG", "ABC", "ABCDEFG", "ABCDFG"};
  if (digit == ' ') {
    return 0;
  }
  CHECK(digit >= '0' && digit <= '9');
  uint8_t mask = 0;
  // Translate letter positions without consulting DIGIT_GLYPHS.
  for (const char *letter = segments[digit - '0']; *letter; ++letter) {
    mask |= static_cast<uint8_t>(1u << (*letter - 'A'));
  }
  return mask;
}

DigitMasks expectedText(const char *text) {
  DigitMasks masks{};
  for (std::size_t digit = 0; digit < 4; ++digit) {
    masks[digit] = expectedGlyph(text[digit]);
  }
  return masks;
}

DigitMasks expectedTime(int hour, int minute) {
  // A literal hour chart avoids sharing the renderer's modulo/blanking logic.
  const char *const hours[] = {"12", " 1", " 2", " 3", " 4", " 5", " 6", " 7",
                              " 8", " 9", "10", "11", "12", " 1", " 2", " 3",
                              " 4", " 5", " 6", " 7", " 8", " 9", "10", "11"};
  char text[5] = {};
  CHECK(std::snprintf(text, sizeof(text), "%s%02d", hours[hour], minute) == 4);
  return expectedText(text);
}

std::vector<std::size_t> expectedCommands(const DigitMasks &target,
                                         const DigitMasks *previous = nullptr) {
  std::vector<std::size_t> addresses;
  // D1-D4/A-G ordering is an explicit requirement; addresses come only from
  // schematic labels. A null previous target means all 28 segments are unknown.
  for (std::size_t digit = 0; digit < 4; ++digit) {
    for (std::size_t segment = 0; segment < 7; ++segment) {
      const uint8_t bit = static_cast<uint8_t>(1u << segment);
      if (!previous || ((target[digit] ^ (*previous)[digit]) & bit)) {
        addresses.push_back(schematicAddress(digit, segment,
                                             (target[digit] & bit) != 0));
      }
    }
  }
  return addresses;
}

void applyAndCheck(DisplayController &controller, Backend &backend,
                   const DigitMasks &target, const DigitMasks *previous) {
  backend.clearHistory();
  const std::vector<std::size_t> commands = expectedCommands(target, previous);
  CHECK(controller.apply(target, backend, true));
  CHECK(controller.lastPulseCount() == commands.size());
  checkPulseTrace(backend, commands, true);
}

DigitMasks letterMasks(const std::array<const char *, 4> &segments) {
  DigitMasks masks{};
  // Spell each letter's strokes independently of the production numeric masks.
  for (std::size_t digit = 0; digit < 4; ++digit) {
    for (const char *segment = segments[digit]; *segment; ++segment) {
      masks[digit] |= static_cast<uint8_t>(1u << (*segment - 'A'));
    }
  }
  return masks;
}

struct DisplayHold {
  uint32_t milliseconds;
  DigitMasks visible;
  Backend trace;
};

struct GreetingBackend : Backend {
  // Start with every stroke visible: a zero register flush cannot blank them.
  DigitMasks visible = expectedText("8888");
  std::vector<DisplayHold> holds;

  void waitDisplay(uint32_t milliseconds) {
    checkSafe(*this);
    // Decode the actual physical outputs, without consulting COMMANDS or targets.
    for (std::size_t address : addresses) {
      CHECK(address < 56);
      const std::string label = SCHEMATIC[address / 8][address % 8];
      const std::size_t digit = static_cast<std::size_t>(label[1] - '1');
      const uint8_t bit = static_cast<uint8_t>(1u << (label[3] - 'A'));
      visible[digit] = static_cast<uint8_t>(label.substr(5) == "SET"
                                               ? visible[digit] | bit
                                               : visible[digit] & ~bit);
    }
    // Preserve the entire pulse trace at each safe hold, retaining physical state.
    holds.push_back(DisplayHold{milliseconds, visible, *this});
    clearHistory();
  }
};

void testStartupGreetingGlyphsAndPhysicalSequence() {
  const DigitMasks hi = letterMasks({{"", "BCEFG", "EF", ""}});
  const DigitMasks name = letterMasks({{"ABDEG", "ABCEFG", "EF", "BCDEG"}});
  // I uses the left verticals; d uses the conventional lowercase approximation.
  CHECK(clock_display::STARTUP_HI == hi);
  CHECK(clock_display::STARTUP_NAME == name);
  DisplayController controller;
  GreetingBackend backend;
  CHECK(clock_display::showStartupGreeting(controller, backend, true));
  CHECK(backend.holds.size() == 3);

  // Each snapshot proves the word, exact schematic sequence, and completed count.
  const DigitMasks targets[] = {hi, DigitMasks{}, name};
  const uint32_t durations[] = {1000, 250, 2000};
  const std::size_t counts[] = {28, 7, 18};
  for (std::size_t stage = 0; stage < 3; ++stage) {
    context = "greeting stage " + std::to_string(stage);
    const DisplayHold &hold = backend.holds[stage];
    CHECK(hold.milliseconds == durations[stage]);
    CHECK(hold.visible == targets[stage]);
    CHECK(hold.trace.addresses.size() == counts[stage]);
    // Existing trace checks also require OE disabled and every latch zero.
    checkPulseTrace(hold.trace,
                    expectedCommands(targets[stage], stage ? &targets[stage - 1] : nullptr),
                    true);
  }
  // The last hold must leave the name retained, with no subsequent output work.
  CHECK(controller.lastPulseCount() == 18);
  CHECK(backend.visible == name);
  CHECK(backend.addresses.empty());
  CHECK(backend.events.empty());
  checkSafe(backend);
}

void testStartupGreetingHandoffAndDisabledState() {
  const DigitMasks name = letterMasks({{"ABDEG", "ABCEFG", "EF", "BCDEG"}});
  for (int padding = 0; padding < 2; ++padding) {
    context = padding ? "greeting to 01:00" : "greeting to blank-1:00";
    DisplayController controller;
    GreetingBackend backend;
    CHECK(clock_display::showStartupGreeting(controller, backend, true));
    CHECK(backend.holds.size() == 3);
    // Exercise the public time renderer before checking the physical handoff.
    DigitMasks time{};
    CHECK(clock_display::renderTime(1, 0, time, padding != 0));
    CHECK(time == expectedText(padding ? "0100" : " 100"));

    // Handoff must use the cached name, including D1 resets or a real zero glyph.
    applyAndCheck(controller, backend, time, &name);
    CHECK(controller.lastPulseCount() == (padding ? 14u : 16u));
    applyAndCheck(controller, backend, time, &time);
    CHECK(controller.lastPulseCount() == 0);
    CHECK(backend.holds.size() == 3);

    // Lockout must clear unsafe initial latches without any pulse or display hold.
    DisplayController disabledController;
    GreetingBackend disabledBackend;
    CHECK(!clock_display::showStartupGreeting(disabledController, disabledBackend, false));
    CHECK(disabledController.lastPulseCount() == 0);
    CHECK(disabledBackend.holds.empty());
    checkRejectedTrace(disabledBackend);

    // No skipped greeting target may mark any segment known, even a hidden D1.
    applyAndCheck(disabledController, disabledBackend, time, nullptr);
    CHECK(disabledController.lastPulseCount() == 28);
    CHECK(disabledBackend.holds.empty());
  }
}

void testMapping() {
  CHECK(clock_display::REGISTER_COUNT == 7);
  CHECK(clock_display::DIGIT_COUNT == 4);
  CHECK(clock_display::SEGMENT_COUNT == 7);
  CHECK(clock_display::OUTPUT_COUNT == 56);
  std::array<unsigned int, 56> uses{};
  // Validate every polarity against physical labels, then account for all pins.
  for (std::size_t digit = 0; digit < 4; ++digit) {
    for (std::size_t segment = 0; segment < 7; ++segment) {
      for (int polarity = 0; polarity < 2; ++polarity) {
        const bool set = polarity != 0;
        const clock_display::SegmentCommand &command =
            clock_display::COMMANDS[digit][segment];
        const std::size_t actual = set ? command.set : command.reset;
        context = SCHEMATIC[schematicAddress(digit, segment, set) / 8]
                           [schematicAddress(digit, segment, set) % 8];
        // Check range before indexing, so a bad table fails without test UB.
        CHECK(actual < 56);
        CHECK(actual == schematicAddress(digit, segment, set));
        ++uses[actual];
      }
    }
  }
  // No duplicate command may conceal an omitted physical output.
  for (unsigned int count : uses) {
    CHECK(count == 1);
  }
  // These pairs deliberately break an assumed even/odd SET/RESET convention.
  CHECK(clock_display::COMMANDS[0][0].set == 10);
  CHECK(clock_display::COMMANDS[0][0].reset == 11);
  CHECK(clock_display::COMMANDS[1][1].set == 30);
  CHECK(clock_display::COMMANDS[1][1].reset == 31);
  CHECK(clock_display::COMMANDS[2][1].set == 32);
  CHECK(clock_display::COMMANDS[2][1].reset == 33);
  CHECK(clock_display::COMMANDS[3][1].reset == 51);
}

void testFramesAndPhysicalChain() {
  PhysicalChain chain;
  Frame frame{};
  CHECK(frame.size() == 7);
  for (std::size_t address = 0; address < 56; ++address) {
    context = "physical address " + std::to_string(address);
    chain.dirty();
    frame.fill(0xff);
    CHECK(clock_display::makePulseFrame(address, frame));
    CHECK(frame == expectedFrame(address));
    // Seven complete bytes must replace both dirty input and dirty latches.
    chain.send(frame);
    CHECK(chain.activeAddress() == address);
    const std::size_t replacement = (address + 19) % 56;
    CHECK(clock_display::makePulseFrame(replacement, frame));
    chain.send(frame);
    CHECK(chain.activeAddress() == replacement);
    chain.send(Frame{});
    CHECK(chain.activeCount() == 0);
  }
  // Concrete schematic examples also check the nearest/farthest byte contract.
  CHECK(clock_display::makePulseFrame(7, frame));
  CHECK(frame[0] == 0x80);
  CHECK(clock_display::wireByte(frame, 6) == 0x80);
  CHECK(clock_display::makePulseFrame(51, frame));
  CHECK(frame[6] == 0x08);
  CHECK(clock_display::wireByte(frame, 0) == 0x08);

  // Distinct bytes make every reversal position observable, including zero-free
  // data that cannot pass merely by choosing the only nonzero byte.
  const Frame distinct = {{0x13, 0x25, 0x47, 0x89, 0xab, 0xcd, 0xef}};
  const uint8_t wire[] = {0xef, 0xcd, 0xab, 0x89, 0x47, 0x25, 0x13};
  for (std::size_t index = 0; index < 7; ++index) {
    CHECK(clock_display::wireByte(distinct, index) == wire[index]);
  }
  // Reject boundary and overflow-sized addresses, always erasing stale commands.
  const std::size_t invalid[] = {56, 57, 255, 256,
                                 std::numeric_limits<std::size_t>::max()};
  for (std::size_t address : invalid) {
    frame.fill(0xff);
    CHECK(!clock_display::makePulseFrame(address, frame));
    CHECK(frame == Frame{});
    // The rejected constructor's output is safe even over dirty physical state.
    chain.dirty();
    chain.send(frame);
    CHECK(chain.activeCount() == 0);
    CHECK(clock_display::wireByte(distinct, address) == 0);
  }
  CHECK(clock_display::wireByte(distinct, 7) == 0);
  CHECK(clock_display::wireByte(distinct, 8) == 0);
}

void testGlyphsAndAllMinutes() {
  for (int digit = 0; digit < 10; ++digit) {
    CHECK(clock_display::DIGIT_GLYPHS[digit] == expectedGlyph('0' + digit));
  }
  // Every minute of a full day checks AM/PM folding, blank tens, minute zeros,
  // and glyph selection against a textual clock rather than the numeric table.
  for (int hour = 0; hour < 24; ++hour) {
    for (int minute = 0; minute < 60; ++minute) {
      context = std::to_string(hour) + ":" + std::to_string(minute);
      DigitMasks actual = {{0xff, 0xff, 0xff, 0xff}};
      CHECK(clock_display::renderTime(hour, minute, actual));
      CHECK(actual == expectedTime(hour, minute));

      // Check optional padding against the independent textual hour chart.
      DigitMasks padded = expectedTime(hour, minute);
      if (padded[0] == 0) {
        padded[0] = expectedGlyph('0');
      }
      CHECK(clock_display::renderTime(hour, minute, actual, true));
      CHECK(actual == padded);
    }
  }
  // Keep the named presentation boundaries visible alongside the exhaustive run.
  struct Example { int hour; int minute; const char *text; };
  const Example examples[] = {{0, 0, "1200"}, {9, 59, " 959"},
                              {10, 0, "1000"}, {12, 0, "1200"},
                              {12, 59, "1259"}, {13, 0, " 100"},
                              {23, 59, "1159"}};
  for (const Example &example : examples) {
    DigitMasks actual{};
    CHECK(clock_display::renderTime(example.hour, example.minute, actual));
    CHECK(actual == expectedText(example.text));
  }
  CHECK(expectedGlyph(' ') == 0);

  // The requested example must differ only in its first digit.
  DigitMasks example{};
  CHECK(clock_display::renderTime(1, 0, example, false));
  CHECK(example == expectedText(" 100"));
  CHECK(clock_display::renderTime(1, 0, example, true));
  CHECK(example == expectedText("0100"));
}

void testInvalidTimeFields() {
  const int invalid[][2] = {{-1, 0}, {24, 0}, {0, -1}, {0, 60},
                            {INT_MIN, 59}, {INT_MAX, 0},
                            {23, INT_MIN}, {0, INT_MAX}, {-1, 60}};
  // Invalid input must leave the caller's target untouched, even with bad bits.
  const DigitMasks sentinel = {{0xa5, 0x5a, 0xff, 0x80}};
  for (const auto &fields : invalid) {
    context = "invalid " + std::to_string(fields[0]) + ":" +
              std::to_string(fields[1]);
    DigitMasks target = sentinel;
    CHECK(!clock_display::renderTime(fields[0], fields[1], target));
    CHECK(target == sentinel);
    // Padding must not bypass field validation or modify a rejected target.
    CHECK(!clock_display::renderTime(fields[0], fields[1], target, true));
    CHECK(target == sentinel);
  }
}

void testActualPulseSequence() {
  for (std::size_t address = 0; address < 56; ++address) {
    context = "pulse " + std::to_string(address);
    Backend backend;
    CHECK(clock_display::pulseCommand(backend, address));
    checkPulseTrace(backend, std::vector<std::size_t>{address}, false);
    // A second pulse uses the same backend, proving the first finished idle.
    backend.clearHistory();
    const std::size_t next = (address + 1) % 56;
    CHECK(clock_display::pulseCommand(backend, next));
    checkPulseTrace(backend, std::vector<std::size_t>{next}, false);
  }
}

void testRejectedPulseAddresses() {
  const std::size_t invalid[] = {56, 57, 255, 256,
                                 std::numeric_limits<std::size_t>::max()};
  for (std::size_t address : invalid) {
    context = "invalid pulse " + std::to_string(address);
    Backend backend;
    CHECK(!clock_display::pulseCommand(backend, address));
    checkRejectedTrace(backend);
    // Rejection must not poison subsequent valid commands.
    backend.clearHistory();
    CHECK(clock_display::pulseCommand(backend, 51));
    checkPulseTrace(backend, std::vector<std::size_t>{51}, false);
  }
}

void testStartupAndUnchanged() {
  // Startup must establish both visible and hidden positions before caching.
  DisplayController controller;
  Backend backend;
  const DigitMasks target = expectedText("1234");
  CHECK(controller.lastPulseCount() == 0);
  CHECK(controller.apply(target, backend, true));
  CHECK(controller.lastPulseCount() == 28);
  checkPulseTrace(backend, expectedCommands(target), true);
  applyAndCheck(controller, backend, target, &target);
  CHECK(controller.lastPulseCount() == 0);
}

void testEveryIndividualSegmentChange() {
  DisplayController controller;
  Backend backend;
  const DigitMasks hidden{};
  CHECK(controller.apply(hidden, backend, true));
  checkPulseTrace(backend, expectedCommands(hidden), true);
  // Isolate each bit so an overbroad update or a missing per-bit cache write
  // cannot hide behind the other changes in an ordinary minute transition.
  for (std::size_t digit = 0; digit < 4; ++digit) {
    for (std::size_t segment = 0; segment < 7; ++segment) {
      context = "single segment D" + std::to_string(digit + 1) + "_" +
                static_cast<char>('A' + segment);
      DigitMasks visible = hidden;
      visible[digit] = static_cast<uint8_t>(1u << segment);
      applyAndCheck(controller, backend, visible, &hidden);
      CHECK(backend.addresses == std::vector<std::size_t>{
                                    schematicAddress(digit, segment, true)});
      // Repeating the request must be free; hiding it must use just its RESET.
      applyAndCheck(controller, backend, visible, &visible);
      CHECK(controller.lastPulseCount() == 0);
      applyAndCheck(controller, backend, hidden, &visible);
      CHECK(backend.addresses == std::vector<std::size_t>{
                                    schematicAddress(digit, segment, false)});
    }
  }
  // Also verify full known-state changes in both directions, with 28 pulses.
  const DigitMasks allVisible = {{0x7f, 0x7f, 0x7f, 0x7f}};
  applyAndCheck(controller, backend, allVisible, &hidden);
  CHECK(controller.lastPulseCount() == 28);
  applyAndCheck(controller, backend, hidden, &allVisible);
  CHECK(controller.lastPulseCount() == 28);
}

void testChangedSegmentsAndBackwardClock() {
  DisplayController controller;
  Backend backend;
  DigitMasks previous = expectedTime(12, 59);
  CHECK(controller.apply(previous, backend, true));
  checkPulseTrace(backend, expectedCommands(previous), true);
  // Exercise hour rollover, midnight, minute rollover, and backward corrections.
  const int times[][2] = {{13, 0}, {12, 59}, {23, 59}, {0, 0},
                          {0, 1}, {0, 0}, {1, 59}, {1, 0}};
  for (const auto &time : times) {
    context = "clock change to " + std::to_string(time[0]) + ":" +
              std::to_string(time[1]);
    const DigitMasks target = expectedTime(time[0], time[1]);
    applyAndCheck(controller, backend, target, &previous);
    CHECK(controller.lastPulseCount() > 0);
    previous = target;
  }
  // Pin one transition to a hand-checked schematic output: 1 -> 7 adds D4 A.
  const DigitMasks one = expectedText(" 101");
  applyAndCheck(controller, backend, one, &previous);
  const DigitMasks seven = expectedText(" 107");
  applyAndCheck(controller, backend, seven, &one);
  CHECK(backend.addresses == std::vector<std::size_t>{48});
  applyAndCheck(controller, backend, one, &seven);
  CHECK(backend.addresses == std::vector<std::size_t>{49});
}

void testBlankHourTensAndHiddenStartup() {
  DisplayController controller;
  Backend backend;
  const DigitMasks blankTens = expectedTime(9, 59);
  CHECK(blankTens[0] == 0);
  CHECK(controller.apply(blankTens, backend, true));
  checkPulseTrace(backend, expectedCommands(blankTens), true);
  // Even a blank D1 is initially unknown: A-G all need explicit RESET pulses.
  const std::size_t d1Resets[] = {11, 13, 14, 2, 6, 4, 0};
  CHECK(controller.lastPulseCount() == 28);
  for (std::size_t segment = 0; segment < 7; ++segment) {
    CHECK(backend.addresses[segment] == d1Resets[segment]);
  }
  // D1 must acquire and then lose its visible hour-tens strokes on rollover.
  const DigitMasks ten = expectedTime(10, 0);
  applyAndCheck(controller, backend, ten, &blankTens);
  applyAndCheck(controller, backend, blankTens, &ten);
  applyAndCheck(controller, backend, blankTens, &blankTens);

  // All-zero masks still require 28 startup resets; a zero cache is not known.
  DisplayController hiddenController;
  Backend hiddenBackend;
  const DigitMasks hidden{};
  CHECK(hiddenController.apply(hidden, hiddenBackend, true));
  CHECK(hiddenController.lastPulseCount() == 28);
  checkPulseTrace(hiddenBackend, expectedCommands(hidden), true);
  applyAndCheck(hiddenController, hiddenBackend, hidden, &hidden);
}

void testDisabledRequestsPreserveUnknownAndKnownState() {
  DisplayController controller;
  Backend backend;
  const DigitMasks original = expectedTime(9, 59);
  const DigitMasks other = expectedTime(10, 0);
  CHECK(!controller.apply(original, backend, false));
  CHECK(controller.lastPulseCount() == 0);
  checkRejectedTrace(backend);
  // Repeated disabled attempts cannot establish even hidden segments as known.
  backend.clearHistory();
  CHECK(!controller.apply(other, backend, false));
  checkRejectedTrace(backend);
  applyAndCheck(controller, backend, original, nullptr);
  CHECK(controller.lastPulseCount() == 28);

  backend.clearHistory();
  CHECK(!controller.apply(other, backend, false));
  CHECK(controller.lastPulseCount() == 0);
  checkRejectedTrace(backend);
  // A skipped update neither overwrites nor discards previously commanded state.
  applyAndCheck(controller, backend, original, &original);
  CHECK(controller.lastPulseCount() == 0);
  applyAndCheck(controller, backend, other, &original);
}

void testInvalidMasksPreserveUnknownAndKnownState() {
  const DigitMasks original = expectedTime(12, 34);
  const DigitMasks other = expectedTime(1, 0);
  for (std::size_t digit = 0; digit < 4; ++digit) {
    context = "invalid mask at D" + std::to_string(digit + 1);
    DisplayController controller;
    Backend backend;
    // Put the illegal bit in each position, including the final digit.
    DigitMasks invalid = other;
    invalid[digit] |= 0x80;
    CHECK(!controller.apply(invalid, backend, true));
    CHECK(controller.lastPulseCount() == 0);
    checkRejectedTrace(backend);
    // A bad late digit must reject the whole request before any earlier pulse.
    applyAndCheck(controller, backend, original, nullptr);
    CHECK(controller.lastPulseCount() == 28);
    backend.clearHistory();
    CHECK(!controller.apply(invalid, backend, true));
    CHECK(controller.lastPulseCount() == 0);
    checkRejectedTrace(backend);
    applyAndCheck(controller, backend, original, &original);
    applyAndCheck(controller, backend, other, &original);
  }
}

struct ChangingBackend : Backend {
  DigitMasks *target = nullptr;
  DigitMasks replacement{};

  void waitPulse() {
    Backend::waitPulse();
    // Simulate a new time arriving during an update, without adding I/O calls.
    if (target) {
      *target = replacement;
      target = nullptr;
    }
  }
};

void testTargetSnapshot() {
  DisplayController controller;
  ChangingBackend backend;
  const DigitMasks original = expectedText("1259");
  DigitMasks target = original;
  backend.target = &target;
  backend.replacement = expectedText(" 100");
  CHECK(controller.apply(target, backend, true));
  CHECK(target == backend.replacement);
  // All initial commands and the retained state must still use the old snapshot.
  checkPulseTrace(backend, expectedCommands(original), true);
  CHECK(controller.lastPulseCount() == 28);
  applyAndCheck(controller, backend, target, &original);
}

class TimeZoneGuard {
 public:
  TimeZoneGuard() {
    const char *current = std::getenv("TZ");
    hadValue_ = current != nullptr;
    if (hadValue_) {
      saved_ = current;
    }
    CHECK(::setenv("TZ", clock_config::TIME_ZONE, 1) == 0);
    ::tzset();
  }

  ~TimeZoneGuard() {
    // Restore the process timezone even when an assertion throws.
    if (hadValue_) {
      ::setenv("TZ", saved_.c_str(), 1);
    } else {
      ::unsetenv("TZ");
    }
    ::tzset();
  }

 private:
  bool hadValue_ = false;
  std::string saved_;
};

bool leapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

std::time_t utcEpoch(int year, int month, int day, int hour, int minute,
                     int second) {
  const int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  long long days = 0;
  // Count Gregorian days directly: mktime would apply the TZ under test and
  // could silently normalize a nonexistent or repeated transition time.
  for (int current = 1970; current < year; ++current) {
    days += leapYear(current) ? 366 : 365;
  }
  for (int current = 1; current < month; ++current) {
    days += monthDays[current - 1] + (current == 2 && leapYear(year) ? 1 : 0);
  }
  days += day - 1;
  return static_cast<std::time_t>(days * 86400 + hour * 3600 + minute * 60 + second);
}

void checkLocal(std::time_t utc, int year, int month, int day, int hour,
                 int minute, int second, bool daylight) {
  std::tm local{};
  CHECK(::localtime_r(&utc, &local) != nullptr);
  CHECK(local.tm_year == year - 1900);
  CHECK(local.tm_mon == month - 1);
  CHECK(local.tm_mday == day);
  CHECK(local.tm_hour == hour);
  CHECK(local.tm_min == minute);
  CHECK(local.tm_sec == second);
  // Check DST status, abbreviation, and signed UTC offset independently.
  CHECK(local.tm_isdst >= 0);
  CHECK((local.tm_isdst > 0) == daylight);
  char name[16] = {};
  CHECK(std::strftime(name, sizeof(name), "%Z", &local) != 0);
  CHECK(std::string(name) == (daylight ? "CDT" : "CST"));
  const std::time_t localAsUtc = utcEpoch(local.tm_year + 1900, local.tm_mon + 1,
                                         local.tm_mday, local.tm_hour,
                                         local.tm_min, local.tm_sec);
  CHECK(std::difftime(localAsUtc, utc) == (daylight ? -5 : -6) * 3600);
  // The converted civil time must also render through the real clock helper.
  DigitMasks rendered{};
  CHECK(clock_display::renderTime(local.tm_hour, local.tm_min, rendered));
  CHECK(rendered == expectedTime(hour, minute));
}

void testCentralTimeAnd2026Boundaries() {
  CHECK(std::string(clock_config::TIME_ZONE) == "CST6CDT,M3.2.0,M11.1.0");
  TimeZoneGuard timezone;
  // Sanity-check the independent epoch builder before using 2026 timestamps.
  CHECK(utcEpoch(1970, 1, 1, 0, 0, 0) == 0);
  CHECK(utcEpoch(2000, 1, 1, 0, 0, 0) == 946684800);
  context = "winter and summer Central offsets";
  checkLocal(utcEpoch(2026, 1, 15, 18, 34, 56), 2026, 1, 15, 12, 34, 56, false);
  checkLocal(utcEpoch(2026, 7, 15, 18, 34, 56), 2026, 7, 15, 13, 34, 56, true);
  // Second Sunday in March: 01:59:59 CST jumps to 03:00:00 CDT at 08:00 UTC.
  context = "2026 spring boundary";
  const std::time_t spring = utcEpoch(2026, 3, 8, 8, 0, 0);
  checkLocal(spring - 1, 2026, 3, 8, 1, 59, 59, false);
  checkLocal(spring, 2026, 3, 8, 3, 0, 0, true);
  checkLocal(spring + 1, 2026, 3, 8, 3, 0, 1, true);

  // First Sunday in November: 01:59:59 CDT returns to 01:00:00 CST at 07:00 UTC.
  context = "2026 fall boundary and repeated hour";
  const std::time_t fall = utcEpoch(2026, 11, 1, 7, 0, 0);
  checkLocal(fall - 3600, 2026, 11, 1, 1, 0, 0, true);
  checkLocal(fall - 1, 2026, 11, 1, 1, 59, 59, true);
  checkLocal(fall, 2026, 11, 1, 1, 0, 0, false);
  checkLocal(fall + 1, 2026, 11, 1, 1, 0, 1, false);
  checkLocal(fall + 3600, 2026, 11, 1, 2, 0, 0, false);
}

}  // namespace

int main() {
  struct Test { const char *name; void (*run)(); };
  // Keep individual failures visible rather than aborting at the first group.
  const Test tests[] = {
      {"complete schematic mapping and uniqueness", testMapping},
      {"56-bit physical shift chain and stale-state clearing", testFramesAndPhysicalChain},
      {"glyphs and all 1440 minutes", testGlyphsAndAllMinutes},
      {"invalid time fields", testInvalidTimeFields},
      {"actual pulse ordering and immediate OE disable", testActualPulseSequence},
      {"rejected pulse addresses", testRejectedPulseAddresses},
      {"28 startup pulses and zero unchanged pulses", testStartupAndUnchanged},
      {"every segment changes alone in both directions", testEveryIndividualSegmentChange},
      {"changed segments and backward clock", testChangedSegmentsAndBackwardClock},
      {"blank D1 and initially hidden segments", testBlankHourTensAndHiddenStartup},
      {"disabled requests preserve unknown/known state", testDisabledRequestsPreserveUnknownAndKnownState},
      {"invalid requests preserve unknown/known state", testInvalidMasksPreserveUnknownAndKnownState},
      {"one target snapshot per update", testTargetSnapshot},
      {"Central timezone and exact 2026 DST boundaries", testCentralTimeAnd2026Boundaries},
      {"startup greeting glyphs, physical sequence, and safe holds", testStartupGreetingGlyphsAndPhysicalSequence},
      {"startup greeting cached handoff and disabled unknown state", testStartupGreetingHandoffAndDisabledState},
  };
  std::size_t failures = 0;
  // Run independent cases after a failure, with enough context to locate a bad
  // address, time field, or transition without a debugger. No assert/NDEBUG gap.
  for (const Test &test : tests) {
    context.clear();
    try {
      test.run();
      std::cout << "PASS: " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
    }
  }
  // A failing group always makes the executable fail, including release builds.
  std::cout << (sizeof(tests) / sizeof(tests[0]) - failures) << "/"
            << sizeof(tests) / sizeof(tests[0]) << " test groups passed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
