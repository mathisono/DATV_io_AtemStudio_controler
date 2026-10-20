#include <AtemProtocol.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <vector>

using Bytes = std::vector<uint8_t>;

struct Wire {
  std::vector<Bytes> sent;
  bool failSend = false;
  static bool send(void* context, const uint8_t* data, size_t length) {
    auto& self = *static_cast<Wire*>(context);
    if (self.failSend) return false;
    self.sent.emplace_back(data, data + length);
    return true;
  }
};

Bytes packet(uint8_t flags, uint16_t session, uint16_t sequence,
             const Bytes& payload = {}, uint16_t ack = 0, uint16_t resend = 0) {
  Bytes result(atem::kHeaderSize + payload.size(), 0);
  atem::Header h;
  h.flags = flags;
  h.length = static_cast<uint16_t>(result.size());
  h.session = session;
  h.sequence = sequence;
  h.ack = ack;
  h.retransmit = resend;
  atem::encodeHeader(result.data(), h);
  if (!payload.empty()) memcpy(result.data() + atem::kHeaderSize, payload.data(), payload.size());
  return result;
}

Bytes block(const char name[4], const Bytes& body = {}) {
  Bytes result(8 + body.size(), 0);
  atem::write16(result.data(), static_cast<uint16_t>(result.size()));
  memcpy(result.data() + 4, name, 4);
  if (!body.empty()) memcpy(result.data() + 8, body.data(), body.size());
  return result;
}

void append(Bytes& destination, const Bytes& item) {
  destination.insert(destination.end(), item.begin(), item.end());
}

void receive(atem::Session& session, const Bytes& bytes, uint32_t now) {
  session.receive(bytes.data(), bytes.size(), now);
}

void hello(atem::Session& session, uint32_t now = 0, uint16_t baseSequence = 0) {
  session.start(now, 0x53ab);
  receive(session, packet(atem::NewSession, 0x53ab, baseSequence, {2, 0, 0, 0, 0, 0, 0, 0}), now + 1);
  assert(!session.connected());
}

void initialize(atem::Session& session, uint32_t now = 0, uint16_t baseSequence = 0) {
  hello(session, now, baseSequence);
  Bytes payload = block("PrgI", {0, 0, 0, 1});
  append(payload, block("PrvI", {0, 0, 0, 2}));
  append(payload, block("InCm"));
  receive(session, packet(atem::AckRequest, 0x8001, atem::nextSequence(baseSequence), payload), now + 2);
  assert(session.connected());
  assert(session.state().program == 1 && session.state().preview == 2);
  assert(session.sessionId() == 0x8001);
}

void testHandshakeAndStateGate() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  assert(!session.cut(0));
  hello(session);
  const Bytes expectedHello = {0x10, 0x14, 0x53, 0xab, 0, 0, 0, 0, 0, 0x3a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
  assert(wire.sent[0] == expectedHello);
  assert(wire.sent[1][0] == 0x80 && wire.sent[1][9] == 3);
  assert(!session.setProgram(6, 2));
  receive(session, packet(atem::AckRequest, 0x8001, 1, block("InCm")), 3);
  assert(!session.connected());
  receive(session, packet(atem::AckRequest, 0x8001, 2, block("PrgI", {0, 0, 0, 3})), 4);
  assert(!session.connected());
  receive(session, packet(atem::AckRequest, 0x8001, 3, block("PrvI", {0, 0, 0, 5})), 5);
  assert(session.connected());
  assert(session.state().program == 3 && session.state().preview == 5);
  assert(session.state().connectionGeneration == 1);

  // A new session invalidates old feedback and never inherits ready state.
  hello(session, 10);
  assert(!session.state().programValid && !session.state().previewValid);
  Bytes payload = block("PrgI", {0, 0, 0, 4});
  append(payload, block("PrvI", {0, 0, 0, 6}));
  receive(session, packet(atem::AckRequest, 0x8002, 1, payload), 12);
  assert(!session.connected());
  receive(session, packet(atem::AckRequest, 0x8002, 2), 13);
  assert(session.connected());  // Older empty-packet initialization marker.
  assert(session.state().connectionGeneration == 2);
}

