# LattePanda 3 Delta / Linux Mint

This is a second deployment target. The original ESP32 sources, board profiles,
PlatformIO environments and switching behavior remain supported. Both hosts use
the **same** C++ arbitration, Program coordinator and ATEM session engine in `lib/`.
Linux wraps that code in a small shared library and uses Python's standard library
for USB serial, UDP sockets, configuration and HTTP. There are no Python runtime
packages to install. Linux Mint with Python 3.10+ and a C++17 compiler is the
deployment target; the automated Linux checks do not certify a particular Mint
kernel, USB installation or physical switcher firmware.

## What runs where

| Component | Responsibility |
| --- | --- |
| Onboard ATmega32U4 / LattePanda Leonardo | Six inputs, 20 ms glitch filter, full input snapshots over internal USB CDC |
| Mint service | Input polarity and qualification, routing policies, ATEM control, dashboard, saved settings and logs |
| Mint Network Manager | Ethernet, Wi-Fi, DHCP/static addressing and hostname |
| Existing ESP32 targets | Standalone alternative; do not run two armed controllers against one ATEM |

The Arduino does **not** control ATEM by itself. Automatic switching requires the
Linux process and its Ethernet connection. It does not carry or convert video.
SNMP/XML polling, traps and live modulation reporting are still pending the
ProView firmware/MIB collection described in [the receiver checklist](receivers/proview-next-steps.md).

The existing three routing policies remain rotation, priority and a designated
source after continuous concurrent lock. The five-minute interval defaults to
300 seconds. **This commit does not add a rolling history of separate lock events.**
See [behavior](behavior.md) for exact timer semantics.

## Input firmware and wiring

| Receiver alarm | Arduino digital pin | Snapshot bit |
| --- | --- | --- |
| 1 | D4 | 0 |
| 2 | D5 | 1 |
| 3 | D6 | 2 |
| 4 | D7 | 3 |
| 5 | D8 | 4 |
| 6 | D9 | 5 |

All six pins use `INPUT_PULLUP`. The default application polarity is **LOW =
asserted**, suitable for a verified dry-contact or isolated open-collector
interface pulling the controller pin toward controller ground. Changing logical
polarity in the dashboard does not change the electrical pull-up. Use an
appropriate isolated sensing interface for the ProView relay contacts; verify
the board pin labels, receiver connector revision, contact ratings and complete
healthy/fault/power-off/unplugged truth table before connecting equipment.
Do not assume a closed NC contact proves healthy decoded video. Do not attach a
powered receiver alarm directly to these pins or reuse ESP32 voltage assumptions.

The firmware never configures the six pins as outputs. D0/D1 are not used: the
host connection is the **internal USB serial link**, not the external UART.

Build the ATmega32U4/16 MHz firmware independently of the ESP32 project:

```sh
sudo apt install python3-venv
python3 -m venv .venv
.venv/bin/python -m pip install platformio==6.1.18
.venv/bin/python -m platformio run -d firmware/lattepanda-inputs
```

