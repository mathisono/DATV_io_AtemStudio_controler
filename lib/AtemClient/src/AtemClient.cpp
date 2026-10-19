#include <AtemClient.h>

#ifdef ARDUINO
#include <esp_system.h>

AtemClient::AtemClient() : session_(&AtemClient::sendPacket, this) {}

bool AtemClient::begin(IPAddress remote, uint16_t localPort) {
  disconnect();
  remote_ = remote;
  requestedLocalPort_ = localPort;
  enabled_ = static_cast<uint32_t>(remote) != 0;
  return enabled_ && openSession(millis());
}

bool AtemClient::openSession(uint32_t now) {
  udp_.stop();
  socketOpen_ = false;
  bindAttemptAt_ = now;
  const uint16_t port = requestedLocalPort_ != 0
                            ? requestedLocalPort_
                            : static_cast<uint16_t>(49152u + (esp_random() % 16384u));
  if (udp_.begin(port) == 0) return false;
  socketOpen_ = true;
  uint16_t nonce = static_cast<uint16_t>(esp_random() & 0x7fff);
  if (nonce == 0) nonce = 1;
  session_.start(now, nonce);
  return true;
}

void AtemClient::disconnect() {
  enabled_ = false;
  socketOpen_ = false;
  udp_.stop();
  session_.stop();
}

bool AtemClient::sendPacket(void* context, const uint8_t* data, size_t size) {
  auto* self = static_cast<AtemClient*>(context);
  if (!self->socketOpen_ || self->udp_.beginPacket(self->remote_, atem::kPort) == 0) return false;
  const size_t written = self->udp_.write(data, size);
  const bool sent = self->udp_.endPacket() == 1;
  return written == size && sent;
}

void AtemClient::tick() {
  if (!enabled_) return;
  const uint32_t now = millis();
  if (!socketOpen_) {
    if (static_cast<uint32_t>(now - bindAttemptAt_) >= 1500) openSession(now);
    return;
  }
  if (session_.readyToRestart(now)) {
    // A fresh local port and session ensure late packets cannot replay commands
    // or restore feedback from the old connection. Commands are never queued
    // across this boundary; the automation layer must evaluate current alarms.
    openSession(now);
    return;
  }
  // A state dump cannot monopolize the main loop or starve alarm debouncing.
  for (uint8_t budget = 0; budget < 16; ++budget) {
    const int packetSize = udp_.parsePacket();
    if (packetSize <= 0) break;
    if (udp_.remoteIP() != remote_ || udp_.remotePort() != atem::kPort) {
      session_.noteForeignPacket();
      udp_.flush();
      continue;
    }
    if (static_cast<size_t>(packetSize) > sizeof(receiveBuffer_) || packetSize < 12) {
      session_.noteMalformedPacket();
      udp_.flush();
      continue;
    }
    const int read = udp_.read(receiveBuffer_, static_cast<size_t>(packetSize));
    if (read != packetSize) {
      session_.noteMalformedPacket();
      udp_.flush();
      continue;
    }
    session_.receive(receiveBuffer_, static_cast<size_t>(read), now);
  }
  session_.tick(now);
}
#endif
