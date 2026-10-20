#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace datv {
constexpr std::size_t kInputCount = 6;
constexpr uint16_t kNoSource = 0xffff;

enum class MultiPolicy : uint8_t { RoundRobin, Priority, Conflict };

struct InputRule {
  bool enabled = true;
  bool activeHigh = true;
  uint16_t source = 1;
  uint8_t priority = 1;
};

struct ArbitrationConfig {
  std::array<InputRule, kInputCount> inputs{};
  uint32_t acquireMs = 1000;
  uint32_t releaseMs = 2000;
  uint32_t switchIntervalMs = 300000;
  MultiPolicy policy = MultiPolicy::RoundRobin;
  uint16_t conflictSource = 0;
  uint16_t fallbackSource = 0;
};

struct InputState {
  bool rawHigh = false;
  bool locked = false;
  bool qualifying = false;
  uint32_t remainingMs = 0;
};

struct Selection {
  uint16_t source = kNoSource;
  const char* reason = "manual";
  uint8_t eligibleCount = 0;
  uint32_t remainingMs = 0;
};

// A hardware-independent state machine. All timing uses unsigned subtraction so
// millis() wrapping at ~49 days does not change qualification or dwell behavior.
class Arbitration {
 public:
  explicit Arbitration(const ArbitrationConfig& config);
  void configure(const ArbitrationConfig& config, uint32_t now);
  void setAutomatic(bool enabled, uint32_t now);
  bool automatic() const { return automatic_; }
  void sample(const std::array<bool, kInputCount>& electricalHigh, uint32_t now);
  const Selection& selection() const { return selection_; }
  const std::array<InputState, kInputCount>& inputs() const { return states_; }

 private:
  struct Qualification {
    bool initialized = false;
    bool candidate = false;
    uint32_t since = 0;
  };
  ArbitrationConfig config_;
  std::array<InputState, kInputCount> states_{};
  std::array<Qualification, kInputCount> qualification_{};
  Selection selection_{};
  bool automatic_ = false;
  bool competing_ = false;
  bool conflictActive_ = false;
  uint32_t competitionSince_ = 0;
  uint32_t selectedSince_ = 0;
  int selectedIndex_ = -1;
  void evaluate(uint32_t now);
  int firstEligible() const;
  int nextEligible(int after) const;
  void choose(int index, uint32_t now);
};
}  // namespace datv
