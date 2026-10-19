#pragma once

#include <AtemProtocol.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFiUdp.h>

// Use only from the Arduino loop / synchronous HTTP handlers, not an ISR or
// another FreeRTOS task. WiFiUDP also uses the ESP32 native Ethernet interface.
class AtemClient {
 public:
  AtemClient();
  // Opens the socket and starts a nonblocking handshake. This return value is
  // socket setup success; connected() becomes true after initial feedback.
  bool begin(IPAddress remote, uint16_t localPort = 0);
  void tick();
  void disconnect();
  bool connected() const { return session_.connected(); }
  const AtemState& state() const { return session_.state(); }
  const AtemStats& stats() const { return session_.stats(); }
  const char* statusText() const { return session_.statusText(); }
  const char* errorText() const { return session_.errorText(); }
  size_t pendingCommands() const { return session_.pendingCommands(); }

  // true = sent and tracked in this session; it does not mean confirmed on air.
  // Calls while disconnected or when the bounded transmit window is full fail.
  bool setProgram(uint16_t input) { return session_.setProgram(input, millis()); }
  bool setPreview(uint16_t input) { return session_.setPreview(input, millis()); }
  bool cut() { return session_.cut(millis()); }
  bool autoTransition() { return session_.autoTransition(millis()); }
  bool fadeToBlack() { return session_.fadeToBlack(millis()); }

 private:
  WiFiUDP udp_;
  IPAddress remote_;
  uint16_t requestedLocalPort_ = 0;
  bool enabled_ = false;
  bool socketOpen_ = false;
  uint32_t bindAttemptAt_ = 0;
  uint8_t receiveBuffer_[atem::kMaxPacketSize];
  atem::Session session_;

  bool openSession(uint32_t now);
  static bool sendPacket(void* context, const uint8_t* data, size_t size);
};
#endif
