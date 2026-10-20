# ATEM transport

This library implements only the M/E 0 controls used by this project. It has no
runtime dependency on an external ATEM library. `AtemProtocol.h/.cpp` is ordinary
C++11; `AtemClient.h/.cpp` adds the ESP32 Arduino `WiFiUDP` socket wrapper, which
can use Wi-Fi or the ESP32's native Ethernet stack.

## Scope and compatibility

The target is the original six-input ATEM Television Studio. The command wire
formats are `CPgI` (Program), `CPvI` (Preview), `DCut`, `DAut`, and `FtbA` (fade to
black toggle), with M/E index 0. Feedback includes `PrgI`, `PrvI`, `FtbS`, `TrPs`,
`_ver`, and `_pin`. Other valid command blocks are skipped.

This is a community-protocol implementation, not the Blackmagic SDK. Automated
tests verify synthetic packet/loss behavior. **An ESP32 and a real original ATEM
have not been available for physical interoperability testing.** Commission the
firmware against the particular switcher firmware before enabling automation.
ATEM 7.5.2 is a useful legacy baseline to record during commissioning; no switcher
firmware update is performed by this library.

## Delivery behavior

- UDP destination port 9910; the sender address and source port are checked.
- Nonblocking handshake and initial-state gating. Program and Preview must be
  received before `connected()` becomes true, together with the `InCm` marker
  or the older end-of-dump empty reliable packet.
- Client-temporary and assigned switcher session IDs are handled separately.
  The assigned session is pinned on the first state packet.
- 15-bit sequence numbers wrap at 32768. Reliable packets use cumulative ACKs.
  Duplicates are re-ACKed and do not roll state backwards.
- Up to eight out-of-order feedback packets are buffered. Further packets are
  dropped without advancing the ACK, allowing switcher retransmission. A gap
  that does not progress for three seconds causes reconnection. Controls are
  rejected while a feedback gap exists.
- Up to eight commands can be in flight. Unacknowledged commands are resent
  after 80 ms with their **same sequence numbers**, so a lost ACK does not
  intentionally cause a second CUT/AUTO/FTB operation. A command is abandoned
  by resetting the session after eight retries or 900 ms.
- An incoming retransmit request is fulfilled from the in-flight window. If
  the requested command is no longer available, the session resets instead of
  inventing or replaying an unrelated command.
- Commands fail immediately while disconnected; no manual-command queue is
  carried over a reconnect. The application must decide whether current alarm
  state calls for a fresh command after reconnection.
- Program/Preview/FTB in `state()` are actual received feedback. A successful
  setter return means a command was sent and tracked, not that a picture has
  appeared on Program. Transport ACKs are not video-lock confirmation.
- No packet processing performs heap allocation inside the session engine.
  Datagrams and command lengths are bounded before parsing. The session engine
  uses about 17 KB for its fixed receive and transmit windows.

Call `tick()` frequently from the same task as setters and HTTP handlers. Do not
invoke it or setters from a GPIO ISR. Stalling the loop can prevent ACK delivery.
The wrapper rotates the local UDP port on reconnect unless a fixed port was
explicitly provided.

## Native tests

From the repository root:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -Ilib/AtemClient/include lib/AtemClient/src/AtemProtocol.cpp \
  lib/AtemClient/tests/session_test.cpp -o /tmp/datv-atem-protocol-test
/tmp/datv-atem-protocol-test
```

The tests cover handshake gating, state feedback, exact command encodings,
retransmission with unchanged IDs, command abandonment, reconnect without stale
commands, receive reordering, duplicate suppression, a full outgoing sequence
rollover, incoming rollover, `millis()` rollover, wrong-session packets, bounded
queues, malformed packets, atomic parsing, timeouts, and local send failure.

## Protocol references and licensing

Protocol facts and command formats were checked against:

- [Sofie ATEM Connection transport](https://github.com/Sofie-Automation/sofie-atem-connection/blob/main/src/lib/atemSocketChild.ts)
  (inspected blob `874f4479142a69cb3fdc7508ff71c2e581296497`).
- [Sofie command definitions](https://github.com/Sofie-Automation/sofie-atem-connection/tree/main/src/commands/MixEffects).
- [ATEM protocol encoding](https://atem.julusian.dev/protocol).
- [OpenSwitcher transport documentation](https://docs.openswitcher.org/udptransport.html).

The implementation is written for this project. Sofie is MIT-licensed and its
notice is retained in `THIRD_PARTY_NOTICES.md` for the transport reference. The
GPL SKAARHOJ Arduino library is not copied, vendored, or linked by this project.

ATEM, Blackmagic Design and other product names belong to their respective owners.
