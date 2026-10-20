#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

// Controller-side dry-contact/open-collector sensing only. Never drive outputs.
// No dependence on ESP32 libraries or the Linux/ATEM control policy.
const uint8_t pins[] = {4, 5, 6, 7, 8, 9};
bool stable[6], candidate[6];
uint32_t since[6];
char line[32];
uint8_t used = 0;
bool discarding = false;

void setup() {
  for (uint8_t i = 0; i < 6; ++i) {
    pinMode(pins[i], INPUT_PULLUP);
    stable[i] = candidate[i] = digitalRead(pins[i]) == HIGH;
    since[i] = millis();
  }
  Serial.begin(115200); // USB CDC: never wait for a monitor/host to open it
}

void answer() {
  if (strncmp(line, "Q1 ", 3) != 0 || strlen(line + 3) != 8) return;
  for (uint8_t i = 3; i < 11; ++i)
    if (!((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f'))) return;
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 6; ++i) if (stable[i]) mask |= 1 << i;
  // Versioned full snapshot, echoing the host's one-use challenge. Old queued
  // snapshots cannot satisfy a new connection or extend input freshness.
  Serial.print(F("S1 "));
  Serial.print(line + 3);
  Serial.print(' ');
  Serial.println(mask);
}

void loop() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < 6; ++i) {
    const bool raw = digitalRead(pins[i]) == HIGH;
    if (raw != candidate[i]) { candidate[i] = raw; since[i] = now; }
    if (static_cast<uint32_t>(now - since[i]) >= 20) stable[i] = raw;
  }
  // Bounded receive work leaves time for input sampling under malformed traffic.
  for (uint8_t budget = 0; budget < 32 && Serial.available(); ++budget) {
    const char ch = Serial.read();
    if (ch == '\n') {
      if (!discarding) { line[used] = 0; answer(); }
      used = 0; discarding = false;
    } else if (ch != '\r' && !discarding) {
      if (used < static_cast<uint8_t>(sizeof(line) - 1)) line[used++] = ch;
      else { used = 0; discarding = true; }
    }
  }
}
