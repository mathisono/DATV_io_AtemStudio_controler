# Build, install and commission

This firmware runs on the ESP32 and serves its control page locally. The original ATEM Television Studio receives commands over Ethernet. The ESP32 does not connect to the ATEM by USB or RS-232, and no computer is required after configuration.

## 1. Choose the board and input interface

Use the **Olimex ESP32-POE-ISO WROOM** for the wired installation, or the **ESP32-WROOM Wi-Fi development environment** for initial bench work. Read [hardware.md](hardware.md) before attaching receiver alarm outputs. The two environments have different GPIO assignments and default input polarities.

The identified **Harmonic ProView 7100 uses dry-contact relays**. Read the
[receiver compatibility review](receivers/harmonic-proview-7100.md), confirm the
actual GPI pinout and configure the desired alarm assignments before connecting
it. The contact-sensing interface supplies its own sensing voltage. The production
firmware defaults to `activeHigh=false`; retain that only if the complete
interface gives GPIO LOW for the verified healthy condition. Measure receiver
power-off and cable-disconnection behavior as well as RF/decoder faults.

## 2. Install the build tools and compile

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the PlatformIO extension for Visual Studio Code. Open the repository directory. `platformio.ini` pins the Espressif32 platform to **6.9.0**, whose Arduino core is **2.0.17**; let PlatformIO install that environment and the pinned dependencies.

For the recommended wired board:

```sh
pio run -e esp32-poe-iso
```

For a generic ESP32-WROOM development board using Wi-Fi:

```sh
pio run -e esp32dev_wifi
```

These are different firmware targets. Do not flash either one onto the Waveshare S3 board described as a future port in the hardware document.

## 3. Flash over USB

Connect the board's USB data port to your computer. Keep receiver alarm wiring disconnected for this first step.

```sh
pio run -e esp32-poe-iso -t upload
pio device monitor -b 115200
```

Replace the environment name with `esp32dev_wifi` for the bench board. If multiple serial devices are connected, specify the intended port with PlatformIO's `--upload-port` option, and use the same port for the monitor. Examples of OS port names are `/dev/ttyUSB0` on Linux and `COM5` on Windows; use the port your system actually assigns.

The firmware creates a unique admin password, saves it in ESP32 nonvolatile storage, and prints the commissioning credentials over serial. The web username is **`admin`**. The same generated password protects the setup Wi-Fi access point. Keep the credentials with the installation record; there is no universal default password to look up later.

The web application is delivered by the firmware, so a separate web host is unnecessary. This version uses USB flashing for updates; it does not provide web OTA.

## 4. Bring up the network

### Olimex Ethernet

Connect the Olimex Ethernet port and the ATEM Ethernet port to the control network. The Olimex obtains an address through DHCP. Read its address in the serial monitor or the DHCP server's lease list, then open `http://<controller-address>/` in a browser on that network.

A DHCP reservation makes the controller easy to find after restarting. Configure the ATEM with an address on the reachable control network using the appropriate Blackmagic software. The ESP32 starts with its ATEM address field blank; set the real ATEM address on the controller's web page. Entering `0.0.0.0` is not a valid configured switcher address.

The setup/fallback Wi-Fi access point starts after approximately 15 seconds without a network uplink. Its SSID is `DATV-ATEM-` followed by a device suffix. Use the password and address reported by the firmware to reach the page during configuration. Once started, this access point remains available for the rest of that boot. An access point for commissioning does not by itself provide a path to an Ethernet ATEM: restore the wired uplink to establish that control connection.

### Wi-Fi development board

An unconfigured Wi-Fi development board starts its setup access point immediately. Connect using the serial-reported credentials, open the reported setup address, and enter the intended Wi-Fi network settings. Save them, then use **Restart controller** to apply the network change. Once it joins the LAN, access it at its assigned LAN address and configure the ATEM's address.

Wi-Fi and hostname changes require that restart. The web admin password changes immediately for authentication; restart afterward to apply the same new password to the setup access point. The page requests a reload/sign-in when the admin password changes.

A blank password field keeps the saved value. To move to an open Wi-Fi network, use **Clear saved Wi-Fi password**, save, and restart. A new Wi-Fi password must be 8–63 characters; a new web admin password must be 12–63 characters.

Use a network that allows communication between wireless clients and wired equipment. A guest SSID or client-isolation setting can let the web page work while preventing connection to the ATEM.

### Access scope

The control page uses HTTP Basic authentication and a token on write requests. Use it on your AV/control LAN. HTTP does not encrypt credentials or commands; do not expose its port directly to the Internet. ATEM protocol control also depends on access to the switcher's network, independently of the ESP32 web login.

## 5. Configure the six channels

**Automation starts disarmed after every boot.** First verify the connection and configuration; explicitly choose **Resume Auto** only when the installation is ready.

For each enabled alarm, set its name, associated ATEM input and electrical polarity. The alarm channel and ATEM source number are a mapping: alarm 1 can control an ATEM input other than 1 if that is how the cables are connected. Each enabled alarm must map to a distinct ATEM input. Disable unused alarm channels. Saving settings pauses automation and restarts input qualification; review the resulting lock indications before resuming.

The original Television Studio has six external source channels. HDMI and SDI connectors shared by its channels 3 and 4 do not create extra independent inputs. Establish the connector selection and video production standard in the Blackmagic software before commissioning automatic selection.

Configure these timing and arbitration options deliberately:

