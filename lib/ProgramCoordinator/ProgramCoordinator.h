#pragma once

#include <Arbitration.h>
#include <stddef.h>
#include <stdint.h>

namespace datv {

struct ProgramFeedback {
  bool synchronized = false;
  bool valid = false;
  uint16_t program = 0;
  uint32_t generation = 0;
  bool inTransition = false;
  size_t inFlight = 0;
};

struct ProgramObservation {
  bool changed = false;
  bool external = false;
  uint16_t program = 0;
};

enum class ProgramAction : uint8_t { None, Request, RestartSession, PauseUnconfirmed };

struct ProgramDecision {
  ProgramAction action = ProgramAction::None;
  uint16_t source = kNoSource;
  const char* reason = "";
};

// Coordinates decisions with actual Program feedback. It never queues an
// obsolete source behind an unresolved automatic request. All timing and state
// are independent of Arduino/networking, allowing loss/override races to be
// reproduced in native tests.
class ProgramCoordinator {
 public:
  // Explicit Pause/Resume/configuration reset the decision budget. The latest
  // observed Program baseline remains useful when control mode changes.
  void reset();
  // A real lost/replaced UDP session drops work but retains the retry budget and
  // one possibly-applied target until fresh state arrives. A feedback packet
  // gap does NOT call this function.
  void sessionLost();
  ProgramObservation observe(const ProgramFeedback& feedback, bool automatic,
                             bool respectExternalControl);
  ProgramDecision decide(const ProgramFeedback& feedback, uint16_t desired,
                         uint32_t now);
  void recordAttempt(uint16_t source, bool accepted, uint32_t now);
  uint8_t attempts() const { return attempts_; }
  uint16_t pendingSource() const { return pending_; }
  uint16_t desiredSource() const { return desired_; }
  static bool mustAbandonOnPause(bool automatic, size_t inFlight) {
    return automatic && inFlight > 0;
  }

 private:
  uint16_t desired_ = kNoSource;
  uint16_t pending_ = kNoSource;
  uint16_t possiblyApplied_ = kNoSource;
  uint16_t lastProgram_ = 0;
  uint32_t lastGeneration_ = 0;
  uint32_t pendingSince_ = 0;
  uint32_t lastAttemptAt_ = 0;
  uint8_t attempts_ = 0;
  bool haveAttempt_ = false;
  bool haveProgram_ = false;
  bool haveGeneration_ = false;
  bool resynchronizing_ = true;
};

}  // namespace datv
