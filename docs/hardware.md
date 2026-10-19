# Deployment hardware and receiver lock wiring

## Recommended build

Use an **Olimex ESP32-POE-ISO with an ESP32-WROOM module**, six electrically conditioned lock inputs, and wired Ethernet to the same control network as the ATEM. The firmware environment is `esp32-poe-iso`. The standard 4 MB board is sufficient for this build; a WROVER variant has different Ethernet-clock/PSRAM pin requirements and is not the configured target.

The board provides 100 Mb Ethernet, IEEE 802.3af PoE, and a USB programming interface. Its isolated PoE supply is useful when attaching a computer for commissioning. **“ISO” describes isolation of the PoE power circuit; it does not isolate the GPIO inputs from the receivers.** Those require their own input interface. [Olimex product and variants](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/open-source-hardware), [manufacturer manual, GPIO/power sections](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/resources/ESP32-POE-user-manual.pdf).

The ESP32 carries control information only. Each demodulator's decoded HDMI/SDI output connects to its associated ATEM video input independently of the alarm cable. Check that the alarm actually means usable decoded video, rather than only RF carrier or transport-stream lock. The controller cannot establish valid video by measuring a logic voltage.

### Bill of materials

| Item | Quantity | Selection requirement |
| --- | ---: | --- |
| Olimex ESP32-POE-ISO, WROOM variant | 1 | Standard board targeted by this firmware; inspect the revision printed on the delivered PCB |
| Contact-sensing input interface | At least 6 channels | For the ProView dry contacts, use protected/filtered sensing with a 3.3 V-compatible controller output; an isolated module may be used with its specified wetting supply/current |
| Controller-side pull-up resistors | As required for 6 channels | 10 kΩ to the controller's 3.3 V rail is a bench starting value; account for existing board pull-ups, noise and the relay's required contact current |
| Ethernet switch and patch cables | As required | Controller and ATEM must have IP connectivity; use an IEEE 802.3af PoE switch/injector if powering the Olimex over Ethernet |
| USB data cable | 1 | Micro-USB for initial flash and serial commissioning |
| Enclosure, terminal connectors, strain relief | 1 set | Label alarm number, associated ATEM input, signal, and return; protect the board from conductive contact |
| Receiver-side interface supply | If required | Use the input module's specification; do not assume the ESP32 can supply the field side of every channel |

Olimex's manual limits the combined external load on this board's 5 V and 3.3 V outputs to approximately **1 W**. Include the input-interface load in the power budget. The ATEM uses its own supply; this project does not power it.

## Identified receiver: Harmonic ProView 7100

The ProView 7100 has **five configurable dry-contact relays** at its GPI connector.
This supersedes the original assumption of a powered HIGH-on-lock output. Allocate
one configured relay from each of the six receivers to the six controller inputs.
See the [ProView-specific pinout, revision caveats and commissioning truth
table](receivers/harmonic-proview-7100.md) for the manufacturer references.

A verified isolated contact can connect controller ground to a protected GPIO
input with a pull-up to the controller's own 3.3 V rail: closed reads LOW, open
reads HIGH. The contact supplies no voltage. An additional optocoupler is optional
for this voltage translation; input protection, filtering and the contact's
electrical ratings still determine the final interface. An industrial isolated
input module may instead sense the contact using its own rated wetting supply.

**Closed does not automatically mean healthy.** Configure the selected receiver
alarms and measure healthy video, RF loss, decoder failure, receiver boot,
receiver power-off and disconnected-cable states. The manuals do not establish
an energized-healthy relay truth table. If healthy video and an unhealthy case
look identical, software polarity cannot distinguish them. Use an additional verified condition
before enabling automatic switching. Current firmware has no receiver network
freshness check to resolve this ambiguity.

## Six input connections on the Olimex

These assignments were checked against Olimex's **revision N** schematic and the Arduino ESP32 **2.0.17** board variant. Header pin numbers are schematic connector numbers, not a universal ESP32 numbering scheme. Identify pin 1 from the PCB before wiring. Check the manufacturer's schematic for a different delivered revision.