| Setting | Initial behavior to verify |
| --- | --- |
| Lock acquisition | A HIGH/LOW level must represent lock continuously for 1,000 ms by default before qualifying |
| Lock loss | A loss must persist for 2,000 ms by default before removing that source |
| Multiple locked receivers | Default policy rotates among currently locked sources |
| Rotation interval | 300 seconds (five minutes) by default |
| Other multiple-lock choices | Priority selection, or a configured fixed conflict source after multiple locks persist for the selected interval |
| No-lock behavior | The configured fallback source, initially ATEM Black (source 0); select and verify it before arming |

The default five-minute setting is a **dwell time for rotation while multiple receivers remain locked**, starting when the controller first sees multiple qualified locks. It is not a five-minute video recording buffer, and it does not keep an unlocked receiver eligible for selection. Choose the fixed conflict-source option if multiple receptions that persist for the selected interval should send a particular source, such as a slate or another prepared feed, to Program. That policy returns to the remaining receiver when only one is locked. Priority mode selects the locked channel with the lowest priority number; equal priorities are resolved by alarm-channel order.

## 6. Commission the complete path

Perform these checks with the ATEM's Program output off air or connected to a test destination. Software tests validate the state machine and protocol handling, but do not prove the receiver wiring, a real Ethernet PHY, or a particular ATEM firmware version works.

1. **Confirm manual control.** With automation disarmed, connect to the ATEM from the web page and select known sources. Compare the page's returned Program state with the ATEM's actual Program output. A command being sent is not the same as receiving state confirmation.
2. **Prove alarm meaning and polarity.** On each ProView, compare normal reception, RF loss, decoder failure with RF still locked, boot, receiver power-off and unplugged alarm cable while watching raw/interpreted input status. Each unhealthy case must become ineligible. Do not assume NC means healthy or that merely changing polarity fixes indistinguishable relay states. Current firmware reads GPIO only and has no ProView SNMP/XML or network-freshness check. Complete the receiver review's truth table and resolve any ambiguous states before arming.
3. **Prove the mapping.** Enable one receiver at a time, arm automation and verify that its actual associated video appears on Program after the acquisition interval. Repeat for all six inputs.
4. **Test overlap.** Lock two receivers together and verify the chosen multiple-lock policy. For a quick bench test, temporarily shorten the dwell interval; restore it to 300 seconds afterward. Repeat with a third receiver and with the current source losing lock.
5. **Test short disturbances.** Brief alarm chatter should be filtered by the acquire/release timers. An actual sustained loss must eventually remove that source and produce the configured remaining-source or fallback behavior.
6. **Test manual operation.** Pause automation, use the web controls, and explicitly resume. Manual Program, CUT, AUTO-transition and fade-to-black actions also pause automation; changing Preview alone leaves automation active. If old automatic commands are still awaiting acknowledgement, the controller resets its ATEM session to stop their retries. Wait for fresh feedback and repeat the manual action if the page reports that reconnection is required. Already delivered datagrams cannot be retracted. Confirm the intended source when resuming before making the Program feed live.
7. **Test reconnection.** Disconnect and restore the controller's Ethernet. The page must report the connection loss and recover ATEM state after reconnecting. Verify that the live source decision reflects current lock inputs rather than stale queued selections.
8. **Test a restart.** Power-cycle the controller. Settings should survive, but automation must remain disarmed until Resume Auto is selected. Include this deliberate operator step in station startup procedures.

The controller reacts to the receiver's alarm; it does not verify the picture content. Confirm that all receiver video outputs are accepted by the ATEM at the selected standard. The original ATEM's frame synchronizers do not convert arbitrary input resolutions or frame rates. See [Blackmagic's original technical specifications](https://performancepcus.com/wp-content/uploads/2016/10/atem-television-studio-techspecs.pdf).

## 7. Update and keep an installation record

Disarm automation and use the same environment's USB upload command for updates. Record the repository commit, board variant/revision, ATEM model/firmware, network addresses, ProView firmware/hardware/licenses, relay assignments and measured truth table, video modes, channel map, timing policy and commissioning result. Keep a copy of the previously working firmware/build revision so it can be restored if an update changes behavior.

A successful build or simulated protocol test is not an on-hardware acceptance result. The first installed controller still needs the bench checks above with the actual receiver and ATEM.

## Troubleshooting

| Symptom | Check first |
| --- | --- |
| All alarms look locked with nothing attached | Input polarity and bias; the Olimex production target expects LOW-on-lock, and GPIO13/36 have existing pull-ups |
| Only GPIO36 behaves inconsistently | Its board pull-up and interface wiring; the ESP32 pin has no internal software pull-up/pull-down |
| SD stops working after wiring alarms | GPIO14 is intentionally assigned to an alarm; this build does not use SD/UEXT peripherals |
| Web page opens but ATEM is disconnected | Correct ATEM address, network route, guest/client isolation, switcher power, and protocol/firmware compatibility |
| Manual action says ATEM is reconnecting after Auto paused | The controller abandoned unacknowledged automatic commands; wait for fresh feedback and repeat the manual action |
| Lock status changes but Program does not | Automation is disarmed, channel disabled/mis-mapped, qualification time not complete, or ATEM connection/state not ready |
| Switching occurs but the picture is black | Actual source video format, source/connector selection, cable path, and receiver video output; a lock logic level alone does not establish compatible video |
| Reboot leaves the previous Program picture | Automation deliberately starts disarmed; review status and choose Resume Auto |

Build references: [PlatformIO Core installation](https://docs.platformio.org/en/latest/core/installation/index.html), [PlatformIO Olimex board definition](https://docs.platformio.org/en/latest/boards/espressif32/esp32-poe-iso.html), [Espressif Arduino board variant](https://github.com/espressif/arduino-esp32/blob/2.0.17/variants/esp32-poe-iso/pins_arduino.h).
