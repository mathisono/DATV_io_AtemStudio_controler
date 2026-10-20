#include <AtemProtocol.h>

#include <string.h>

namespace atem {
namespace {
constexpr uint32_t kHelloRetryMs = 1000;
constexpr uint32_t kHelloTimeoutMs = 5000;
constexpr uint32_t kReceiveTimeoutMs = 5000;
constexpr uint32_t kInitialStateTimeoutMs = 10000;
constexpr uint32_t kGapTimeoutMs = 3000;
constexpr uint32_t kReconnectDelayMs = 1500;
constexpr uint32_t kRetransmitMs = 80;
constexpr uint32_t kCommandMaxAgeMs = 900;
constexpr uint8_t kMaxRetries = 8;
constexpr uint32_t kGapRequestMs = 150;
constexpr uint16_t kHalfRange = 0x4000;

uint16_t distanceFrom(uint16_t previous, uint16_t next) {
  return static_cast<uint16_t>((next - previous) & kSequenceMask);
}

bool commandIs(const uint8_t* name, const char expected[4]) {
  return memcmp(name, expected, 4) == 0;
}

bool validKnownLength(const uint8_t* name, size_t bodySize) {
  if (commandIs(name, "PrgI") || commandIs(name, "PrvI") ||
      commandIs(name, "FtbS") || commandIs(name, "_ver")) {
    return bodySize >= 4;
  }
  if (commandIs(name, "TrPs")) return bodySize >= 6;
  if (commandIs(name, "_pin")) return bodySize >= 1;
  return true;  // Unknown commands are safely skipped by their declared length.
}
}  // namespace

uint16_t read16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

void write16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value);
}

bool decodeHeader(const uint8_t* data, size_t size, Header& result) {
  if (data == nullptr || size < kHeaderSize || size > kMaxPacketSize) return false;
  const uint16_t flagsAndLength = read16(data);
  if ((flagsAndLength & 0x07ff) != size) return false;
  result.flags = static_cast<uint8_t>(flagsAndLength >> 11);
  result.length = static_cast<uint16_t>(size);
  result.session = read16(data + 2);
  result.ack = read16(data + 4);
  result.retransmit = read16(data + 6);
  result.auxiliary = read16(data + 8);
  result.sequence = read16(data + 10);
  if ((result.flags & AckRequest) && result.sequence > kSequenceMask) return false;
  if ((result.flags & AckReply) && result.ack > kSequenceMask) return false;
  // ATEM occasionally asks for 32768 to represent sequence 0 on wrap.
  if ((result.flags & RetransmitRequest) && result.retransmit > 0x8000) return false;
  return true;
}

void encodeHeader(uint8_t* data, const Header& header) {
  write16(data, static_cast<uint16_t>((header.flags << 11) | header.length));
  write16(data + 2, header.session);
  write16(data + 4, header.ack);
  write16(data + 6, header.retransmit);
  write16(data + 8, header.auxiliary);
  write16(data + 10, header.sequence);
}

uint16_t nextSequence(uint16_t sequence) {
  return static_cast<uint16_t>((sequence + 1) & kSequenceMask);
}

bool coveredByAck(uint16_t ack, uint16_t sequence) {
  if (ack > kSequenceMask || sequence > kSequenceMask) return false;
  return distanceFrom(sequence, ack) < kHalfRange;
}

bool validateCommandBlocks(const uint8_t* payload, size_t size) {
  if (size > 0 && payload == nullptr) return false;
  size_t offset = 0;
  while (offset < size) {
    if (size - offset < 8) return false;
    const size_t length = read16(payload + offset);
    if (length < 8 || length > size - offset) return false;
    if (!validKnownLength(payload + offset + 4, length - 8)) return false;
    offset += length;
  }
  return true;
}

Session::Session(SendFunction send, void* context) : send_(send), context_(context) {}

void Session::clearSessionState() {
  stats_.droppedCommands += static_cast<uint32_t>(pendingCount_);
  pendingCount_ = 0;
  for (auto& item : buffered_) item.used = false;
  const uint32_t generation = state_.connectionGeneration;
  state_ = AtemState{};
  state_.connectionGeneration = generation;
  nextTxSequence_ = 1;
  lastRxSequence_ = 0;
  highestRxSequence_ = 0;
  awaitAssignedSession_ = false;
  initComplete_ = false;
  havePayload_ = false;
  hasGap_ = false;
}

