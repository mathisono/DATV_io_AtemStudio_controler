# Validation scope

This initial version is software-tested and cross-compiled. No physical ESP32,
DVB-S2 demodulator or original ATEM Television Studio was available during
implementation. Treat the first installation as commissioning, not as a previously
proven field deployment.

## Repeatable checks

```sh
python scripts/test_native.py
npm ci --ignore-scripts
npm test
pio run -e esp32-poe-iso -e esp32dev_wifi
```

The native runner builds the real hardware-independent C++ modules with C++17,
`-Wall -Wextra -Werror -pedantic`, then runs their scenario tests. It does not send
network traffic to a real ATEM. There are 26 native scenario groups in total.

| Area | Checked behavior |
| --- | --- |
| Alarm arbitration | Eight scenario groups: acquisition/release filtering, five-minute rotation, all six channels, source loss, priority/mapping, fixed conflict output, manual/reconfigure behavior, electrical inversion/disabled inputs, and wrapping timers |
| ATEM UDP session | Nine scenario groups: initialization gating, actual feedback, command encoding, ACK loss, retransmission, duplicate suppression, receive reordering, session/packet validation, abandoned commands, timeout/reconnect, and sequence/time rollover |
| Program coordination | Nine scenario groups: pending automatic requests versus external takeover, own feedback before ACK, feedback gaps, obsolete target cancellation, confirmation timeout, reconnect retry budget, and wrapping request timers |
| Protocol sequence limit | 32,769 real command/ACK cycles within the native simulator exercise the outgoing 15-bit sequence rollover |
| Web behavior | Actual dashboard assets under jsdom: mapped Take, actual-feedback-only Program tally, CSRF token refresh, safe text rendering, preserved unsaved edits, validation, Wi-Fi credential clearing, stale/disconnected state and password reauthentication |
| Firmware | PlatformIO builds for Olimex `esp32-poe-iso` and generic WROOM `esp32dev_wifi`, including the embedded compressed web assets |

The protocol engine and Program coordinator additionally passed AddressSanitizer and UndefinedBehaviorSanitizer
checks. In the restricted implementation environment, LeakSanitizer was disabled
because it could not inspect `/proc`. Dashboard tests validate DOM interactions,
not layout: browser screenshot verification was unavailable because the Chromium
download endpoints returned invalid archives.

GitHub Actions repeats the native/dashboard tests and both firmware builds. The
workflow neither flashes hardware nor controls an on-air switcher.

## What requires hardware

The following remain to be established on the delivered equipment:

1. Receiver alarm pinout, voltage, source/sink current, and whether the alarm means
   decoded video, transport-stream lock, or only RF lock.
2. Input conditioning, polarity, disconnected-cable state, isolation and noise
   behavior on all six installed channels.
3. The actual board revision, Ethernet PHY initialization, DHCP, interface changes,
   packet loss recovery, and recovery after a power interruption.
4. ATEM handshake/command interoperability with the original switcher's installed
   firmware and actual Program output for each requested source.
5. Correct receiver HDMI/SDI format and the original ATEM's source/connector mapping.
6. Manual takeover under packet loss, including an unacknowledged automatic command,
   concurrent control from Blackmagic Software Control, and reconnection.
7. Display layout and operation in the actual desktop/mobile browsers used by the
   operator, followed by a sustained network/hardware soak test.

Use the ordered acceptance procedure in [deployment.md](deployment.md). Record
board/receiver/ATEM models and firmware, the code commit, network addresses,
timing configuration and observed results before using the controller on air.
