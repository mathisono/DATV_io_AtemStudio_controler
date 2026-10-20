# Local HTTP API

The Linux target serves these control/status endpoints and dashboard on port 8080 by default. It adds `arduino` freshness, per-input `known` (unknown levels are null), and `capabilities` flags. Linux networking stays in the OS; `/api/reboot` is not exposed. Serial identity is configured locally. See [LattePanda details](lattepanda.md).

The ESP32 serves HTTP on port 80. Every route uses HTTP Basic authentication:
username `admin`, device password printed over USB serial and saved in NVS. HTTPS
is not implemented; use a controlled local network. All write requests require
`Content-Type: application/json` and `X-CSRF-Token` copied from a current
`GET /api/status` response. A new boot generates a new token. Credentials and
tokens are not URL parameters.

## Routes

| Method and path | Request | Result |
| --- | --- | --- |
| `GET /` | — | Embedded dashboard |
| `GET /api/status` | — | Current network, alarm, controller and ATEM feedback state, plus configuration and CSRF token |
| `POST /api/mode` | `{"mode":"auto"}` or `{"mode":"manual"}` | Resume/pause automatic routing |
| `POST /api/action` | See actions below | Send a manual switcher command |
| `POST /api/config` | Full configuration object, plus optional new passwords | Validate and save configuration; pause Auto |
| `POST /api/reboot` | `{}` | Reply, then restart after approximately 500 ms |

All successful writes return `{"ok":true,"message":"..."}`. A successful action
means accepted for transport, not confirmed on-air. Poll status for actual
Program/Preview feedback. JSON errors return `{"ok":false,"error":"..."}` with
an appropriate non-2xx status. Authentication failures use the browser's Basic
authentication challenge. A disconnected/busy command is rejected rather than
stored for later execution.

## Manual actions

```json
{"action":"program","source":1}
```

`program` and `preview` accept an integer `source` from 0 (Black) through 6.
`cut`, `auto`, and `ftb` do not take a source. All actions except `preview` pause
automatic routing. `auto` means the ATEM's configured transition; it does not
enable this controller's automatic routing. Use `/api/mode` for that.

`ftb` toggles Fade to black using the ATEM command. It is not a request to set an
absolute FTB state, so clients must not blindly repeat the HTTP request after an
ambiguous response. Consult the reported feedback before another operator action.

## Configuration

Start with the `config` object returned by status, edit it, and send the complete
object. Ordinary configuration writes do not return stored passwords.

| Field | Type/range |
| --- | --- |
| `atemIp` | Numeric unicast IPv4 string; empty disables the configured destination |
| `hostname` | 1–31 letters/digits/hyphens; hyphens cannot be first/last |
| `wifiSsid` | String, 0–32 bytes |
| `wifiPassword` | Optional new 8–63 character password; blank/omitted preserves the current one |
| `clearWifiPassword` | Optional boolean; `true` explicitly clears the saved Wi-Fi password and cannot accompany a nonempty new one |
| `adminPassword` | Optional new 12–63 character password; blank/omitted preserves the current one |
| `acquireMs`, `releaseMs` | Integers, 0–60,000 |
| `switchIntervalSec` | Integer, 1–86,400 |
| `multiPolicy` | `round_robin`, `priority`, or `conflict` |
| `conflictSource`, `fallbackSource` | Integers, 0–6 |
| `respectExternalControl` | Boolean |
| `inputs` | Exactly six objects, in physical alarm-channel order |

Each `inputs` entry contains `label` (1–32 bytes), `source` (1–6), `priority`
(1–6), `enabled` (boolean), and `activeHigh` (boolean). Enabled entries must use
different source numbers. Priority ties are allowed. GPIO pins are board-profile
constants and cannot be changed through JSON.

A saved configuration returns `rebootRequired` and `reauthRequired`. Network and
hostname changes are applied at restart. A changed admin password takes effect
immediately for HTTP authentication; restart also applies it to the setup AP.
Input policy/mapping changes apply after saving, with automation paused.

## Status

The status object contains:

- `csrfToken`, `uptimeMs`, `board`, `mode` (`auto` or `manual`).
- `network`: uplink address/interface/link plus setup AP address and SSID if active.
- `atem`: configured/connected flags, actual Program and Preview source IDs,
  transition/FTB feedback, product/version, transport status and error counters.
  Program/Preview are `null` without synchronized feedback. Product/version can
  remain diagnostic historical data after loss of connectivity.
- `automatic`: desired source, reason code, qualified count, policy time remaining.
  Desired source is `null` while paused.
- `inputs`: six raw electrical levels and qualified states plus their mappings,
  enabled flags, GPIO numbers and qualification timers. Input `index` is zero-based.
- `events`: bounded recent event list, newest first. Timestamps are uptime values,
  not wall-clock times. Event timestamps use the wrapping 32-bit firmware clock.
- `config`: saved configuration, `wifiPasswordSet` flag, and no password values.

The protocol version integer is ATEM's encoded protocol version, not the desktop
software release number. `lastRxMs` is a timestamp in the 32-bit firmware clock,
not a packet age; use wrapping unsigned subtraction when calculating its age.