void testCommandWireFormatAndFeedback() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  initialize(session);
  wire.sent.clear();
  assert(session.setProgram(6, 10));
  assert(session.setPreview(1000, 11));
  assert(session.cut(12));
  assert(session.autoTransition(13));
  assert(session.fadeToBlack(14));
  const char* names[] = {"CPgI", "CPvI", "DCut", "DAut", "FtbA"};
  for (size_t i = 0; i < 5; ++i) {
    const auto& p = wire.sent[i];
    assert(p.size() == 24 && p[0] == 0x08 && p[1] == 0x18);
    assert(atem::read16(p.data() + 2) == 0x8001);
    assert(atem::read16(p.data() + 10) == i + 1);
    assert(atem::read16(p.data() + 12) == 12);
    assert(memcmp(p.data() + 16, names[i], 4) == 0);
    assert(p[20] == 0 && p[21] == 0);
  }
  assert(atem::read16(wire.sent[0].data() + 22) == 6);
  assert(atem::read16(wire.sent[1].data() + 22) == 1000);
  assert(session.state().program == 1);  // No optimistic mutation.
  receive(session, packet(atem::AckReply, 0x8001, 0, {}, 5), 20);
  assert(session.pendingCommands() == 0 && session.stats().acknowledgedCommands == 5);
  assert(session.state().program == 1);  // ACK is transport delivery, not tally.
  Bytes payload = block("PrgI", {0, 0, 0, 6});
  append(payload, block("FtbS", {0, 1, 0, 0}));
  append(payload, block("TrPs", {0, 1, 20, 0, 0, 100}));
  append(payload, block("_ver", {0, 2, 0, 28}));
  append(payload, block("_pin", {'A', 'T', 'E', 'M', 0}));
  receive(session, packet(atem::AckRequest, 0x8001, 2, payload), 21);
  assert(session.state().program == 6);
  assert(session.state().fadeToBlackValid && session.state().fadeToBlack);
  assert(session.state().transitionValid && session.state().inTransition);
  assert(session.state().version == 0x0002001c);
  assert(strcmp(session.state().product, "ATEM") == 0);
}

void testLossRetryAndNoStaleReplay() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  initialize(session);
  wire.sent.clear();
  assert(session.fadeToBlack(20));
  const Bytes original = wire.sent.back();
  session.tick(100);
  assert(wire.sent.size() == 2);
  Bytes retransmitted = wire.sent.back();
  assert((retransmitted[0] >> 3) == (atem::AckRequest | atem::IsRetransmit));
  retransmitted[0] &= static_cast<uint8_t>(~(atem::IsRetransmit << 3));
  assert(retransmitted == original);  // Same payload AND sequence for a toggle.
  receive(session, packet(atem::AckReply, 0x8001, 0, {}, 2), 101);
  assert(session.pendingCommands() == 1);  // A future ACK must be ignored.
  receive(session, packet(atem::AckReply, 0x8001, 0, {}, 1), 102);
  assert(session.pendingCommands() == 0);
  assert(session.cut(110));
  session.tick(1010);
  assert(!session.connected() && session.pendingCommands() == 0);
  assert(session.lastError() == atem::Error::CommandTimeout);
  assert(session.stats().droppedCommands == 1);
  assert(!session.cut(1011));
  assert(!session.readyToRestart(2509) && session.readyToRestart(2510));
  wire.sent.clear();
  initialize(session, 2510);
  // Only hello/ACKs were emitted during reconnection. No stale CUT or FTB.
  for (const auto& p : wire.sent) assert(p.size() != 24);
  assert(session.cut(2520));
  assert(atem::read16(wire.sent.back().data() + 10) == 1);
}

