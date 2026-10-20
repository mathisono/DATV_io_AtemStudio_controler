#pragma once

#include <stddef.h>
#include <stdint.h>

// Feedback, not optimistic copies of commands sent by this controller.
struct AtemState {
  bool programValid = false;
  bool previewValid = false;
  bool fadeToBlackValid = false;
  bool transitionValid = false;
  uint16_t program = 0;
  uint16_t preview = 0;
  bool fadeToBlack = false;
  bool fadeToBlackTransitioning = false;
  bool inTransition = false;
  uint32_t version = 0;
  char product[45] = {};
  uint32_t connectionGeneration = 0;
  uint32_t lastRxMs = 0;
};

struct AtemStats {
  uint32_t rxPackets = 0;
  uint32_t txPackets = 0;
  uint32_t malformedPackets = 0;
  uint32_t foreignPackets = 0;
  uint32_t retransmissions = 0;
  uint32_t connectionAttempts = 0;
  uint32_t disconnects = 0;
  uint32_t rejectedCommands = 0;
  uint32_t acknowledgedCommands = 0;
  uint32_t droppedCommands = 0;
  uint32_t sequenceGaps = 0;
  uint32_t outOfOrderDrops = 0;
  uint32_t sendErrors = 0;
};

namespace atem {

constexpr uint16_t kPort = 9910;
constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxPacketSize = 2047;
constexpr uint16_t kSequenceMask = 0x7fff;
constexpr size_t kMaxPendingCommands = 8;
constexpr size_t kReceiveWindow = 8;

enum PacketFlag : uint8_t {
  AckRequest = 0x01,
  NewSession = 0x02,
  IsRetransmit = 0x04,
  RetransmitRequest = 0x08,
  AckReply = 0x10
};

struct Header {
  uint8_t flags = 0;
  uint16_t length = 0;
  uint16_t session = 0;
  uint16_t ack = 0;
  uint16_t retransmit = 0;
  uint16_t auxiliary = 0;
  uint16_t sequence = 0;
};

uint16_t read16(const uint8_t* data);
void write16(uint8_t* data, uint16_t value);
bool decodeHeader(const uint8_t* data, size_t size, Header& result);
void encodeHeader(uint8_t* data, const Header& header);
uint16_t nextSequence(uint16_t sequence);
// True for an equal or earlier sequence within the half-range, including wrap.
bool coveredByAck(uint16_t ack, uint16_t sequence);
bool validateCommandBlocks(const uint8_t* payload, size_t size);

enum class Phase : uint8_t { Stopped, Hello, Synchronizing, Ready, RetryWait };
enum class Error : uint8_t {
  None,
  HandshakeTimeout,
  ConnectionRefused,
  PeerRestart,
  ReceiveTimeout,
  InitialStateTimeout,
  ReceiveGapTimeout,
  CommandTimeout,
  RetransmitUnavailable,
  SendFailed
};

// A single-threaded, no-allocation session engine. The UDP wrapper checks the
// sender's IP/port before delivering a datagram. The injected clock makes loss,
// reconnection and millis()/sequence wrap behavior testable without a switcher.
class Session {
 public:
  using SendFunction = bool (*)(void*, const uint8_t*, size_t);

  Session(SendFunction send, void* context);
  void start(uint32_t now, uint16_t helloSession);
  void stop();
  void tick(uint32_t now);
  void receive(const uint8_t* data, size_t size, uint32_t now);
  bool readyToRestart(uint32_t now) const;
  bool connected() const { return phase_ == Phase::Ready && !hasGap_; }
  Phase phase() const { return phase_; }
  Error lastError() const { return error_; }
  const char* statusText() const;
  const char* errorText() const;
  const AtemState& state() const { return state_; }
  const AtemStats& stats() const { return stats_; }
  size_t pendingCommands() const { return pendingCount_; }
  uint16_t sessionId() const { return sessionId_; }

  bool setProgram(uint16_t input, uint32_t now);
  bool setPreview(uint16_t input, uint32_t now);
  bool cut(uint32_t now);
  bool autoTransition(uint32_t now);
  bool fadeToBlack(uint32_t now);

  void noteForeignPacket() { ++stats_.foreignPackets; }
  void noteMalformedPacket() { ++stats_.malformedPackets; }

 private:
  struct Pending {
    uint8_t packet[24] = {};
    uint16_t sequence = 0;
    uint32_t createdAt = 0;
    uint32_t sentAt = 0;
    uint8_t retries = 0;
  };
  struct Buffered {
    bool used = false;
    uint16_t sequence = 0;
    uint16_t length = 0;
    uint8_t flags = 0;
    uint8_t payload[kMaxPacketSize - kHeaderSize] = {};
  };

  SendFunction send_;
  void* context_;
  Phase phase_ = Phase::Stopped;
  Error error_ = Error::None;
  AtemState state_;
  AtemStats stats_;
  uint16_t sessionId_ = 0;
  uint16_t handshakeSessionId_ = 0;
  uint16_t nextTxSequence_ = 1;
  uint16_t lastRxSequence_ = 0;
  uint16_t highestRxSequence_ = 0;
  bool awaitAssignedSession_ = false;
  bool initComplete_ = false;
  bool havePayload_ = false;
  bool hasGap_ = false;
  uint32_t startedAt_ = 0;
  uint32_t helloSentAt_ = 0;
  uint32_t progressAt_ = 0;
  uint32_t gapRequestAt_ = 0;
  uint32_t failedAt_ = 0;
  Pending pending_[kMaxPendingCommands];
  size_t pendingCount_ = 0;
  Buffered buffered_[kReceiveWindow];

  bool transmit(const uint8_t* packet, size_t size);
  void clearSessionState();
  void fail(Error error, uint32_t now);
  void sendHello(uint32_t now);
  void acknowledge(uint16_t sequence, uint16_t auxiliary = 0);
  void requestMissing(uint32_t now);
  void handleAcknowledgement(uint16_t sequence);
  void retransmitFrom(uint16_t sequence, uint32_t now);
  void retransmit(size_t index, uint32_t now);
  bool command(const char name[4], uint16_t input, uint32_t now);
  void deliver(const uint8_t* payload, size_t size, uint8_t flags, uint32_t now);
  void parseCommands(const uint8_t* payload, size_t size);
  void drainBuffered(uint32_t now);
  void updateReady();
};

}  // namespace atem
