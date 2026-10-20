#include "ProgramCoordinator.h"

namespace datv {
namespace {
constexpr uint32_t kRequestSpacingMs = 500;
constexpr uint32_t kConfirmationTimeoutMs = 2000;
constexpr uint8_t kMaximumAttempts = 3;
}

void ProgramCoordinator::reset() {
  desired_ = pending_ = possiblyApplied_ = kNoSource;
  attempts_ = 0;
  haveAttempt_ = false;
}

void ProgramCoordinator::sessionLost() {
  if (pending_ != kNoSource) possiblyApplied_ = pending_;
  pending_ = kNoSource;
  resynchronizing_ = true;
  // Deliberately keep desired_, attempts_ and lastProgram_. Repeated network
  // failure must not give one unconfirmed source an unlimited request budget.
}

ProgramObservation ProgramCoordinator::observe(const ProgramFeedback& feedback,
                                               bool automatic,
                                               bool respectExternalControl) {
  ProgramObservation result;
  if (!feedback.synchronized || !feedback.valid) {
    if (!feedback.valid) sessionLost();
    return result;
  }
  if (haveGeneration_ && feedback.generation != lastGeneration_) sessionLost();
  lastGeneration_ = feedback.generation;
  haveGeneration_ = true;
  const bool expectedOwnChange = feedback.program == pending_ ||
                                (resynchronizing_ && feedback.program == possiblyApplied_);
  result.changed = !haveProgram_ || feedback.program != lastProgram_;
  result.external = result.changed && haveProgram_ && automatic &&
                    respectExternalControl && !expectedOwnChange;
  result.program = feedback.program;
  // Update from the snapshot before the caller possibly resets the transport.
  lastProgram_ = feedback.program;
  haveProgram_ = true;
  if (feedback.program == pending_) pending_ = kNoSource;
  if (feedback.program == desired_) attempts_ = 0;
  possiblyApplied_ = kNoSource;
  resynchronizing_ = false;
  return result;
}

ProgramDecision ProgramCoordinator::decide(const ProgramFeedback& feedback,
                                          uint16_t desired, uint32_t now) {
  ProgramDecision result;
  if (desired != desired_) {
    desired_ = desired;
    attempts_ = 0;
    // Do not discard pending_: its expected feedback can still be on the wire.
    // Do not reset request spacing merely because a different source won.
  }
  // Loss of feedback does not make an obsolete outgoing command safe to keep
  // retransmitting. Cancel it even during a recoverable receive sequence gap.
  if (pending_ != kNoSource && pending_ != desired && feedback.inFlight > 0) {
    result.action = ProgramAction::RestartSession;
    result.reason = "Source decision changed; abandoning an unacknowledged Program request";
    return result;
  }
  if (!feedback.synchronized || !feedback.valid) return result;
  if (pending_ != kNoSource) {
    if (static_cast<uint32_t>(now - pendingSince_) >= kConfirmationTimeoutMs) {
      // ACK without matching Program feedback is not a successful take. Reset
      // rather than stacking another command onto an ambiguous old session.
      result.action = ProgramAction::RestartSession;
      result.reason = "Program confirmation timed out; requesting fresh ATEM state";
    }
    return result;
  }
  if (desired == kNoSource) return result;
  if (feedback.program == desired) {
    attempts_ = 0;
    return result;
  }
  if (feedback.inTransition || feedback.inFlight > 0) return result;
  if (attempts_ >= kMaximumAttempts) {
    result.action = ProgramAction::PauseUnconfirmed;
    result.reason = "Program not confirmed after three requests; automation paused";
    return result;
  }
  if (haveAttempt_ && static_cast<uint32_t>(now - lastAttemptAt_) < kRequestSpacingMs) return result;
  result.action = ProgramAction::Request;
  result.source = desired;
  return result;
}

void ProgramCoordinator::recordAttempt(uint16_t source, bool accepted, uint32_t now) {
  if (source != desired_) {
    desired_ = source;
    attempts_ = 0;
  }
  if (attempts_ < kMaximumAttempts) ++attempts_;
  haveAttempt_ = true;
  lastAttemptAt_ = now;
  if (accepted) {
    pending_ = source;
    pendingSince_ = now;
  }
}

}  // namespace datv