The PlatformIO environment is a standard Leonardo **compile profile**, pinned to
Atmel AVR 5.1.0. LattePanda documents a vendor-specific Leonardo board definition
and USB identity. For first installation, use the manufacturer's
[LattePanda Leonardo configuration and upload instructions](https://docs.lattepanda.com/content/3rd_delta_edition/drivers_and_software/).
In Arduino IDE, install/select **LattePanda AVR Boards → LattePanda Leonardo**,
create a sketch named `datv_inputs`, and copy the contents of
`firmware/lattepanda-inputs/src/main.cpp` into `datv_inputs.ino`. Select the onboard
USB serial port, compile and upload. A compile check using standard Leonardo is
not a claim that automatic PlatformIO port discovery/upload has been verified
against the factory LattePanda bootloader. Do not change bootloaders or fuses.

Stop the Linux service and close serial monitors before uploading. The firmware
does not wait for a serial monitor at boot. After uploading, identify the
application-mode device again because its USB name may differ from the factory
firmware or bootloader:

```sh
ls -l /dev/serial/by-id/ /dev/serial/by-path/
```

Prefer the unique application-mode `/dev/serial/by-id/...` link; use the observed
internal USB `/dev/serial/by-path/...` link if no unique serial ID is exposed.
Do not assume `/dev/ttyACM0` stays assigned to the onboard controller. Both names
can change when different board definitions/USB descriptors are flashed.

## Run on Mint

From the repository root:

```sh
sudo apt update
sudo apt install build-essential python3
sudo usermod -aG dialout "$USER"
python3 linux/build.py
python3 linux/controller.py --init --config config.local.json
```

Log out and back in after adding your account to `dialout`. Initialization prints
the generated password and creates a mode-0600 config file; it refuses to overwrite
an existing file. Set `serialDevice` in that file to the observed stable link.
Never commit local configuration or credentials. Start:

```sh
python3 linux/controller.py --config config.local.json
```

Open **http://127.0.0.1:8080/** and sign in as `admin` with the generated password.
Enter the ATEM address and verify mappings, polarity and actual Program feedback.
The service defaults to localhost. For a trusted, isolated control LAN, explicitly
bind the LAN address with `--bind <LattePanda-control-LAN-IP>`. HTTP Basic
credentials are not encrypted; use an HTTPS reverse proxy or an SSH tunnel when
accessing across an untrusted network. Do not expose the HTTP service publicly.

The shared dashboard hides ESP32 Wi-Fi/hostname/reboot controls on Linux. Use Mint
to configure networking. Switching settings and password changes apply immediately;
they do not reboot Linux. Restart the service with `systemctl` when required.

## Start as a service

Build the shared library first, then perform this initial installation from the
repository root (create the `datv` account only if it does not already exist):

```sh
sudo useradd --system --user-group --home-dir /var/lib/datv-atem --no-create-home --groups dialout datv
sudo install -d /opt/datv-atem/build
sudo cp -a linux web /opt/datv-atem/
sudo install -m 0644 build/libdatv.so /opt/datv-atem/build/libdatv.so
sudo install -d -o datv -g datv -m 0700 /var/lib/datv-atem
sudo -u datv python3 /opt/datv-atem/linux/controller.py --init --config /var/lib/datv-atem/config.json
sudoedit /var/lib/datv-atem/config.json
sudo install -m 0644 linux/datv-atem.service /etc/systemd/system/datv-atem.service
sudo systemctl daemon-reload
sudo systemctl enable --now datv-atem
sudo journalctl -u datv-atem -f
```

Set `serialDevice` during the `sudoedit` step. The service runs as the unprivileged
`datv` user with `dialout` access, restarts after process failure and starts without
a desktop login. It starts **paused after every process restart**. For LAN access,
place `DATV_BIND=<control-LAN-IP>` and optionally `DATV_PORT=8080` in
`/etc/default/datv-atem`, then `sudo systemctl restart datv-atem`. No GPIO/serial
permissions need to be made world-writable. Do not enable `PrivateDevices` in the
unit because it would hide the onboard USB device.

For updates, stop the service, replace `linux/`, `web/` and the freshly built
library, then restart. Keep `/var/lib/datv-atem/config.json` to retain settings.
The systemd unit does not configure BIOS auto-power-on or a hardware watchdog;
those remain machine commissioning tasks.

## Serial protocol and failure behavior

- USB CDC is opened at 115200, with DTR asserted. No 1200-baud reset is issued.
- The host sends `Q1 <eight-lowercase-hex-digit-token>\n`, normally every 50 ms.
- The Arduino replies `S1 <same-token> <decimal-mask-0-through-63>\r\n`.
- Bit 0 corresponds to D4, bit 5 to D9; **1 means electrical HIGH**, not lock.
- Every reply contains all six filtered levels. There is no dependence on an
  edge occurring after startup, and no ATEM or GPIO-write command in this protocol.
- Only a reply to the outstanding token received within 250 ms is accepted.
  Duplicate, malformed, oversized, unsolicited and old-session responses do not
  refresh the input timestamp. This is freshness correlation, not authentication.
- After one second without a valid snapshot, a disconnect, or a host scheduling
  stall beyond the deadline, all inputs become **unknown**, automation pauses,
  and qualified lock history is cleared. An unresolved automatic command is
  abandoned by resetting its ATEM session before further retries.
- Unknown input state never requests the no-lock fallback. The service leaves
  the ATEM alone; it cannot undo a packet already sent or a change already applied.
- Serial reconnection retries automatically. Current snapshots requalify the
  inputs; an operator must explicitly resume Auto. Manual ATEM controls remain
  available whenever ATEM feedback is synchronized, even without the Arduino.
- Acquisition/loss timing runs on Linux using the existing state machine, on top
  of the Arduino's 20 ms filter. Sampling and scheduling add latency; this is not
  a hard real-time failover system.

## Verification and commissioning

```sh
python3 linux/build.py
python3 -m unittest discover -s test/linux -v
python3 scripts/test_native.py
npm ci --ignore-scripts
npm test
.venv/bin/python -m platformio run -d firmware/lattepanda-inputs
.venv/bin/python -m platformio run -e esp32-poe-iso -e esp32dev_wifi
```

Host tests exercise the compiled shared core, pseudo-terminal serial, loopback UDP
and HTTP authentication/CSRF/settings persistence. They cover stale and partial
frames, late/duplicate replies, reconnection, paused startup, actual feedback,
external takeover, cancellation of pending commands, and unknown input display.

No physical LattePanda, ProView or ATEM was available for these tests. Before
deployment, validate all six inputs, application/bootloader USB identity, service
permissions, live ATEM firmware interoperability, source video formats, unplugged
serial, a stopped Arduino, host suspend/resume, power restoration and extended
operation. Keep automation paused until the contact wiring and mappings are proven.

Hardware references:
[internal USB / Arduino interface](https://docs.lattepanda.com/content/3rd_delta_edition/io_playability_Internal/),
[pinout](https://docs.lattepanda.com/content/3rd_delta_edition/io_playability/),
[board specifications](https://docs.lattepanda.com/content/3rd_delta_edition/specification/).
