#include "Arbitration.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
void check(bool value, const char* expression, int line) {
  if (!value) {
    std::cerr << "Arbitration test failed at line " << line << ": " << expression << '\n';
    std::exit(1);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

datv::ArbitrationConfig config() {
  datv::ArbitrationConfig value;
  for (std::size_t i = 0; i < datv::kInputCount; ++i) {
    value.inputs[i].source = static_cast<uint16_t>(i + 1);
    value.inputs[i].priority = static_cast<uint8_t>(i + 1);
  }
  return value;
}

void qualificationAndGlitches() {
  auto settings = config();
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{};
  controller.sample(raw, 100);
  CHECK(!controller.automatic());
  CHECK(controller.selection().source == datv::kNoSource);
  controller.setAutomatic(true, 100);
  CHECK(controller.selection().source == 0);

  raw[0] = true;
  controller.sample(raw, 200);
  CHECK(controller.inputs()[0].qualifying);
  CHECK(controller.inputs()[0].remainingMs == 1000);
  controller.sample(raw, 1199);
  CHECK(!controller.inputs()[0].locked);
  controller.sample(raw, 1200);
  CHECK(controller.inputs()[0].locked);
  CHECK(controller.selection().source == 1);

  raw[0] = false;
  controller.sample(raw, 1300);
  CHECK(controller.inputs()[0].locked);
  CHECK(controller.inputs()[0].qualifying);
  raw[0] = true;  // A short loss does not cut away from the source.
  controller.sample(raw, 2000);
  CHECK(controller.inputs()[0].locked);
  CHECK(!controller.inputs()[0].qualifying);
  raw[0] = false;
  controller.sample(raw, 2100);
  controller.sample(raw, 4099);
  CHECK(controller.selection().source == 1);
  controller.sample(raw, 4100);
  CHECK(!controller.inputs()[0].locked);
  CHECK(controller.selection().source == 0);
}

void fiveMinuteRotation() {
  auto settings = config();
  settings.acquireMs = settings.releaseMs = 0;
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{true, false, false, false, false, false};
  controller.sample(raw, 0);
  controller.setAutomatic(true, 0);
  CHECK(controller.selection().source == 1);
  raw[1] = true;
  controller.sample(raw, 60000);
  CHECK(controller.selection().remainingMs == 300000);
  raw[2] = true;
  controller.sample(raw, 120000);
  CHECK(controller.selection().remainingMs == 240000);  // A join cannot restart dwell.
  controller.sample(raw, 359999);
  CHECK(controller.selection().source == 1);
  controller.sample(raw, 360000);
  CHECK(controller.selection().source == 2);
  controller.sample(raw, 660000);
  CHECK(controller.selection().source == 3);
  controller.sample(raw, 960000);
  CHECK(controller.selection().source == 1);
}

void allSixInputsAndLoss() {
  auto settings = config();
  settings.acquireMs = settings.releaseMs = 0;
  settings.switchIntervalMs = 1000;
  settings.inputs[5].priority = 1;
  settings.inputs[0].priority = 2;
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{true, true, true, true, true, true};
  controller.sample(raw, 0);
  controller.setAutomatic(true, 0);
  CHECK(controller.selection().source == 6);
  for (uint32_t source = 1; source <= 6; ++source) {
    controller.sample(raw, source * 1000);
    CHECK(controller.selection().source == source);
  }
  raw[5] = false;
  controller.sample(raw, 6001);
  CHECK(controller.selection().source == 1);  // Failed selected source is skipped immediately.
  raw[0] = raw[1] = raw[2] = raw[3] = false;
  controller.sample(raw, 6002);
  CHECK(controller.selection().source == 5);
  CHECK(controller.selection().remainingMs == 0);
}

void priorityAndMapping() {
  auto settings = config();
  settings.acquireMs = settings.releaseMs = 0;
  settings.policy = datv::MultiPolicy::Priority;
  settings.inputs[0].source = 6;
  settings.inputs[5].source = 1;
  settings.inputs[0].priority = 4;
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{true, false, false, false, false, false};
  controller.sample(raw, 0);
  controller.setAutomatic(true, 0);
  CHECK(controller.selection().source == 6);
  raw[1] = true;
  controller.sample(raw, 1);
  CHECK(controller.selection().source == 2);
  controller.sample(raw, 900000);
  CHECK(controller.selection().source == 2);
  raw[1] = false;
  controller.sample(raw, 900001);
  CHECK(controller.selection().source == 6);
}

void fixedConflictOutput() {
  auto settings = config();
  settings.acquireMs = settings.releaseMs = 0;
  settings.policy = datv::MultiPolicy::Conflict;
  settings.conflictSource = 6;
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{true, true, false, false, false, false};
  controller.sample(raw, 100);
  controller.setAutomatic(true, 100);
  CHECK(controller.selection().source == 1);
  controller.sample(raw, 300099);
  CHECK(controller.selection().source == 1);
  controller.sample(raw, 300100);
  CHECK(controller.selection().source == 6);  // Designated output need not have its own lock.
  CHECK(std::string(controller.selection().reason) == "conflict_output");
  raw[1] = false;
  controller.sample(raw, 300101);
  CHECK(controller.selection().source == 1);
  raw[1] = true;
  controller.sample(raw, 300102);
  CHECK(controller.selection().remainingMs == 300000);  // Competition must be continuous.
  raw[0] = raw[1] = false;
  controller.sample(raw, 300103);
  CHECK(controller.selection().source == 0);
}

void manualPauseAndReconfigure() {
  auto settings = config();
  settings.acquireMs = settings.releaseMs = 0;
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{true, true, false, false, false, false};
  controller.sample(raw, 0);
  controller.setAutomatic(true, 0);
  controller.setAutomatic(false, 100);
  controller.sample(raw, 900000);
  CHECK(controller.selection().source == datv::kNoSource);
  CHECK(controller.inputs()[0].locked);  // Monitoring remains live while paused.
  controller.setAutomatic(true, 900001);
  CHECK(controller.selection().source == 1);
  CHECK(controller.selection().remainingMs == 300000);
  controller.configure(settings, 900002);
  CHECK(!controller.automatic());
  CHECK(!controller.inputs()[0].locked);
  CHECK(controller.selection().source == datv::kNoSource);
}

void polarityAndDisabledInputs() {
  auto settings = config();
  settings.acquireMs = settings.releaseMs = 0;
  for (auto& rule : settings.inputs) rule.enabled = false;
  settings.inputs[0].enabled = true;
  settings.inputs[0].activeHigh = false;
  settings.inputs[0].source = 6;
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{true, true, true, true, true, true};
  controller.sample(raw, 0);
  controller.setAutomatic(true, 0);
  CHECK(controller.selection().source == 0);
  raw[0] = false;  // Receiver HIGH drives the optocoupler, producing GPIO LOW.
  controller.sample(raw, 1);
  CHECK(controller.selection().source == 6);
  CHECK(controller.selection().eligibleCount == 1);
  CHECK(!controller.inputs()[1].locked);
}

void wrappingClock() {
  auto settings = config();
  settings.acquireMs = 100;
  settings.releaseMs = 50;
  const uint32_t start = std::numeric_limits<uint32_t>::max() - 50;
  datv::Arbitration controller(settings);
  std::array<bool, 6> raw{true, false, false, false, false, false};
  controller.sample(raw, start);
  controller.setAutomatic(true, start);
  controller.sample(raw, start + 99);
  CHECK(controller.selection().source == 0);
  controller.sample(raw, start + 100);
  CHECK(controller.selection().source == 1);
  raw[0] = false;
  controller.sample(raw, start + 120);
  controller.sample(raw, start + 169);
  CHECK(controller.selection().source == 1);
  controller.sample(raw, start + 170);
  CHECK(controller.selection().source == 0);

  settings.acquireMs = settings.releaseMs = 0;
  settings.switchIntervalMs = 200;
  controller.configure(settings, start);
  raw[0] = raw[1] = true;
  controller.sample(raw, start);
  controller.setAutomatic(true, start);
  controller.sample(raw, start + 199);
  CHECK(controller.selection().source == 1);
  controller.sample(raw, start + 200);
  CHECK(controller.selection().source == 2);
  CHECK(controller.selection().remainingMs == 200);

  settings.policy = datv::MultiPolicy::Conflict;
  settings.conflictSource = 6;
  settings.switchIntervalMs = 1000;
  controller.configure(settings, 100);
  controller.sample(raw, 100);
  controller.setAutomatic(true, 100);
  controller.sample(raw, 1200);
  CHECK(controller.selection().source == 6);
  controller.sample(raw, std::numeric_limits<uint32_t>::max());
  CHECK(controller.selection().source == 6);
  controller.sample(raw, 101);  // Competition persists through an entire clock cycle.
  CHECK(controller.selection().source == 6);
}
}  // namespace

int main() {
  qualificationAndGlitches();
  fiveMinuteRotation();
  allSixInputsAndLoss();
  priorityAndMapping();
  fixedConflictOutput();
  manualPauseAndReconfigure();
  polarityAndDisabledInputs();
  wrappingClock();
  std::cout << "All 8 arbitration scenario groups passed.\n";
}
