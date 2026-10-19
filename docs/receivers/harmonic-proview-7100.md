# Harmonic ProView 7100 compatibility

This installation uses Harmonic ProView 7100 receivers and the **original six-input ATEM Television Studio**, not a later Television Studio HD/4K model. Identifying the receiver supersedes the project's initial assumption of a powered, HIGH-on-lock alarm: the documented ProView GPI interface uses **dry relay contacts**. Verify the installed receiver revision before making the alarm cable. [Harmonic guide 4.0.1, Appendix C, pp. 150–152][guide401]

## What is supported today

The firmware already reads six GPIO channels, applies configurable polarity and qualification delays, selects ATEM sources, and serves a control page. It **does not yet poll ProView SNMP/XML status or receive ProView traps**. The network-monitoring design below is a proposed extension; it must not be treated as an installed receiver-health check.

The existing GPIO status named `locked` means that the configured electrical input has qualified. It is not independent proof of RF reception, successful decoding, or a usable picture at the ATEM.

## 1. GPI contacts and ESP32 wiring

### Documented connector

The following is the **GPI HD15 female connector**, not the receiver's similarly shaped RGB or audio connector. The 4.0.1 and 4.1 guides give this contact assignment. [4.0.1, Table C–9, pp. 151–152][guide401]; [4.1, Table C–9, pp. 152–153][guide41]

| Relay | COM pin | NC pin | NO pin |
|---|---:|---:|---:|
| 1 | 1 | 6 | 11 |
| 2 | 2 | 7 | 12 |
| 3 | 3 | 8 | 13 |
| 4 | 4 | 9 | 14 |
| 5 | 5 | 10 | 15 |

An older v2.6 guide labels pins 6–10 unused. **NC availability is not universal across the documentation.** Check the actual hardware and use molded connector pin numbers to avoid reversing the view. [v2.6 Rev E, Table B–4, p. 187][guide26]

### Relay setup and meaning

Under **Administration → GPI**, select one of five relays and choose manual On, manual Off, or Alarm Triggering with selected individual alarms. [4.1, GPI, p. 114][guide41]

Engineering recommendation: allocate one relay per receiver to the specific reception/decoder path that feeds its ATEM input. Avoid grouping unrelated faults from unused receiver ports into the source decision. Six physical receivers therefore need six contact-sensing circuits, even though each receiver has additional relays.

For a dry-contact bench connection, a usable arrangement is:

| Connection | Controller-side wiring |
|---|---|
| One relay COM, for example pin 1 | Controller GND |
| Corresponding NO, for example pin 11 | Protected/filtered input leading to the assigned GPIO |
| GPIO bias | Pull-up to controller **3.3 V** |

This produces GPIO LOW when that contact closes and HIGH when it opens. It does **not** establish which state means healthy reception. Confirm the relay behavior first, then configure the input polarity or provide additional conditioning. The production board's default `activeHigh=false` is appropriate only when the complete interface produces LOW for the desired qualified condition. See [controller GPIO assignments and existing pull-ups](../hardware.md).

The relay supplies no sensing voltage. Do not add a 5 V or 24 V signal directly to an ESP32 pin. A verified isolated dry contact can be sensed with controller-side power; an additional optocoupler is not required merely to translate a receiver voltage. If an isolated input module is retained, choose its wetting supply and current from the module/contact specifications. Relay contact ratings and minimum wetting current were not established by the reviewed documents.

### Verify fault and power behavior

The reviewed documentation does not establish an energized-healthy relay configuration or a complete boot/power-loss truth table. **Do not hardcode NC closed = healthy.** A deenergized relay can resemble its no-alarm contact state.

Record measured contact state, GPIO level, and intended eligibility for each case:

| Commissioning case | Desired eligibility |
|---|---|
| Selected service decodes normally | Eligible after acquisition delay |
| RF lock lost | Ineligible after loss delay |
| RF remains locked but selected video cannot decode | Ineligible |
| Receiver boots or has no power | Ineligible |
| Alarm cable disconnected | Ineligible |

If no single contact/polarity combination distinguishes healthy video from every listed unhealthy condition, the present one-bit input is insufficient. Add a proven hardware condition or implement the fresh receiver-status check described below before enabling automatic operation. A polarity change alone cannot resolve physically indistinguishable healthy and unhealthy states.

