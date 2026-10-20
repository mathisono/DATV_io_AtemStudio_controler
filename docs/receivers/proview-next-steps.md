# ProView 7100 integration: next steps

This is the ordered implementation and commissioning checklist for the six-input
ESP32/ATEM controller. Receiver monitoring is a proposed extension: current
firmware qualifies GPIO inputs and controls the ATEM; it does not read ProView
SNMP/XML status or receive traps. The [compatibility review](harmonic-proview-7100.md)
contains the documented GPI pinout, video/RF limits and monitoring evidence.

Start with one representative receiver, then record differences across all units.
The first development milestone is accurate receiver information on the dashboard
while the existing GPIO selection behavior remains in use.

## Ordered work

| Step | Work | Completion evidence |
| --- | --- | --- |
| 1 | Identify installed firmware, cards and source paths | Receiver inventory and screenshots |
| 2 | Obtain matching GPI/MIB/API documentation | Versioned references and a verified object/field map |
| 3 | Prove the relay interface | Measured healthy/fault/power/cable truth table |
| 4 | Prove network status on one receiver | Correlated GUI, polling and trap captures |
| 5 | Add receiver configuration and status model | Settings migration and explicit unknown/fresh/stale states |
| 6 | Implement monitoring and dashboard diagnostics | Correct displayed status without changing switching authority |
| 7 | Add optional network-qualified selection | Verified eligibility feeding existing arbitration |
| 8 | Validate and commission all six feeds | Recorded software and physical acceptance results |

## 1. Collect firmware and hardware information

- [ ] Read the complete displayed firmware string from each distinct configuration.
- [ ] Record RF/front-end and decoder card types/part numbers and installed licenses.
- [ ] Photograph the rear panel and GPI connector label.
- [ ] Record management address and the active RF input, logical transport stream,
  selected service, decoder output, ATEM input and actual video standard.
- [ ] Capture existing GPI alarm assignments and SNMP listener settings.

The following paths are documented in the 4.0.1 and 4.1 user guides. On a different
release, capture the equivalent screens and retain its own manual.

| Information | Documented path |
| --- | --- |
| Firmware and unit identity in browser | Platform → HW Inventory → Platform Properties |
| Running firmware on front panel | Root → Unit → Firmware → Running Firmware |
| RF hardware on front panel | Root → Unit → HW Inventory → Front End Card |
| Decoder option on front panel | Root → Unit → HW Inventory → Bottom Option Card |
| Licenses in browser | Administration → Licensing → FW Licenses |
| Current relay assignments | Administration → GPI |
| Trap destinations | Administration → SNMP |

References: [4.0.1, pp. 54–56 and 111–115][guide401]; [4.1, pp. 54–56 and
112–116][guide41]. The manuals document a firmware-version field, not a separate
build-number field; preserve the full string actually displayed. Inventory does
not require a firmware upgrade.

Use this inventory, with supporting screenshots kept under the same receiver ID:

| ID | Firmware string | RF/decoder cards and licenses | Active RF / TS / service / decoder | ATEM input and output standard |
| --- | --- | --- | --- | --- |
| RX1 | Pending | Pending | Pending | Pending |
| RX2 | Pending | Pending | Pending | Pending |
| RX3 | Pending | Pending | Pending | Pending |
| RX4 | Pending | Pending | Pending | Pending |
| RX5 | Pending | Pending | Pending | Pending |
| RX6 | Pending | Pending | Pending | Pending |

Before later configuration changes, retain a working configuration backup.
Platform → Presets → Backup Device Configuration to a File produces
`RunningPreset.xml` and also stores that preset on the receiver. This is a backup
operation, not a purely read-only screen; DMS-controlled units restrict it.
Record network and license information separately. [4.1, pp. 117–118][guide41]

## 2. Obtain the matching documentation

- [ ] Match the user guide and electrical/GPI specification to the actual hardware.
- [ ] Obtain the Harmonic MIB package and SNMP agent documentation for the installed
  firmware, including supported version, polling access, alarms and notifications.
- [ ] Obtain XML/HTTP API documentation if the needed status is not available via
  documented SNMP objects on that firmware.
- [ ] Establish contact ratings and minimum contact/wetting current before choosing
  the final input circuit.

For every field used in firmware, record its object/endpoint, data type, valid
values, units/scale, RF/service/decoder index, unknown/unlocked behavior and source
document version. Required fields are reception/demodulation health and the mapped
service/decoder health. Actual modulation, FEC, symbol rate and quality measurements
are additional fields only where verified.

SNMP alarm polling is documented by an existing [ProView 7000/7100 integration][icontrol].
Detailed telemetry remains firmware/interface dependent; [Skyline's family
integration][skyline] is supporting evidence, not a replacement API contract.

Keep read communities/passwords, license keys and raw configuration exports out
of the public repository. Use sanitized captures for committed test fixtures.

## 3. Verify the GPI contacts and input circuit

