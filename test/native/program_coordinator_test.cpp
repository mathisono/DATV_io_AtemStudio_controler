#include <ProgramCoordinator.h>

#include <assert.h>
#include <stdio.h>

using datv::ProgramAction;
using datv::ProgramCoordinator;
using datv::ProgramFeedback;

ProgramFeedback feedback(uint16_t program, uint32_t generation = 1, size_t inFlight = 0) {
  ProgramFeedback value;
  value.synchronized = value.valid = true;
  value.program = program;
  value.generation = generation;
  value.inFlight = inFlight;
  return value;
}

void establish(ProgramCoordinator& coordinator, uint16_t program = 0) {
  assert(coordinator.observe(feedback(program), false, true).changed);
  coordinator.reset();
}

void request(ProgramCoordinator& coordinator, const ProgramFeedback& state,
             uint16_t source, uint32_t now) {
  auto decision = coordinator.decide(state, source, now);
  assert(decision.action == ProgramAction::Request && decision.source == source);
  coordinator.recordAttempt(source, true, now);
}

void testExternalOverrideDuringPending() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, 0);
  // An unrelated Program change is external even while our request is pending.
  auto observation = coordinator.observe(feedback(6, 1, 1), true, true);
  assert(observation.changed && observation.external && observation.program == 6);
  assert(ProgramCoordinator::mustAbandonOnPause(true, 1));
  assert(!ProgramCoordinator::mustAbandonOnPause(false, 1));
  assert(!ProgramCoordinator::mustAbandonOnPause(true, 0));
  coordinator.reset();
  coordinator.sessionLost();
  assert(coordinator.pendingSource() == datv::kNoSource);
  assert(coordinator.attempts() == 0);
}

void testExpectedFeedbackAndPauseBeforeAck() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, 0);
  auto own = coordinator.observe(feedback(1, 1, 1), true, true);
  assert(own.changed && !own.external);
  assert(coordinator.pendingSource() == datv::kNoSource);
  // Feedback can beat the packet ACK. Pause must still abandon the in-flight
  // stream even though application-level pendingSource has already cleared.
  assert(ProgramCoordinator::mustAbandonOnPause(true, 1));
  assert(coordinator.decide(feedback(1, 1, 1), 2, 600).action == ProgramAction::None);
  assert(coordinator.decide(feedback(1, 1, 0), 2, 601).action == ProgramAction::Request);
}

void testGapPreservesPendingAndBaseline() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, 0);
  auto gap = feedback(0, 1, 1);
  gap.synchronized = false;
  assert(!coordinator.observe(gap, true, true).changed);
  assert(coordinator.pendingSource() == 1 && coordinator.attempts() == 1);
  auto own = coordinator.observe(feedback(1), true, true);
  assert(own.changed && !own.external);
  gap = feedback(1);
  gap.synchronized = false;
  coordinator.observe(gap, true, true);
  assert(coordinator.observe(feedback(6), true, true).external);
}

void testRetryBudgetSurvivesReconnect() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  for (uint32_t attempt = 1; attempt <= 3; ++attempt) {
    request(coordinator, feedback(0, attempt), 1, attempt * 3000);
    assert(coordinator.attempts() == attempt);
    coordinator.sessionLost();
    coordinator.observe(ProgramFeedback{}, true, true);
    auto fresh = coordinator.observe(feedback(0, attempt + 1), true, true);
    assert(!fresh.external);
    assert(coordinator.attempts() == attempt);
  }
  assert(coordinator.decide(feedback(0, 4), 1, 12000).action == ProgramAction::PauseUnconfirmed);
  // A different current lock decision has a separate, bounded budget.
  assert(coordinator.decide(feedback(0, 4), 2, 12000).action == ProgramAction::Request);
  assert(coordinator.attempts() == 0);
}

void testReconnectOwnVersusExternalProgram() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, 0);
  coordinator.sessionLost();
  // The command may have applied before the connection failed. Fresh state
  // matching that target is expected, not an external takeover.
  auto own = coordinator.observe(feedback(1, 2), true, true);
  assert(own.changed && !own.external && coordinator.attempts() == 0);
  request(coordinator, feedback(1, 2), 2, 500);
  coordinator.sessionLost();
  assert(coordinator.observe(feedback(6, 3), true, true).external);
}

