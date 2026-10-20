// C ABI for the Linux host. The actual state machines are also used by ESP32.
#include <Arbitration.h>
#include <ProgramCoordinator.h>
#include <AtemProtocol.h>
#include <new>

using Send = bool (*)(void*, const uint8_t*, size_t);
using Log = void (*)(const char*);

struct NativeConfig {
  uint32_t acquire, release, interval, policy, conflict, fallback, respectExternal;
  uint32_t enabled[6], activeHigh[6], source[6], priority[6];
};
struct NativeStatus {
  uint32_t automatic, fresh, connected, programValid, previewValid, ftbValid;
  uint32_t program, preview, ftb, transitioning, version, generation, lastRx;
  uint32_t pending, sent, errors, retries, desired, eligible, remaining, restart;
  uint32_t raw[6], locked[6], qualifying[6], inputRemaining[6];
};

class Core {
 public:
  Core(Send send, Log log) : session(send, nullptr), log_(log) {}
  atem::Session session;
  datv::Arbitration arb{datv::ArbitrationConfig{}};
  datv::ProgramCoordinator coordinator;
  datv::ArbitrationConfig config;
  bool fresh = false, restart = false, respectExternal = true;
  uint32_t sent = 0, errors = 0;

  void reconnect() { coordinator.sessionLost(); session.stop(); restart = true; }
  void pause(uint32_t now, const char* reason) {
    const bool abandon = datv::ProgramCoordinator::mustAbandonOnPause(
        arb.automatic(), session.pendingCommands());
    if (arb.automatic()) log_(reason);
    arb.setAutomatic(false, now);
    coordinator.reset();
    if (abandon) reconnect();
  }
  datv::ProgramFeedback feedback() const {
    const auto& s = session.state();
    return {session.connected(), s.programValid, s.program,
            s.connectionGeneration, s.transitionValid && s.inTransition,
            session.pendingCommands()};
  }
  void inputs(uint32_t mask, bool valid, uint32_t now) {
    if (!valid) {
      if (fresh) {
        pause(now, "Arduino status unknown; automation paused");
        arb.configure(config, now); // discard qualified locks across outages
        coordinator.reset();
        log_("Arduino unavailable; waiting for a fresh input snapshot");
      }
      fresh = false;
      return;
    }
    if (!fresh) log_("Arduino input snapshots available; check inputs before resuming Auto");
    fresh = true;
    std::array<bool, datv::kInputCount> raw{};
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = mask & (1u << i);
    arb.sample(raw, now);
  }
  void tick(uint32_t now) {
    session.tick(now);
    const auto observation = coordinator.observe(feedback(), arb.automatic(), respectExternal);
    if (observation.external) pause(now, "External Program change; automation paused");
    if (!arb.automatic() || !fresh) return;
    const auto decision = coordinator.decide(feedback(), arb.selection().source, now);
    switch (decision.action) {
      case datv::ProgramAction::RestartSession:
        log_(decision.reason); reconnect(); break;
      case datv::ProgramAction::PauseUnconfirmed:
        ++errors; pause(now, decision.reason); break;
      case datv::ProgramAction::Request: {
        const bool accepted = session.setProgram(decision.source, now);
        coordinator.recordAttempt(decision.source, accepted, now);
        if (accepted) ++sent; else ++errors;
        break;
      }
      default: break;
    }
  }
 private:
  Log log_;
};

extern "C" {
Core* datv_create(Send send, Log log) { return new (std::nothrow) Core(send, log); }
void datv_destroy(Core* c) { delete c; }
void datv_configure(Core* c, const NativeConfig* v, uint32_t now) {
  c->pause(now, "Configuration applied; automation paused");
  auto& a = c->config;
  a.acquireMs = v->acquire; a.releaseMs = v->release; a.switchIntervalMs = v->interval;
  a.policy = static_cast<datv::MultiPolicy>(v->policy);
  a.conflictSource = v->conflict; a.fallbackSource = v->fallback;
  for (size_t i = 0; i < 6; ++i)
    a.inputs[i] = {v->enabled[i] != 0, v->activeHigh[i] != 0,
                   static_cast<uint16_t>(v->source[i]), static_cast<uint8_t>(v->priority[i])};
  c->respectExternal = v->respectExternal;
  c->arb.configure(a, now);
  c->coordinator.reset();
  c->fresh = false;
}
void datv_start(Core* c, uint32_t now, uint32_t nonce) {
  c->coordinator.sessionLost(); c->restart = false; c->session.start(now, nonce);
}
void datv_disconnect(Core* c) { c->reconnect(); }
void datv_receive(Core* c, const uint8_t* data, size_t size, uint32_t now) {
  c->session.receive(data, size, now);
}
void datv_inputs(Core* c, uint32_t mask, uint32_t fresh, uint32_t now) {
  c->inputs(mask, fresh != 0, now);
}
void datv_tick(Core* c, uint32_t now) { c->tick(now); }
uint32_t datv_restart_due(Core* c, uint32_t now) {
  return c->restart || c->session.readyToRestart(now);
}
uint32_t datv_mode(Core* c, uint32_t automatic, uint32_t now) {
  if (!automatic) { c->pause(now, "Operator paused automatic switching"); return 1; }
  if (!c->fresh || !c->session.connected() || c->session.pendingCommands()) return 0;
  if (!c->arb.automatic()) {
    c->coordinator.reset(); c->arb.setAutomatic(true, now);
  }
  return 1;
}
uint32_t datv_action(Core* c, uint32_t action, uint32_t source, uint32_t now) {
  if (action > 4 || source > 6) return 0;
  if (action != 1) c->pause(now, "Manual on-air action; automation paused");
  bool accepted = false;
  switch (action) {
    case 0: accepted = c->session.setProgram(source, now); break;
    case 1: accepted = c->session.setPreview(source, now); break;
    case 2: accepted = c->session.cut(now); break;
    case 3: accepted = c->session.autoTransition(now); break;
    case 4: accepted = c->session.fadeToBlack(now); break;
  }
  if (accepted) ++c->sent; else ++c->errors;
  return accepted;
}
void datv_status(Core* c, NativeStatus* o) {
  const auto& s = c->session.state(); const auto& stats = c->session.stats();
  const auto& a = c->arb.selection();
  *o = {};
  o->automatic = c->arb.automatic(); o->fresh = c->fresh; o->connected = c->session.connected();
  o->programValid = s.programValid; o->previewValid = s.previewValid; o->ftbValid = s.fadeToBlackValid;
  o->program = s.program; o->preview = s.preview; o->ftb = s.fadeToBlack;
  o->transitioning = s.transitionValid && s.inTransition;
  o->version = s.version; o->generation = s.connectionGeneration; o->lastRx = s.lastRxMs;
  o->pending = c->session.pendingCommands(); o->sent = c->sent;
  o->errors = c->errors + stats.malformedPackets + stats.sendErrors + stats.droppedCommands;
  o->retries = stats.retransmissions; o->desired = a.source;
  o->eligible = a.eligibleCount; o->remaining = a.remainingMs; o->restart = c->restart;
  for (size_t i = 0; i < 6; ++i) {
    const auto& input = c->arb.inputs()[i];
    o->raw[i] = input.rawHigh; o->locked[i] = input.locked;
    o->qualifying[i] = input.qualifying; o->inputRemaining[i] = input.remainingMs;
  }
}
const char* datv_text(Core* c, uint32_t field) {
  switch (field) {
    case 0: return c->session.statusText();
    case 1: return c->session.errorText();
    case 2: return c->session.state().product;
    default: return c->arb.selection().reason;
  }
}
}