void testReorderDuplicatesAndGap() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  initialize(session);
  receive(session, packet(atem::AckRequest, 0x8001, 3, block("PrgI", {0, 0, 0, 5})), 10);
  assert(session.state().program == 1);
  assert(!session.connected());
  assert(!session.setProgram(2, 11));  // No commands while feedback has a gap.
  assert(atem::read16(wire.sent.back().data() + 4) == 1);  // Never ACK 3 yet.
  receive(session, packet(atem::AckRequest, 0x8001, 2, block("PrgI", {0, 0, 0, 4})), 12);
  assert(session.connected() && session.state().program == 5);
  assert(atem::read16(wire.sent.back().data() + 4) == 3);
  receive(session, packet(atem::AckRequest | atem::IsRetransmit, 0x8001, 2,
                          block("PrgI", {0, 0, 0, 4})), 13);
  assert(session.state().program == 5);  // Never roll back on old feedback.

  // More out-of-order packets than the fixed window cannot exhaust memory or
  // incorrectly acknowledge a gap. Repeated traffic must not hide a stall.
  for (uint16_t n = 5; n <= 20; ++n) {
    receive(session, packet(atem::AckRequest, 0x8001, n), 20 + n);
  }
  assert(session.stats().outOfOrderDrops > 0);
  receive(session, packet(atem::AckRequest, 0x8001, 20), 3010);
  session.tick(3025);
  assert(session.lastError() == atem::Error::ReceiveGapTimeout);
}

void testSequenceWrapBothDirections() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  initialize(session, 0, 32766);
  receive(session, packet(atem::AckRequest, 0x8001, 0, block("PrgI", {0, 0, 0, 3})), 3);
  assert(session.state().program == 3);
  receive(session, packet(atem::AckRequest, 0x8001, 32767, block("PrgI", {0, 0, 0, 2})), 4);
  assert(session.state().program == 3);
  assert(atem::coveredByAck(0, 32767) && !atem::coveredByAck(32767, 0));
  // Exercise an actual full command sequence rollover, including zero.
  for (uint32_t n = 1; n <= 32769; ++n) {
    wire.sent.clear();
    assert(session.setProgram(1, n + 10));
    const uint16_t sequence = static_cast<uint16_t>(n & 0x7fff);
    assert(atem::read16(wire.sent.back().data() + 10) == sequence);
    receive(session, packet(atem::AckReply, 0x8001, 0, {}, sequence), n + 10);
    assert(session.pendingCommands() == 0);
  }
}

void testClockWrapAndSessionIsolation() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  const uint32_t start = UINT32_MAX - 100;
  initialize(session, start);
  assert(session.cut(start + 10));
  session.tick(20);  // 111 ms after command despite uint32_t wrap.
  assert(session.stats().retransmissions == 1);
  receive(session, packet(atem::AckReply, 0x8123, 0, {}, 1), 21);
  assert(session.pendingCommands() == 1 && session.stats().foreignPackets == 1);
  receive(session, packet(atem::AckReply, 0x8001, 0, {}, 1), 22);
  assert(session.pendingCommands() == 0);
  receive(session, packet(atem::AckRequest, 0x8123, 2, block("PrgI", {0, 0, 0, 6})), 5000);
  assert(session.state().program == 1);
  session.tick(5022);
  assert(session.lastError() == atem::Error::ReceiveTimeout);
}

void testRetransmitRequestsAndQueueBound() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  initialize(session);
  for (size_t n = 0; n < atem::kMaxPendingCommands; ++n) assert(session.setProgram(1, 10));
  assert(!session.setProgram(2, 11));
  assert(session.pendingCommands() == atem::kMaxPendingCommands);
  wire.sent.clear();
  receive(session, packet(atem::RetransmitRequest, 0x8001, 0, {}, 0, 3), 20);
  assert(wire.sent.size() == atem::kMaxPendingCommands - 2);
  assert(atem::read16(wire.sent.front().data() + 10) == 3);
  receive(session, packet(atem::AckReply, 0x8001, 0, {}, 8), 21);
  assert(session.pendingCommands() == 0);
  receive(session, packet(atem::RetransmitRequest, 0x8001, 0, {}, 0, 3), 22);
  assert(session.lastError() == atem::Error::RetransmitUnavailable);
  assert(!session.connected());
}