## 2. Video connection to all six ATEM inputs

Decoder cards and licenses vary. Harmonic documents two SDI connectors per video channel and HDMI for single-channel decoder configurations; video decoding is an optional hardware function. Check every unit's rear panel and installed decoder/licenses. [Official datasheet, pp. 3–4 and notes][datasheet]

One practical six-receiver wiring plan is:

| Receiver | ATEM input | Video connection |
|---|---:|---|
| 1 | 1 | HDMI; use SDI→HDMI conversion if the receiver lacks HDMI |
| 2 | 2 | HDMI; use SDI→HDMI conversion if the receiver lacks HDMI |
| 3 | 3 | SDI; configure the ATEM's shared input for SDI |
| 4 | 4 | SDI; configure the ATEM's shared input for SDI |
| 5 | 5 | SDI |
| 6 | 6 | SDI |

The original ATEM has six usable inputs and four SDI input connectors. Thus six SDI-only receivers need two SDI→HDMI conversions. The table is a wiring recommendation, not a required alarm mapping. [Original ATEM specifications, Connections][atem]

Use the receiver's **decoded SDI/HDMI** output. ASI BNC connectors carry compressed transport streams and cannot substitute for SDI video, despite their physical similarity. [Harmonic datasheet, Transport Stream Output Interfaces and Video and Audio Interfaces][datasheet]

### Match one production standard

Harmonic's interlaced labels express frames per second; the equivalent ATEM labels express fields per second. The compatible common choices are:

| ProView output notation | Original ATEM standard |
|---|---|
| 720p @ 50 | 720p50 |
| 720p @ 59.94 | 720p59.94 |
| 1080i @ 25 | 1080i50 |
| 1080i @ 29.97 | 1080i59.94 |
| 576i @ 25 | 625i50 PAL |
| 480i @ 29.97 | 525i59.94 NTSC |

Native 60 Hz HD modes and 1080p are outside this original ATEM's documented standards. Set all feeds to the same accepted standard. [Harmonic video formats, datasheet p. 3][datasheet]; [ATEM specifications, Standards][atem]

The ProView's **Automatic** display setting follows the incoming resolution/frame rate. Configure and verify a fixed suitable output where the installed decoder supports it. [4.1, Video tab, pp. 85–86][guide41]

No universal 50↔59.94↔60 baseband frame-rate conversion was established by this review. In particular, the datasheet's optional compressed-video transcoding section is not proof that every SDI decoder output is a universal standards converter. A connector converter also does not necessarily convert the video standard. Confirm actual output timing and picture on the ATEM multiview for each incoming mode.

## 3. RF and DATV limits

The official datasheet specifies **950–2150 MHz** RF input and **1–45 Msym/s** for DVB-S/S2. Newer S2X/APSK capabilities have card/license conditions; its notes identify RF board `HW-PVR-7100-S2X-B-0004`. [Datasheet, RF Input Interfaces and notes, pp. 3–4][datasheet]

Consequently, **125/250/500 ksym/s DATV is below the documented 1 Msym/s lower limit**. Do not assume those modes work merely because the receiver supports DVB-S2. The controller cannot extend a receiver's demodulation or codec capabilities. Record each receiver's RF card, decoder card, firmware, licenses, and intended signal modes before treating all six units as equivalent.

## 4. SNMP, SMTP, and richer receiver status

**SNMP** is the relevant management protocol; **SMTP** is email transport. Harmonic lists SNMP traps/alarms and an XML control/monitoring interface. This review did not establish a ProView SMTP alarm-message format. [Datasheet, Control and Monitoring, p. 4][datasheet]

Each receiver supports **five trap listeners**, with address, UDP port, community, and description settings. These are five destination slots **per receiver**, not a five-receiver system limit: all six receivers can send traps to the same controller address. The RF status display includes lock, modulation standard, modulation/FEC, symbol rate, C/N, Eb/No, link margin, and PER. These documented GUI fields do not establish their exact SNMP OIDs, table indexes, value scales, or XML request schema. [4.0.1, pp. 113 and 122][guide401]