| Alarm channel | ESP32 GPIO | Olimex header | Revision N board connections to account for |
| --- | ---: | --- | --- |
| 1 | 4 | EXT1 pin 9 | Also UEXT UART TX; leave that peripheral unused |
| 2 | 13 | EXT2 pin 10 | Also UEXT I²C SDA; **R35 is a 2.2 kΩ pull-up to 3.3 V** |
| 3 | 14 | EXT2 pin 9 | Also SD clock/UEXT clock; leave SD and UEXT peripherals unused |
| 4 | 32 | EXT2 pin 6 | General GPIO in this build |
| 5 | 33 | EXT2 pin 5 | General GPIO in the configured WROOM build |
| 6 | 36 | EXT2 pin 2 | Input only; **R37 is a 10 kΩ board pull-up to 3.3 V**; also UEXT receive circuitry |

Controller 3.3 V is EXT1 pin 2; controller ground is EXT1 pin 3. Do not install an SD card or attach UEXT peripherals with these assignments. GPIO13/14 are also JTAG functions, so use USB serial for this build's commissioning.

References: [Olimex revision N schematic](https://github.com/OLIMEX/ESP32-POE-ISO/blob/master/HARDWARE/ESP32-PoE-ISO-Rev.N/ESP32-PoE-ISO_Rev_N.pdf), [machine-readable manufacturer schematic](https://github.com/OLIMEX/ESP32-POE-ISO/blob/master/HARDWARE/ESP32-PoE-ISO-Rev.N/ESP32-PoE-ISO_Rev_N.kicad_sch), [Arduino 2.0.17 board variant](https://github.com/espressif/arduino-esp32/blob/2.0.17/variants/esp32-poe-iso/pins_arduino.h).

### Optional isolated interface for powered HIGH-on-lock alarms

This section applies to a separately verified powered logic alarm or an interface
engineered to energize when healthy. It is not the ProView's native dry-contact
output. For such a HIGH-on-lock source, arrange the isolated interface so that the
asserted alarm turns its optocoupler on. On the ESP32 side, the corresponding
transistor sinks the GPIO to controller ground.

| Connection | Destination |
| --- | --- |
| Receiver alarm output | Input interface's correctly rated field-side signal terminal |
| Receiver alarm reference/return | The interface's field-side return, according to its input circuit |
| Interface controller-side collector/open-drain output | The assigned ESP32 GPIO |
| Interface controller-side emitter/output return | ESP32 controller ground |
| GPIO pull-up | ESP32 3.3 V through the selected pull-up resistor |

**The receiver can still be HIGH-on-lock while the ESP32 GPIO is LOW-on-lock.** Set the channel's `activeHigh` option to `false`, which is the Olimex firmware default. The dashboard's interpreted lock status then follows the receiver correctly.

| Receiver condition | Isolator output transistor | ESP32 pin | Interpreted lock |
| --- | --- | --- | --- |
| No lock, or field-side cable unplugged | Off | HIGH through pull-up | No lock |
| Valid lock, interface energized | On | LOW | Locked |

This wiring makes an open alarm cable appear unlocked. It cannot diagnose every fault: a short to controller ground can still look like a lock. Keep the field-side return separate from controller ground when the interface is intended to provide galvanic isolation; bonding them together bypasses that isolation.

The existing 2.2 kΩ pull-up on GPIO13 must be included when choosing the isolator. With an additional 10 kΩ pull-up, that channel needs roughly 1.8 mA of sink capability near ground; choose a device that meets the ESP32 LOW threshold with margin at the receiver's minimum alarm drive and the optocoupler's minimum guaranteed transfer ratio. Do not select LED series resistors until the receiver's voltage and available output current are known. A commercial input module must explicitly support the signal and controller-side supply being used.

### Why generic HIGH-on-lock wiring needs additional care

GPIO13 and GPIO36 are already pulled HIGH on the Olimex board. An unplugged direct HIGH-on-lock signal can therefore look locked. A 10 kΩ pull-down does not cancel the 2.2 kΩ pull-up on GPIO13; on GPIO36 it makes a nominal mid-rail divider against the board's 10 kΩ pull-up.

For a direct non-inverting interface, use an actively driven, correctly powered 3.3 V output and engineer the disconnected/power-off state, or revise the bias circuit deliberately. Changing a software polarity flag does not fix an incompatible voltage or an indeterminate input level. A verified healthy-state sinking interface suits the configured Olimex pins; the ProView relay's actual state must be established separately.

## Receiver output types

The installed receiver model is now identified as the ProView 7100, with documented
dry contacts. Its actual connector revision, contact ratings and configured
fault/power behavior still need to be checked on the delivered units. The table
below is a reference for other receivers or external alarm interfaces; it does
not imply that the ProView provides each of these output types.

| Actual alarm output | Suitable interface approach |
| --- | --- |
| Verified 3.3 V push-pull logic | A compatible buffer or appropriately driven isolator; a short common-ground bench connection is possible after checking both devices' electrical limits |
| 5 V push-pull logic | Rated level conversion or isolation; do not connect directly to ESP32 GPIO |
| 12 V or 24 V alarm voltage | A digital-input interface rated for that voltage and its transients; do not connect directly |
| Open collector/open drain | Establish which state turns the transistor on, its voltage/current rating, and whether a field-side supply/pull-up is needed; “HIGH” may merely mean released |
| Dry contact/relay | Use a contact-sensing input circuit; a contact does not generate a voltage by itself |
| Unknown or multiplexed alarm pin | Obtain the receiver pinout and measure its states before selecting the interface |

The ESP32's 3.3 V-domain input specification gives a guaranteed HIGH starting at **0.75 × VDD** and LOW up to **0.25 × VDD**: nominally 2.475 V and 0.825 V at a 3.3 V supply. Its documented upper input range is VDD + 0.3 V; that is not a reason to design a 3.6 V signal. Use nominal 3.3 V logic and do not assume 5 V tolerance. GPIO34–39 have no software-controlled pull-up/pull-down, including GPIO36; the pull-up noted above is a component on the Olimex PCB. [Espressif ESP32 datasheet, DC characteristics](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf), [Espressif GPIO reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html).

## Ethernet configuration for this build

The standard WROOM board's Arduino variant defines:

| Setting | Value |
| --- | --- |
| PHY | LAN8720 |
| PHY address | 0 |
| MDC | GPIO23 |
| MDIO | GPIO18 |
| PHY power control | GPIO12 |
| RMII clock | `ETH_CLOCK_GPIO17_OUT` |

With Arduino ESP32 2.0.17, the equivalent explicit initializer is:

```cpp
ETH.begin(0, 12, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO17_OUT);
```

Do not substitute the argument order from Arduino 3.x examples. This project uses PlatformIO's `esp32-poe-iso` board definition and a pinned Arduino 2.x toolchain. See the [PlatformIO board reference](https://docs.platformio.org/en/latest/boards/espressif32/esp32-poe-iso.html) and [pinned Arduino variant](https://github.com/espressif/arduino-esp32/blob/2.0.17/variants/esp32-poe-iso/pins_arduino.h).

## Other hardware options

### Low-cost bench development

The `esp32dev_wifi` environment targets a conventional ESP32-WROOM development board over Wi-Fi. Its six input pins are **16, 17, 18, 19, 21, 22**, HIGH-on-lock by default, with internal pull-downs. Test it using known 3.3 V logic or switches to the board's own 3.3 V supply. These are not the Olimex pin assignments. Check that an actual development board exposes these pins and does not use them for attached peripherals or PSRAM.

### Integrated DIN-rail input hardware: future port

The **Waveshare ESP32-S3-POE-ETH-8DI-8DO** combines a DIN-rail enclosure, eight optoisolated digital inputs and W5500 Ethernet. It is a reasonable alternative when integrated field wiring matters more than keeping the current board target. It is **not a supported flash target in this version**: the manufacturer requires Arduino ESP32 3.0.0 or later, and its SPI Ethernet interface needs a separate board profile/network initialization.

For a future port, the manufacturer maps DI1–6 to GPIO4–9; W5500 IRQ12, MOSI13, MISO14, clock15, CS16, reset39. Its active-input voltage range and wiring must still match the demodulator; an isolated industrial input is not automatically compatible with a weak 3.3 V logic output. [Waveshare hardware reference](https://docs.waveshare.com/ESP32-S3-POE-ETH-8DI-8DO), [Waveshare Arduino requirements](https://docs.waveshare.com/ESP32-S3-POE-ETH-8DI-8DO/Arduino).