void Session::start(uint32_t now, uint16_t helloSession) {
  clearSessionState();
  sessionId_ = helloSession;
  handshakeSessionId_ = helloSession;
  startedAt_ = progressAt_ = now;
  state_.lastRxMs = now;
  phase_ = Phase::Hello;
  error_ = Error::None;
  ++stats_.connectionAttempts;
  sendHello(now);
}

void Session::stop() {
  clearSessionState();
  phase_ = Phase::Stopped;
}

void Session::fail(Error error, uint32_t now) {
  if (phase_ == Phase::RetryWait || phase_ == Phase::Stopped) return;
  ++stats_.disconnects;
  clearSessionState();
  phase_ = Phase::RetryWait;
  error_ = error;
  failedAt_ = now;
}

bool Session::transmit(const uint8_t* packet, size_t size) {
  if (send_ != nullptr && send_(context_, packet, size)) {
    ++stats_.txPackets;
    return true;
  }
  ++stats_.sendErrors;
  return false;
}

void Session::sendHello(uint32_t now) {
  uint8_t packet[20] = {};
  Header header;
  header.flags = NewSession;
  header.length = sizeof(packet);
  header.session = sessionId_;
  header.auxiliary = 0x003a;
  encodeHeader(packet, header);
  packet[12] = 0x01;
  helloSentAt_ = now;
  transmit(packet, sizeof(packet));
}

void Session::acknowledge(uint16_t sequence, uint16_t auxiliary) {
  uint8_t packet[kHeaderSize] = {};
  Header header;
  header.flags = AckReply;
  header.length = sizeof(packet);
  header.session = sessionId_;
  header.ack = sequence;
  header.auxiliary = auxiliary;
  encodeHeader(packet, header);
  transmit(packet, sizeof(packet));
}

void Session::requestMissing(uint32_t now) {
  // Cumulative ACK leaves everything after lastRxSequence_ unacknowledged;
  // the switcher retransmits it. This is also how atem-connection handles loss.
  acknowledge(lastRxSequence_);
  gapRequestAt_ = now;
}

bool Session::readyToRestart(uint32_t now) const {
  return phase_ == Phase::RetryWait &&
         static_cast<uint32_t>(now - failedAt_) >= kReconnectDelayMs;
}

void Session::tick(uint32_t now) {
  if (phase_ == Phase::Stopped || phase_ == Phase::RetryWait) return;
  if (phase_ == Phase::Hello) {
    if (static_cast<uint32_t>(now - startedAt_) >= kHelloTimeoutMs) {
      fail(Error::HandshakeTimeout, now);
    } else if (static_cast<uint32_t>(now - helloSentAt_) >= kHelloRetryMs) {
      sendHello(now);
    }
    return;
  }
  if (static_cast<uint32_t>(now - state_.lastRxMs) >= kReceiveTimeoutMs) {
    fail(Error::ReceiveTimeout, now);
    return;
  }
  if (phase_ == Phase::Synchronizing &&
      static_cast<uint32_t>(now - startedAt_) >= kInitialStateTimeoutMs) {
    fail(Error::InitialStateTimeout, now);
    return;
  }
  if (hasGap_) {
    if (static_cast<uint32_t>(now - progressAt_) >= kGapTimeoutMs) {
      fail(Error::ReceiveGapTimeout, now);
      return;
    }
    if (static_cast<uint32_t>(now - gapRequestAt_) >= kGapRequestMs) requestMissing(now);
  }
  if (pendingCount_ > 0) {
    if (static_cast<uint32_t>(now - pending_[0].createdAt) >= kCommandMaxAgeMs ||
        pending_[0].retries >= kMaxRetries) {
      fail(Error::CommandTimeout, now);
      return;
    }
    if (static_cast<uint32_t>(now - pending_[0].sentAt) >= kRetransmitMs) {
      for (size_t i = 0; i < pendingCount_; ++i) retransmit(i, now);
    }
  }
}

