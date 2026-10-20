# Validation scope

This initial version is software-tested and cross-compiled. No physical LattePanda, ESP32,
DVB-S2 demodulator or original ATEM Television Studio was available during
implementation. Treat the first installation as commissioning, not as a previously
proven field deployment.

## Repeatable checks

```sh
python scripts/test_native.py
python3 linux/build.py
python3 -m unittest discover -s test/linux -v
npm ci --ignore-scripts
npm test
pio run -e esp32-poe-iso -e esp32dev_wifi
pio run -d firmware/lattepanda-inputs
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

GitHub Actions repeats the native/dashboard tests, Linux integration tests, both
ESP32 builds and the onboard Arduino compile check. The
workflow neither flashes hardware nor controls an on-air switcher.

## LattePanda / Linux addition

- The shared Linux library builds with C++17 and warnings treated as errors.
- Nine host integration tests pass against the real shared core, pseudo-terminal
  USB serial simulation, loopback UDP and a local HTTP server. They verify
  freshness, bounded serial parsing, stale/duplicate replies, startup/reconnect,
  automatic-command cancellation, actual Program feedback, operator takeover,
  authentication, CSRF, input validation and private atomic settings persistence.
- Dashboard checks cover unknown Arduino inputs, arming restrictions, manual
  control without input telemetry and platform-specific setup fields. Existing
  ESP32 dashboard cases still pass.
- The ATmega32U4 firmware compiles with the pinned Leonardo profile. This is a
  compile check; the vendor board package/bootloader upload and physical internal
  USB link have not been exercised. See [LattePanda deployment](lattepanda.md).
- The systemd unit passes `systemd-analyze verify`; actual service installation,
  serial permissions, Linux Mint hardware support and power recovery need bench
  verification. No receiver firmware/MIB values are fabricated.

## ProView 7100 documentation review

The [receiver compatibility review](receivers/harmonic-proview-7100.md) identifies
dry-contact alarm wiring, shared video modes and documented receiver monitoring
interfaces. This is a review of manufacturer and integration documentation, not
a successful hardware interoperability test. **SNMP/XML polling, trap reception,
live modulation reporting and a network-polled receiver-status freshness gate are not implemented
in the current firmware.** Matching receiver firmware/MIB/API information and
observed responses are required before those features can be implemented and
validated. The existing GPIO `locked` status only reports qualified input state.

## What requires hardware

The following remain to be established on the delivered equipment:

1. ProView GPI pinout/revision, contact ratings, configured relay alarm assignments,
   and whether the chosen condition represents the active decoded service.
2. Input conditioning, polarity, disconnected-cable, receiver boot/power-off and
   decoder-failure states, isolation and noise behavior on all six installed channels.
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