SNMP polling is supported beyond trap notifications. Grass Valley's own ProView 7000/7100 integration documentation lists polled RF and decoder alarms, including carrier not detected, demodulation failure, MPEG sync loss, program decoding failure, and missing PIDs; it also documents the SNMP read community and polling controls. Its BER/Eb/N/PER threshold alarms demonstrate alarm availability, not access to the underlying numerical measurements. The guide does not supply the corresponding OIDs. [iControl Supported Device Reference Guide, pp. 91 and 95–98][icontrol]

Detailed modulation telemetry depends on the receiver firmware and interface. Skyline's PVR7000-family connector documentation associates its SSH integration with receiver firmware 3.x and modulation parameters with firmware **3.3 or later**; its HTTP integration requires receiver firmware **4.x**. Newer connector ranges also retrieve alarms over HTTP. Skyline's connector version numbers are separate from the receiver firmware versions. This is evidence of working family integrations, not a ProView 7100 API contract or proof that actual detected MODCOD/FEC is available through a particular SNMP object. Verify the installed firmware and distinguish configured modulation from received modulation before implementing either field. [Skyline connector documentation][skyline]

### Proposed integration, not current firmware behavior

Use periodic read-only polling for a current snapshot and traps to request prompt refreshes. Avoid treating receipt of one historical “locked” event as a permanent eligibility flag. Obtain the MIB/API documentation matching the deployed firmware and capture actual responses before implementing specific field mappings.

A proposed per-source eligibility expression is:

```text
eligible = enabled
           AND receiver_status_is_fresh
           AND selected_input_is_usable
           AND selected_program_is_decoding
           AND video_output_standard_is_compatible
```

Only use terms that can be measured reliably. If an output-format field is unavailable remotely, verify it during commissioning instead of inventing a runtime status. Apply the existing acquisition/loss delays after determining the raw eligibility condition.

Keep receiver identity, RF input, logical transport stream, selected service/decoder, and ATEM source as explicit mappings. An unused tuner fault should not veto healthy video, and receiver-level primary/backup recovery must be reflected in the status of the active decoded output.

Poll all six receivers independently. Use response deadlines and a bounded freshness interval so one unresponsive unit does not block ATEM control or the web page. A trap should trigger a refresh; missing or reordered events must not leave an old healthy state on the dashboard. After a receiver reboot or controller restart, require fresh verified status before eligibility. Expose unavailable or stale modulation data as unknown instead of displaying a retained value as current.

## Evidence needed for commissioning

For each hardware variant, collect a rear-panel photo, firmware version, hardware inventory, installed licenses, selected RF/decoder path, and output mode. For network status, obtain the matching MIB/API package and representative healthy, unlocked, and decoder-failure responses. For contact operation, complete the fault/power truth table above. Keep automation paused until these checks establish the actual meaning of each source's input.

## Sources and scope

Reviewed on 2026-10-19. Page numbers refer to printed document pages. The datasheet is hosted by Harmonic; the older user guides and original Blackmagic specification are manufacturer-authored documents available through mirrors. Hardware/firmware-specific behavior takes precedence over assumptions based only on a product family name.

[datasheet]: https://www.harmonicinc.com/hubfs/datasheet/proview-7100.pdf "Harmonic ProView 7100 datasheet, November 16, 2022"
[guide401]: https://manualzz.com/doc/33537261/harmonic-proview-7100-4.0.1-user-guide "Harmonic ProView 7100 Release 4.0.1 User Guide, Rev A, February 2016"
[guide41]: https://pdfcoffee.com/proview-7100-4-1-userguide-revb-pdf-pdf-free.html "Harmonic ProView 7100 Release 4.1 User Guide, Rev B, December 2016"
[guide26]: https://manualzz.com/doc/61920167/harmonic-proview-7000--proview-7100-user-manual "Harmonic ProView 7000/7100 v2.6 User Guide, Rev E"
[atem]: https://performancepcus.com/wp-content/uploads/2016/10/atem-television-studio-techspecs.pdf "Blackmagic original ATEM Television Studio technical specifications"
[icontrol]: https://manuals.plus/m/2a5762dd213de2f2b54b7abcb00e25f2b0d87edc70c6daa4a778e0f2746ee557.pdf "Grass Valley iControl Supported Device Reference Guide, M226-0900-112, 8 June 2015"
[skyline]: https://docs.dataminer.services/connector/doc/Harmonic_Proview_PVR7000.html "Skyline Communications Harmonic Proview PVR7000 connector documentation"