void Session::handleAcknowledgement(uint16_t sequence) {
  if (pendingCount_ == 0) return;
  // Never acknowledge a command that has not actually been transmitted.
  const uint16_t latest = static_cast<uint16_t>((nextTxSequence_ - 1) & kSequenceMask);
  if (!coveredByAck(latest, sequence)) return;
  size_t count = 0;
  while (count < pendingCount_ && coveredByAck(sequence, pending_[count].sequence)) ++count;
  stats_.acknowledgedCommands += static_cast<uint32_t>(count);
  for (size_t i = count; i < pendingCount_; ++i) pending_[i - count] = pending_[i];
  pendingCount_ -= count;
}

void Session::retransmit(size_t index, uint32_t now) {
  Pending& item = pending_[index];
  item.packet[0] |= static_cast<uint8_t>(IsRetransmit << 3);
  item.sentAt = now;
  ++item.retries;
  ++stats_.retransmissions;
  // The sequence stays unchanged, so an accepted non-idempotent command (CUT,
  // AUTO, FTB) is not intentionally executed twice if its ACK was lost.
  transmit(item.packet, sizeof(item.packet));
}

void Session::retransmitFrom(uint16_t sequence, uint32_t now) {
  sequence &= kSequenceMask;
  size_t index = 0;
  while (index < pendingCount_ && pending_[index].sequence != sequence) ++index;
  if (index == pendingCount_) {
    // There is no safe way to invent an old command. Reconnect, dropping it.
    fail(Error::RetransmitUnavailable, now);
    return;
  }
  for (size_t i = index; i < pendingCount_; ++i) {
    if (pending_[i].retries >= kMaxRetries ||
        static_cast<uint32_t>(now - pending_[i].createdAt) >= kCommandMaxAgeMs) {
      fail(Error::CommandTimeout, now);
      return;
    }
    retransmit(i, now);
  }
}

bool Session::command(const char name[4], uint16_t input, uint32_t now) {
  // An HTTP handler may run before the next regular tick. Do not accept work
  // on a connection whose last feedback/previous command has already expired.
  if (phase_ == Phase::Ready &&
      static_cast<uint32_t>(now - state_.lastRxMs) >= kReceiveTimeoutMs) {
    fail(Error::ReceiveTimeout, now);
  } else if (pendingCount_ > 0 &&
             static_cast<uint32_t>(now - pending_[0].createdAt) >= kCommandMaxAgeMs) {
    fail(Error::CommandTimeout, now);
  }
  if (!connected() || pendingCount_ >= kMaxPendingCommands) {
    ++stats_.rejectedCommands;
    return false;
  }
  Pending& item = pending_[pendingCount_];
  item = Pending{};
  item.sequence = nextTxSequence_;
  item.createdAt = item.sentAt = now;
  Header header;
  header.flags = AckRequest;
  header.length = sizeof(item.packet);
  header.session = sessionId_;
  header.sequence = item.sequence;
  encodeHeader(item.packet, header);
  write16(item.packet + 12, 12);  // 8-byte command header + 4-byte payload.
  memcpy(item.packet + 16, name, 4);
  // Byte 20 selects M/E 0. The source ID at 22 is zero for CUT/AUTO/FTB.
  write16(item.packet + 22, input);
  if (!transmit(item.packet, sizeof(item.packet))) {
    ++stats_.rejectedCommands;
    fail(Error::SendFailed, now);
    return false;
  }
  ++pendingCount_;
  nextTxSequence_ = nextSequence(nextTxSequence_);
  return true;
}

bool Session::setProgram(uint16_t input, uint32_t now) { return command("CPgI", input, now); }
bool Session::setPreview(uint16_t input, uint32_t now) { return command("CPvI", input, now); }
bool Session::cut(uint32_t now) { return command("DCut", 0, now); }
bool Session::autoTransition(uint32_t now) { return command("DAut", 0, now); }
bool Session::fadeToBlack(uint32_t now) { return command("FtbA", 0, now); }

