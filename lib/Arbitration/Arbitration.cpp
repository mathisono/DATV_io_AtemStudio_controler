#include "Arbitration.h"

namespace datv {
Arbitration::Arbitration(const ArbitrationConfig& config) : config_(config) {}

void Arbitration::configure(const ArbitrationConfig& config, uint32_t now) {
  config_ = config;
  states_ = {};
  qualification_ = {};
  automatic_ = false;
  competing_ = false;
  conflictActive_ = false;
  selectedIndex_ = -1;
  selection_ = {};
  selectedSince_ = competitionSince_ = now;
}

void Arbitration::setAutomatic(bool enabled, uint32_t now) {
  if (automatic_ == enabled) return;
  automatic_ = enabled;
  selectedIndex_ = -1;
  competing_ = false;
  conflictActive_ = false;
  selectedSince_ = competitionSince_ = now;
  evaluate(now);
}

void Arbitration::sample(const std::array<bool, kInputCount>& electricalHigh,
                         uint32_t now) {
  for (std::size_t i = 0; i < kInputCount; ++i) {
    auto& state = states_[i];
    auto& q = qualification_[i];
    const auto& rule = config_.inputs[i];
    state.rawHigh = electricalHigh[i];
    const bool asserted = rule.enabled && (electricalHigh[i] == rule.activeHigh);
    if (!q.initialized || q.candidate != asserted) {
      q.initialized = true;
      q.candidate = asserted;
      q.since = now;
    }
    if (!rule.enabled) state.locked = false;
    const uint32_t threshold = asserted ? config_.acquireMs : config_.releaseMs;
    const uint32_t elapsed = now - q.since;
    state.qualifying = asserted != state.locked;
    state.remainingMs = state.qualifying && elapsed < threshold ? threshold - elapsed : 0;
    if (state.qualifying && elapsed >= threshold) {
      state.locked = asserted;
      state.qualifying = false;
      state.remainingMs = 0;
    }
  }
  evaluate(now);
}

int Arbitration::firstEligible() const {
  int result = -1;
  for (std::size_t i = 0; i < kInputCount; ++i) {
    if (!config_.inputs[i].enabled || !states_[i].locked) continue;
    if (result < 0 || config_.inputs[i].priority < config_.inputs[result].priority)
      result = static_cast<int>(i);
  }
  return result;
}

int Arbitration::nextEligible(int after) const {
  // Round robin follows physical alarm-channel order, skipping disabled/unlocked
  // channels. Priority determines only the first selection and priority policy.
  for (std::size_t offset = 1; offset <= kInputCount; ++offset) {
    const int index = (after + static_cast<int>(offset)) % static_cast<int>(kInputCount);
    if (config_.inputs[index].enabled && states_[index].locked) return index;
  }
  return -1;
}

void Arbitration::choose(int index, uint32_t now) {
  if (selectedIndex_ != index) selectedSince_ = now;
  selectedIndex_ = index;
  selection_.source = index < 0 ? config_.fallbackSource : config_.inputs[index].source;
}

void Arbitration::evaluate(uint32_t now) {
  selection_.eligibleCount = 0;
  selection_.remainingMs = 0;
  for (std::size_t i = 0; i < kInputCount; ++i)
    if (config_.inputs[i].enabled && states_[i].locked) ++selection_.eligibleCount;
  if (!automatic_) {
    selection_.source = kNoSource;
    selection_.reason = "manual";
    competing_ = false;
    conflictActive_ = false;
    return;
  }
  if (selection_.eligibleCount == 0) {
    choose(-1, now);
    selection_.reason = "no_lock_fallback";
    competing_ = false;
    conflictActive_ = false;
    return;
  }
  if (selectedIndex_ < 0 || !states_[selectedIndex_].locked ||
      !config_.inputs[selectedIndex_].enabled) choose(firstEligible(), now);
  if (selection_.eligibleCount == 1) {
    choose(firstEligible(), now);
    selection_.reason = "single_lock";
    competing_ = false;
    conflictActive_ = false;
    return;
  }
  if (!competing_) {
    competing_ = true;
    conflictActive_ = false;
    competitionSince_ = selectedSince_ = now;
  }
  if (config_.policy == MultiPolicy::Priority) {
    choose(firstEligible(), now);
    selection_.reason = "priority";
    return;
  }
  if (config_.policy == MultiPolicy::Conflict) {
    const uint32_t elapsed = now - competitionSince_;
    // Once expired, remain on the conflict output even over a complete millis()
    // cycle. Only the end of competition or an operator reset clears the latch.
    if (elapsed >= config_.switchIntervalMs) conflictActive_ = true;
    if (conflictActive_) {
      selection_.source = config_.conflictSource;
      selection_.reason = "conflict_output";
    } else {
      selection_.source = config_.inputs[selectedIndex_].source;
      selection_.reason = "conflict_wait";
      selection_.remainingMs = config_.switchIntervalMs - elapsed;
    }
    return;
  }
  if (static_cast<uint32_t>(now - selectedSince_) >= config_.switchIntervalMs) {
    choose(nextEligible(selectedIndex_), now);
    selectedSince_ = now;
  }
  selection_.source = config_.inputs[selectedIndex_].source;
  selection_.reason = "round_robin";
  selection_.remainingMs = config_.switchIntervalMs - static_cast<uint32_t>(now - selectedSince_);
}
}  // namespace datv