void testMalformedPacketsAndAtomicParsing() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  initialize(session);
  for (size_t size = 0; size < 12; ++size) receive(session, Bytes(size), 10);
  Bytes bad = packet(atem::AckRequest, 0x8001, 2, block("PrgI", {0, 0, 0, 6}));
  bad[1] = static_cast<uint8_t>(bad[1] + 1);
  receive(session, bad, 11);
  Bytes payload = block("PrgI", {0, 0, 0, 6});
  append(payload, {0, 0, 0, 0, 'B', 'A', 'D', '!'});  // Zero-sized next command.
  receive(session, packet(atem::AckRequest, 0x8001, 2, payload), 12);
  assert(session.state().program == 1);  // First block must not apply either.
  receive(session, packet(atem::AckRequest, 0x8001, 2, block("PrgI", {0})), 13);
  assert(session.state().program == 1);
  receive(session, packet(atem::AckRequest, 0x8001, 0x8000), 14);
  receive(session, packet(atem::AckReply, 0x8001, 0, {}, 0x8000), 15);
  assert(session.stats().malformedPackets == 17);
  receive(session, packet(atem::AckRequest, 0x8001, 2, block("PrgI", {0, 0, 0, 3})), 16);
  assert(session.state().program == 3);

  // Exercise all lengths and arbitrary binary data under ASan/UBSan. This is a
  // bounds test, not a claim to validate compatibility against real hardware.
  uint32_t random = 0x51a74913;
  for (size_t size = 0; size < 2200; ++size) {
    Bytes fuzz(size);
    for (auto& byte : fuzz) {
      random = random * 1664525u + 1013904223u;
      byte = static_cast<uint8_t>(random >> 24);
    }
    atem::Header h;
    atem::decodeHeader(fuzz.data(), fuzz.size(), h);
    atem::validateCommandBlocks(fuzz.data(), fuzz.size());
    receive(session, fuzz, 17);
  }
}

void testTimeoutAndSendFailure() {
  Wire wire;
  atem::Session session(&Wire::send, &wire);
  session.start(0, 0x53ab);
  session.tick(1000);
  assert(wire.sent.size() == 2);
  session.tick(5000);
  assert(session.lastError() == atem::Error::HandshakeTimeout);
  hello(session, 10000);
  receive(session, packet(atem::AckRequest, 0x8001, 1), 14000);
  receive(session, packet(atem::AckRequest, 0x8001, 2), 18000);
  session.tick(20000);
  assert(session.lastError() == atem::Error::InitialStateTimeout);
  initialize(session, 21000);
  wire.failSend = true;
  assert(!session.cut(21010));
  assert(!session.connected() && session.pendingCommands() == 0);
  assert(session.lastError() == atem::Error::SendFailed);
  wire.failSend = false;
  hello(session, 22000);
  receive(session, packet(atem::NewSession, 0x53ab, 0, {4, 0, 0, 0, 0, 0, 0, 0}), 22002);
  assert(session.lastError() == atem::Error::PeerRestart);
  session.start(24000, 0x53ab);
  receive(session, packet(atem::NewSession, 0x53ab, 0, {3, 0, 0, 0, 0, 0, 0, 0}), 24001);
  assert(session.lastError() == atem::Error::ConnectionRefused);
}

int main() {
  testHandshakeAndStateGate();
  testCommandWireFormatAndFeedback();
  testLossRetryAndNoStaleReplay();
  testReorderDuplicatesAndGap();
  testSequenceWrapBothDirections();
  testClockWrapAndSessionIsolation();
  testRetransmitRequestsAndQueueBound();
  testMalformedPacketsAndAtomicParsing();
  testTimeoutAndSendFailure();
  puts("ATEM protocol: 9 scenario groups passed (including 32,769 command/ACK cycles).");
}