void Session::parseCommands(const uint8_t* payload, size_t size) {
  // receive() already validated ALL blocks, so a malformed later block cannot
  // leave an earlier Program/Preview update partially applied.
  for (size_t offset = 0; offset < size;) {
    const size_t length = read16(payload + offset);
    const uint8_t* name = payload + offset + 4;
    const uint8_t* body = payload + offset + 8;
    const size_t bodySize = length - 8;
    if (commandIs(name, "PrgI") && body[0] == 0) {
      state_.program = read16(body + 2);
      state_.programValid = true;
    } else if (commandIs(name, "PrvI") && body[0] == 0) {
      state_.preview = read16(body + 2);
      state_.previewValid = true;
    } else if (commandIs(name, "FtbS") && body[0] == 0) {
      state_.fadeToBlack = body[1] != 0;
      state_.fadeToBlackTransitioning = body[2] != 0;
      state_.fadeToBlackValid = true;
    } else if (commandIs(name, "TrPs") && body[0] == 0) {
      state_.inTransition = body[1] != 0;
      state_.transitionValid = true;
    } else if (commandIs(name, "_ver")) {
      state_.version = (static_cast<uint32_t>(read16(body)) << 16) | read16(body + 2);
    } else if (commandIs(name, "_pin")) {
      // Product name is a 40-byte string; bytes after that are other fields.
      const size_t count = bodySize < 40 ? bodySize : 40;
      memcpy(state_.product, body, count);
      state_.product[count] = '\0';
    } else if (commandIs(name, "InCm")) {
      initComplete_ = true;
    }
    offset += length;
  }
}

void Session::updateReady() {
  if (phase_ == Phase::Synchronizing && initComplete_ &&
      state_.programValid && state_.previewValid) {
    phase_ = Phase::Ready;
    error_ = Error::None;
    ++state_.connectionGeneration;
  }
}

void Session::deliver(const uint8_t* payload, size_t size, uint8_t flags, uint32_t now) {
  if (size > 0) {
    havePayload_ = true;
    parseCommands(payload, size);
  } else if (phase_ == Phase::Synchronizing && havePayload_ && (flags & AckRequest)) {
    // Legacy firmware also marks the end of its state dump with an empty
    // reliable packet. Strict sequence delivery means no earlier packet is lost.
    initComplete_ = true;
  }
  progressAt_ = now;
  updateReady();
}

void Session::drainBuffered(uint32_t now) {
  bool found;
  do {
    found = false;
    const uint16_t wanted = nextSequence(lastRxSequence_);
    for (auto& item : buffered_) {
      if (item.used && item.sequence == wanted) {
        lastRxSequence_ = wanted;
        deliver(item.payload, item.length, item.flags, now);
        item.used = false;
        found = true;
        break;
      }
    }
  } while (found);
  const uint16_t distance = distanceFrom(lastRxSequence_, highestRxSequence_);
  hasGap_ = distance > 0 && distance < kHalfRange;
}