void testSupersededUnacknowledgedDecision() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, 0);
  // Source 1 loses lock while its command is unacknowledged: no command for 2
  // may be queued after it. Restart and decide from fresh switcher feedback.
  assert(coordinator.decide(feedback(0, 1, 1), 2, 20).action == ProgramAction::RestartSession);
  assert(coordinator.pendingSource() == 1);
  coordinator.sessionLost();
  assert(!coordinator.observe(feedback(1, 2), true, true).external);
  auto desired = coordinator.decide(feedback(1, 2), 2, 600);
  assert(desired.action == ProgramAction::Request && desired.source == 2);

  ProgramCoordinator duringGap;
  establish(duringGap);
  request(duringGap, feedback(0), 1, 0);
  auto gap = feedback(0, 1, 1);
  gap.synchronized = false;
  duringGap.observe(gap, true, true);
  assert(duringGap.decide(gap, 2, 20).action == ProgramAction::RestartSession);
}

void testSupersededAcknowledgedDecisionWaitsForFeedback() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, 0);
  assert(coordinator.decide(feedback(0), 2, 100).action == ProgramAction::None);
  assert(coordinator.pendingSource() == 1);  // Keep expected own feedback.
  assert(!coordinator.observe(feedback(1), true, true).external);
  assert(coordinator.decide(feedback(1), 2, 500).action == ProgramAction::Request);

  ProgramCoordinator missing;
  establish(missing);
  request(missing, feedback(0), 1, 0);
  assert(missing.decide(feedback(0), 1, 1999).action == ProgramAction::None);
  assert(missing.decide(feedback(0), 1, 2000).action == ProgramAction::RestartSession);
  missing.sessionLost();
  missing.observe(feedback(0, 2), true, true);
  assert(missing.attempts() == 1);
}

void testCancelBeforeHoldOrAlreadyDesired() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, 0);
  // Even though Program already equals the NEW desired source, an old in-flight
  // command would change it later. Cancel that command before returning idle.
  assert(coordinator.decide(feedback(0, 1, 1), 0, 20).action == ProgramAction::RestartSession);
  coordinator.sessionLost();
  coordinator.observe(feedback(0, 2), true, true);
  assert(coordinator.decide(feedback(0, 2), 0, 500).action == ProgramAction::None);
  request(coordinator, feedback(0, 2), 2, 600);
  assert(coordinator.decide(feedback(0, 2, 1), datv::kNoSource, 610).action == ProgramAction::RestartSession);
  coordinator.sessionLost();
  coordinator.observe(feedback(0, 3), true, true);
  assert(coordinator.decide(feedback(0, 3), datv::kNoSource, 2000).action == ProgramAction::None);
}

void testModeAndTiming() {
  ProgramCoordinator coordinator;
  establish(coordinator);
  request(coordinator, feedback(0), 1, UINT32_MAX - 100);
  coordinator.observe(feedback(1), true, true);
  assert(coordinator.decide(feedback(1), 2, 20).action == ProgramAction::None);
  assert(coordinator.decide(feedback(1), 2, 399).action == ProgramAction::Request);
  coordinator.reset();
  auto manual = coordinator.observe(feedback(6), false, true);
  assert(manual.changed && !manual.external);
  auto transitioning = feedback(6);
  transitioning.inTransition = true;
  assert(coordinator.decide(transitioning, 1, 600).action == ProgramAction::None);
  assert(!coordinator.observe(feedback(5), true, false).external);
}

int main() {
  testExternalOverrideDuringPending();
  testExpectedFeedbackAndPauseBeforeAck();
  testGapPreservesPendingAndBaseline();
  testRetryBudgetSurvivesReconnect();
  testReconnectOwnVersusExternalProgram();
  testSupersededUnacknowledgedDecision();
  testSupersededAcknowledgedDecisionWaitsForFeedback();
  testCancelBeforeHoldOrAlreadyDesired();
  testModeAndTiming();
  puts("Program coordination: 9 race/timing scenario groups passed.");
}