- [ ] Check the actual HD15 GPI connector against the
  [documented pinout and revision caveat](harmonic-proview-7100.md#documented-connector).
- [ ] Choose one relay per feed and record the selected reception/decoder alarms.
- [ ] With the controller disconnected, establish the contact states using a
  suitable contact/continuity test; verify NC availability on the installed unit.
- [ ] Select a protected/filtered contact-sensing interface using its own supply
  and a 3.3 V-compatible controller output.
- [ ] Measure GPIO state and configure polarity only after confirming which
  condition represents usable video.

Repeat this table for each different receiver/interface configuration. Perform
intentional reception and power interruptions on a bench or test output.

| Condition | COM–NO | COM–NC, if fitted | GPIO level | Desired eligibility |
| --- | --- | --- | --- | --- |
| Selected video decodes normally | Measure | Measure | Measure | Qualified after acquire delay |
| RF reception lost | Measure | Measure | Measure | Unqualified after release policy |
| RF locked, selected video fails | Measure | Measure | Measure | Unqualified |
| Receiver boots | Measure | Measure | Measure | Unqualified |
| Receiver has no power | Measure | Measure | Measure | Unqualified |
| Alarm cable disconnected | N/A | N/A | Measure | Unqualified |

The contacts provide no sensing voltage. A closed NC contact is not proof of a
powered, healthy receiver. If healthy and unhealthy conditions are electrically
indistinguishable, add another verified condition before automatic commissioning.

## 4. Establish network status with one receiver

- [ ] Verify read-only access with the supported SNMP version and configured read
  credentials. Trap-community settings alone do not establish polling access.
- [ ] Read the verified RF/TS/service/decoder objects and compare them with the
  receiver GUI and actual decoded picture.
- [ ] Capture healthy reception, RF loss, decoder failure while RF stays locked,
  recovery, receiver restart and a controlled modulation/FEC change if available.
- [ ] Record receiver identity, active path and capture timestamps alongside each
  response. Distinguish configured modulation from actual received modulation.
- [ ] Receive traps at a test listener and identify assertion/clear semantics,
  object indexes and recovery behavior from the matching MIB and real packets.

Preserve existing monitoring destinations when configuring a test listener. The
five documented listener slots are per receiver; all six units can target one
controller. Exact OIDs, enums, scales and notification schemas are pending the
matching package and observed responses. [4.0.1, p. 113][guide401]

## 5. Add configuration and a receiver-status model

- [ ] Add per-source receiver address, protocol/profile, supported capabilities,
  active-path mapping, read-only access settings and polling/freshness controls.
- [ ] Migrate the existing version-1 saved settings, preserving credentials,
  mappings, polarity and policies. Keep GPIO authority as the migration default.
- [ ] Model each required condition as known healthy, known faulty or unknown,
  with an observation age and reason. Expired data must be recognizable as stale.
- [ ] Refresh eligibility only when the necessary mapped conditions are current.
  A successful system-uptime query must not renew an old decoder-health result.
- [ ] Keep the status model independent of SNMP/HTTP transport so captured responses
  and selection behavior can be tested without a receiver.

Current implementation constraints: `src/Settings.cpp` accepts settings schema 1;
an unsupported version falls back to defaults. Configuration/request JSON budgets
are 8 KB and status is 16 KB. Check bounded sizes after adding six receiver records.

## 6. Implement monitoring and the served page

- [ ] Choose a client library/implementation supporting the required SNMP
  manager/poller and trap-receiver roles, compatible with the pinned ESP32 build.
- [ ] Add bounded polling, request/peer/object validation and independent per-unit
  deadlines. An unavailable receiver must not block ATEM or web processing.
- [ ] Add coalesced trap-triggered refreshes, periodic polling and reboot recovery.
- [ ] Extend the status API and page with receiver health, active input/service,
  actual/configured MODCOD, available quality values, age and diagnostic reason.
- [ ] Label qualified GPIO separately from verified receiver/video health. Existing
  `LOCKED` cards and `video lock qualified/lost` events need that distinction.
- [ ] Run monitoring beside current GPIO control and compare each displayed state
  with real receiver behavior before enabling network-based eligibility.

## 7. Integrate eligibility with source selection

- [ ] Provide an explicit choice of GPIO authority, network authority or verified
  combined GPIO-and-network authority; preserve the existing GPIO mode.
- [ ] Add a logical eligibility boundary. `Arbitration::sample()` currently accepts
  electrical HIGH/LOW and applies polarity; a network-health Boolean must not
  accidentally pass through that electrical inversion.
- [ ] Apply qualification once and document total loss-detection delay, including
  polling, timeouts, freshness and the release interval.
- [ ] Preserve five-minute policy semantics, boot/configuration pause, manual and
  external takeover, actual ATEM tallies and Program confirmation handling.

Network-qualified selection must require fresh usable status for the active
decoded feed. Keep the source-rotation interval separate from telemetry freshness.

## 8. Validate and commission all six feeds

- [ ] Use sanitized actual responses as fixtures for healthy/fault/unknown states,
  malformed data, missing fields, unknown enums and incorrect port/service indexes.
- [ ] Test delayed/lost/reordered replies and traps, restart, stale state, one failed
  receiver and bounded processing during a trap burst.
- [ ] Test settings migration and unchanged GPIO/manual/ATEM control behavior.
- [ ] Build both configured ESP32 targets and run the existing native/dashboard checks.
- [ ] On hardware, verify all six source mappings, GPI fault states, common accepted
  video standard, multi-source policy, manual override and network/power recovery.
- [ ] Record the deployed code commit, receiver inventory, settings and results.

## First handoff

The most useful first package is one receiver's **HW Inventory, Licensing, GPI and
SNMP screenshots**, a rear-panel photo, and the matching MIB/API package if already
available. The remaining receivers need a comparison inventory before deployment.

[guide401]: https://manualzz.com/doc/33537261/harmonic-proview-7100-4.0.1-user-guide
[guide41]: https://pdfcoffee.com/proview-7100-4-1-userguide-revb-pdf-pdf-free.html
[icontrol]: https://manuals.plus/m/2a5762dd213de2f2b54b7abcb00e25f2b0d87edc70c6daa4a778e0f2746ee557.pdf
[skyline]: https://docs.dataminer.services/connector/doc/Harmonic_Proview_PVR7000.html