void Session::receive(const uint8_t* data, size_t size, uint32_t now) {
  if (phase_ == Phase::Stopped || phase_ == Phase::RetryWait) return;
  Header header;
  if (!decodeHeader(data, size, header)) {
    ++stats_.malformedPackets;
    return;
  }

  if (header.flags & NewSession) {
    if (size != 20 || header.sequence > kSequenceMask) {
      ++stats_.malformedPackets;
      return;
    }
    const uint8_t status = data[12];
    if (phase_ == Phase::Hello) {
      if (status != 0x02) {
        fail(status == 0x03 ? Error::ConnectionRefused : Error::PeerRestart, now);
        return;
      }
      sessionId_ = handshakeSessionId_ = header.session;
      lastRxSequence_ = highestRxSequence_ = header.sequence;
      awaitAssignedSession_ = true;
      phase_ = Phase::Synchronizing;
      state_.lastRxMs = progressAt_ = now;
      ++stats_.rxPackets;
      // The auxiliary 3 is accepted by older Television Studio firmware.
      acknowledge(header.sequence, 0x0003);
      return;
    }
    // A duplicate hello while the initial ACK is being retransmitted must not
    // erase state already received. A real peer restart requires a new socket.
    if (status == 0x02 && phase_ == Phase::Synchronizing &&
        header.session == handshakeSessionId_) {
      ++stats_.rxPackets;
      state_.lastRxMs = now;
      const uint16_t activeSession = sessionId_;
      sessionId_ = handshakeSessionId_;
      acknowledge(header.sequence, 0x0003);
      sessionId_ = activeSession;
    } else {
      fail(Error::PeerRestart, now);
    }
    return;
  }

  if (phase_ == Phase::Hello) return;
  const bool sequenced = (header.flags & AckRequest) ||
                         (phase_ == Phase::Synchronizing && size > kHeaderSize);
  if (sequenced && header.sequence > kSequenceMask) {
    ++stats_.malformedPackets;
    return;
  }
  if (size > kHeaderSize && !validateCommandBlocks(data + kHeaderSize, size - kHeaderSize)) {
    ++stats_.malformedPackets;
    return;
  }
  if (awaitAssignedSession_ && sequenced) {
    // Original ATEM handshake responses can contain the temporary client ID;
    // the actual session ID appears on the first state packet. Pin it once.
    sessionId_ = header.session;
    awaitAssignedSession_ = false;
  } else if (header.session != sessionId_) {
    ++stats_.foreignPackets;
    return;
  }
  ++stats_.rxPackets;
  state_.lastRxMs = now;

  if (header.flags & AckReply) handleAcknowledgement(header.ack);
  if (header.flags & RetransmitRequest) {
    retransmitFrom(header.retransmit, now);
    if (phase_ == Phase::RetryWait) return;
  }
  if (!sequenced) return;

  const uint16_t distance = distanceFrom(lastRxSequence_, header.sequence);
  if (distance == 1) {
    lastRxSequence_ = header.sequence;
    if (distanceFrom(highestRxSequence_, header.sequence) < kHalfRange) {
      highestRxSequence_ = header.sequence;
    }
    const bool firstEmpty = phase_ == Phase::Synchronizing && size == kHeaderSize && havePayload_;
    deliver(data + kHeaderSize, size - kHeaderSize, header.flags, now);
    drainBuffered(now);
    if (header.flags & AckRequest) {
      acknowledge(lastRxSequence_, firstEmpty ? 0x0061 : 0);
    }
  } else if (distance == 0 || distance >= kHalfRange) {
    // Duplicate/old packet: ACK the contiguous high-water mark; never roll
    // Program or Preview state backwards to stale retransmitted feedback.
    if (header.flags & AckRequest) acknowledge(lastRxSequence_);
  } else {
    if (!hasGap_) {
      ++stats_.sequenceGaps;
      progressAt_ = now;
      hasGap_ = true;
    }
    if (distanceFrom(highestRxSequence_, header.sequence) < kHalfRange) {
      highestRxSequence_ = header.sequence;
    }
    bool duplicate = false;
    Buffered* freeSlot = nullptr;
    for (auto& item : buffered_) {
      if (item.used && item.sequence == header.sequence) duplicate = true;
      if (!item.used && freeSlot == nullptr) freeSlot = &item;
    }
    if (!duplicate && freeSlot != nullptr && distance <= 64) {
      freeSlot->sequence = header.sequence;
      freeSlot->length = static_cast<uint16_t>(size - kHeaderSize);
      freeSlot->flags = header.flags;
      memcpy(freeSlot->payload, data + kHeaderSize, freeSlot->length);
      freeSlot->used = true;
    } else if (!duplicate) {
      ++stats_.outOfOrderDrops;
    }
    requestMissing(now);
  }
}

const char* Session::statusText() const {
  switch (phase_) {
    case Phase::Stopped: return "Stopped";
    case Phase::Hello: return "Connecting";
    case Phase::Synchronizing: return "Receiving switcher state";
    case Phase::Ready: return hasGap_ ? "Waiting for missing feedback" : "Connected";
    case Phase::RetryWait: return "Waiting to reconnect";
  }
  return "Unknown";
}

const char* Session::errorText() const {
  switch (error_) {
    case Error::None: return "";
    case Error::HandshakeTimeout: return "No ATEM handshake response";
    case Error::ConnectionRefused: return "ATEM client connections are full";
    case Error::PeerRestart: return "ATEM session restarted";
    case Error::ReceiveTimeout: return "ATEM receive timeout";
    case Error::InitialStateTimeout: return "ATEM initial state was incomplete";
    case Error::ReceiveGapTimeout: return "ATEM feedback packet gap did not recover";
    case Error::CommandTimeout: return "ATEM command was not acknowledged; session discarded";
    case Error::RetransmitUnavailable: return "ATEM requested an unavailable command; session discarded";
    case Error::SendFailed: return "UDP send failed";
  }
  return "Unknown";
}

}  // namespace atem
