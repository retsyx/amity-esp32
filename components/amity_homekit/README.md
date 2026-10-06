# Optional HomeKit television

Matter commissions first and supplies Wi-Fi credentials. The authenticated
**HomeKit** web tab enables an optional, separate HomeKit association; Matter
continues exposing the Basic Video Player, Speaker and remote battery endpoints.
No additional Matter outlet endpoint is created. Pairing requires a connected
Wi-Fi station, so removing Matter fabrics later does not prevent HomeKit re-pairing.

Starting pairing generates a fresh `XXX-XX-XXX` PIN and HomeKit QR payload for a
five-minute window. Setup material appears only in the authenticated web UI while
the window is open. The PIN is held in RAM, never logged or included in the
provisioning package; only its SRP salt/verifier and setup ID are persisted in
`amity_cfg`. Closing pairing, expiration, successful pairing and reboot hide it.
Restarting pairing invalidates incomplete sessions from the preceding window.
The accessory's long-term identity and paired controllers reside in the normal
NVS partition. Removing HomeKit erases only HomeKit controllers, leaves Wi-Fi,
Matter fabrics, BLE bonds and activities intact, and keeps pairing closed.

The accessory is named **Amity**, category Television (31), with these services:

| Service | Behavior |
|---|---|
| Television | Observed activity on/off, selected activity, configurable name, navigation and transport keys |
| Television Speaker | Observed mute; relative volume up/down using HDMI-CEC |
| Input Source | One stable identifier per activity slot; saved activity names, rename and visibility support |
| Battery | Remote battery level, low-battery status and charging state |

Television Speaker and all inputs are linked to the primary Television. Sixteen
input slots keep characteristic identifiers stable across activity edits and
reboots; unused slots are unconfigured and hidden. HomeKit input identifiers are
one-based; zero means no activity. Renames and hidden-input preferences persist
while the saved activity in that slot remains the same.

On starts the first activity from standby; Off requests standby. The existing
activity settling guard applies. Reads and notifications report the observed hub
state. Unknown mute or battery readings return HomeKit's resource-absent error.
A paired but disconnected remote reports battery unavailable; an absent remote
uses 100% and not charging, matching Matter. Absolute volume is omitted because
CEC cannot reliably set it. Play/pause alternates the last HomeKit transport
request, resets on activity changes, and does not claim observed playback state.

The vendored [Espressif HAP components](../../third_party/esp-homekit-sdk/UPSTREAM.md)
use the existing Wi-Fi interface and shared mDNS responder. They neither initialize
a second Wi-Fi/BLE stack nor replace Matter's hostname. HAP is initialized on the
first pairing request and restored on boot once a verifier exists. The server
remains running with pair setup gated when an unpaired window closes. The Amity
worker, HAP event loop, HTTP requests, notifications and session cleanup serialize
access using a recursive mutex. HAP allocations and the diagnostic log buffer use
PSRAM; task stacks remain internal. Startup of a saved HomeKit configuration must
succeed before firmware boot health is confirmed.

`tests/web_homekit_ui.py` covers the tab, progress, QR visibility and removal with
a browser fixture. `tests/homekit_hardware.py` uses an isolated `aiohomekit==3.2.20`
controller to verify SRP, encrypted sessions, service links, writes, notifications,
controller removal and reboot persistence on the authorized DUT. This protocol
test does not automate Apple's Home app.
