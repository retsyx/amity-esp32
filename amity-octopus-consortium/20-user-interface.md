# User interface: preserve the original Amity workflow

Normative interface contract dated **20 October 2026**, against workflow baseline commit `d50af8026938797fc0820d0107f4c4e064c2d2bc`. The **21 October 2026 visual refinement amendment** below uses approved main commit `aa9395aaecdc861e1d4cce040079a5ac6121e67d` as its presentation baseline. [Original-controller policy](21-original-controller-policy.md) applies. The baseline forms/workflows remain the product contract; implementation and physical evidence are recorded separately.

**29 October 2026 explicit-removal amendment:** Remove lists every saved activity using the selected Amity through source, display, audio or switch and warns that confirmation deletes those activities. It deletes the owner, equipment, settings and saved name; no removed-device card or equipment placeholder remains. Review fences include current configuration/presentation and membership revisions, target boot and exact affected activity IDs. Changed data requires a fresh review. Replace stays available before Remove and preserves/remaps activity definitions while revoking and forgetting the isolated old Amity. Remove the controller/capacity sentence from device cards and the remote-ownership sentence from Replace. [Removal and replacement](13-faults-removal-and-recovery.md).

**21 October 2026 navigation amendment:** The approved tab order is **Activities, Remote, Matter, HomeKit, Amity devices, Advanced**. Advanced is always last, immediately after Amity devices. This narrow ordering change supersedes the earlier no-reorganization wording; existing tab contents and workflows remain unchanged except for the separately approved per-adapter settings move.

**21 October 2026 activity editor amendment:** Show activity fields in one column, ordered **Name**, **Display**, **Source**, **Audio**, **Switch Device**, **Input**. Hide **Input** while **Switch Device** is empty, and show it when a switch device is entered. This requested layout and label change applies to the original and distributed interfaces, retaining the existing activity and switch behavior.

**21 October 2026 visual refinement amendment:** Retain the sage background and dark green palette while clarifying action hierarchy: activity Start/Repair uses the primary treatment, Edit is quiet, and Delete uses restrained red text. The active activity has a faint sage background and its existing badge. Use a compact, horizontally scrollable tab strip with a selected underline and `aria-current`, retaining all six tabs in their approved order. Tighten the header and collapse the empty message area. Present the existing Firmware, Wi-Fi and Remote diagnostics as aligned label/value rows, and space Download logs and Restart Amity apart. This is a presentation change; labels, control order, workflows and guarded operations remain unchanged.

Activity and recommendation edit dialogs use a 380-pixel desktop width, with a maximum width and height leaving 16-pixel viewport margins and scrollable content on smaller screens. Reduce gaps between fields, align actions and retain a compact numeric switch input. Advanced file, hostname and password fields use widths suited to their expected values, and file-picker buttons share the existing palette. Per-adapter default front physical address fields use a compact monospace width for `X.X.X.X`, retaining their existing validation and seven-character limit.

Tab panels have a default maximum width of 768 pixels. Header, main and footer use an 816-pixel outer width, allowing the existing 24-pixel padding on either side of the main content. Retain the existing 16-pixel header/main padding at mobile widths. Local browser checks cover all six tabs at 1280, 816, 768, 650, 390 and 320 pixels, confirming the cap, outer alignment and no horizontal overflow.

The login password panel wraps its form within a 400-pixel maximum width, centers horizontally and uses 16-pixel vertical margins. Use the shared section padding and responsive width on smaller screens, with the password field filling the panel content width. On narrow phones, compact Amity device-card padding and action spacing leave room for the hostname while retaining Settings and Remove in the same title row and 44-pixel button targets.

Validation for the earlier compact-layout amendment: the existing `tests/web_activities_ui.py`, `tests/web_system_ui.py` and `tests/web_device_settings_ui.py` browser fixtures pass. A focused browser check at 1280- and 430-pixel widths confirms field order and vertical alignment, immediate Input visibility when setting/clearing Switch Device, and the existing switch-input value when opening an activity. Follow-up compact-width checks at 1280, 390 and 320 pixels cover both activity dialogs, Advanced inputs and per-adapter address fields, including maximum-length editing, seven visible address characters, aligned fields, long action labels and no horizontal overflow. This is local browser evidence; no physical CEC or device deployment is involved.

Validation for the visual refinement: the same three browser fixtures pass, including scoped activity operations, device management and adapter settings. Local layout checks at 1280, 390 and 320 pixels cover all six tabs, activity actions and editors, the three diagnostics rows, device cards and sparse configured-adapter states without horizontal overflow or browser errors.

**27 October 2026 qualified-lookup amendment:** Preserve the original activity handling behind these forms. Saved choices identify Amity device ID / HDMI adapter / CEC side / OSD name. Schema 3 has no persistent activity enabled flag or route approval. Missing equipment stays selected and uses ordinary runtime lookup; AVR audio and explicit switch inputs remain usable through their original local algorithms. No Enable/Repair-path/Approve-topology workflow is added. [Addressing](03-domain-model-and-addressing.md) and [execution](07-activity-planning-and-execution.md) supersede former planner approval requirements.

**29 October 2026 scan/cache amendment:** Saving activities keeps HDMI device caches and remote observations available without disconnecting Amities or starting a scan. Device enumeration occurs only at boot, on explicit **Scan HDMI**, or for actual communication with an uncached recipient. Lookup scans have an independent 120-second cooldown per owner/adapter/side; boot/UI scans bypass eligibility and refresh their timestamps. Reconnects and periodic timers do not independently scan. [Discovery](05-discovery-and-topology.md) defines the detailed rule; no new UI workflow is added.

**28 October 2026 equipment-label amendment:** Discovered devices and activity equipment choices show **Adapter 1**, **Adapter 2**, etc., only for owners compiled to support multiple adapters, with stable connector numbering. Hide the adapter label for single-adapter builds. Use the owner's current compiled capacity when available, falling back to retained capacity while offline; the number of configured or populated adapters does not determine visibility. A multi-adapter build with only adapter 1 populated still shows **Adapter 1**. Omit screen/source connection descriptions. With no added Amity, omit the main's hostname from its equipment labels; a lone single-adapter Amity therefore shows only the equipment name. Once another Amity is enrolled through **Amity devices → Add**, prefix equipment locations with the owning hostname, including while an added device is offline. Activity selections do not determine hostname visibility. An offline owner's saved equipment selection keeps its owner name until explicit removal or replacement. These labels are presentation only; saved owner/adapter/CEC-side/OSD selectors remain exact, including unchanged selections whose shorter labels coincide.

Local validation covers lone-main and multiple-owner labels in `tests/activity_editor_test.sh` and `tests/web_system_ui.py`, including single/multiple-adapter capacity, current compiled capacity overriding a larger retained value, stable adapter numbering and retained removed-owner selections. `tests/activity_choice_identity_ui_test.js` verifies that equal labels retain exact saved selectors and concurrent hidden changes require a reload. The browser fixture passes 26 checks with no script errors; screenshots cover the label forms. The firmware builds successfully. These are host and browser checks, without a DUT deployment.

## Exact baseline and only permitted changes

Before role enrollment, the single firmware image has no saved Wi-Fi network and hosts no UI. At boot, a zero-fabric board automatically opens its bounded Matter setup window when it is unclaimed or the proven original main with retained authority, including after factory reset and still-unclaimed reboots. No setup button is required. Complete Matter BLE enrollment to establish the original controller and its normal browser interface, or authenticated Amity BLE enrollment to add a permanently headless actuator. Reboot and removing Matter fabrics retain that chosen role. Headless/prepared/retired/recovery roles do not automatically open Matter setup. A proven main whose last fabric was removed reopens setup at its next boot while keeping main authority/UI and explicit Matter-tab pairing controls; saved activities and commissioning history do not gate this. This startup eligibility overrides any earlier browser-first setup assumption; it does not add a new browser setup flow or firmware variant.

Use the existing UI at commit `d50af8026938797fc0820d0107f4c4e064c2d2bc` as the activity/device/remote/integration/administration workflow contract, with the approved visual refinement at `aa9395aaecdc861e1d4cce040079a5ac6121e67d` as the shared presentation baseline. Preserve forms, labels, buttons, control order and ordinary workflows. The permitted visible differences, including the per-adapter settings amendment, are:

1. Device listings include the owning Amity device's canonical hostname and HDMI-adapter context so identically named equipment can be distinguished. The underlying selector remains the full scoped OSD name; friendly context is never parsed back into identity.
2. Add exactly one **Amity devices** tab listing the main and added devices. It contains supported Add, status, per-device Settings, remove, replace and update operations. It does not replace the original tabs; place it immediately before **Advanced**, which remains last.
3. Move the former global Advanced default front physical address into each named device's Settings dialog, independently for its stable HDMI-adapter slots. Remove that former global form/API; other Advanced workflows retain their baseline locations.
4. Apply the approved visual refinement to the shared interface while retaining device-specific controls, attachment context and the per-adapter settings layout. The HTML baseline guard compares against a digest of the approved main presentation after removing only the additional Amity devices tab and the relocated global address setting, without requiring the separate main commit in a consort-only checkout.

Keep the original tab contents and use this exact navigation order: **Activities**, **Remote**, **Matter**, **HomeKit**, **Amity devices**, **Advanced**. Do not introduce Equipment/Settings navigation, a guided activity wizard, screen-first question sequence, batch-activity review, new remote owner/button mapping UI or a primary-zone selector. No custom phone application is required. Normal user vocabulary remains Amity device, HDMI adapter and Amity system; octopus/tentacle/consortium stay internal developer terms.

## Activities and device choices

Retain Scan HDMI, the original discovered-device/recommendation area, activity list and reorder behavior, Add activity, Save activities and Discard changes. Show the activity dialog fields in one column: **Name**, **Display**, **Source**, **Audio**, **Switch Device**, and **Input** 0–255, with **Input** visible only when **Switch Device** is nonempty and the original Start/OK/Cancel controls. Keep the receiver/audio and switch behavior represented by those fields. The backend saves schema-3 user intent with qualified selectors and delivers original operations to the selected owner. It preserves AVR/audio/switch handling without a separate executable-route model.

Device entries use the owning Amity's actual canonical hostname as location context, followed by the stable adapter number where needed. UI ordering, duplicate names and current logical addresses cannot silently select a different attachment. Save stable ComponentId/ActivityId/scoped selectors internally. Rename/reorder and ordinary OSD-name LA churn retain identity. Keep the original owner-local name/PA replacement and lookup behavior without adding an ambiguity, freshness or complete-scan approval gate. The selected owner resolves its adapter, side and OSD name using the original local discovery behavior. Missing equipment does not erase or disable its saved selection.

Preserve the baseline effects of configuring, scanning, recommending and explicitly selecting activities. Start uses the original activity algorithm with ordinary lookup and current authenticated input context. No durable current-activity/resume journal, global resource reservation or prepared-plan barrier is added. Report actual admission/failure outcomes; do not hide an architectural restriction behind a request to repair an existing activity.

## Remote and integrations

Preserve the original Remote tab's Pair/Unpair/status workflow and original button behavior. The sole remote pairs only to the original first-setup controller and may select activities/screens throughout the system. Added devices have no remote owner/mapping UI or input-gateway permission. Preserve the Matter and HomeKit tabs, existing setup windows, controller/fabric management and original integration identities. The internal primary projection stays compatible; do not add a new primary-zone choice to the baseline UI.

The original remote behavior explicitly includes Siri swipes: gesture recognition, thresholds, timing, generated press/release sequences and cancellation stay unchanged. The consortium layer delivers those existing events to their owning output; no new gesture configuration or interpretation is introduced. [Algorithm-preservation rule](21-original-controller-policy.md#original-algorithms-and-functionality-amendment--21-october-2026).

The original controller hosts every tab and browser/admin session. Added devices host no UI, login, recovery/status page or home-control integration. Their management goes through the one extra Amity tab and authenticated peer channels. Controller absence makes user control unavailable; no member offers an alternative UI.

The authenticated `POST /api/key` accepts `{"key":65,"duration_ms":250}`
with an integer CEC user-control code from 0 to 255 and an optional integer
hold duration from 0 to 5000 milliseconds. Zero, also the default when
`duration_ms` is omitted, sends one complete press/release tap. A positive
duration begins the existing held-key/repetition lifecycle, waits in the
administration task, then releases that exact key before writing the HTTP
response. Disconnecting the caller does not skip release. The owner task keeps
advancing RX, activity setup, discovery and CEC completions throughout that
bounded wait. Other HTTP administration requests wait behind this request for
the requested hold plus ordinary task/cleanup admission time. The duration
sets when firmware requests release; it does not promise an exact physical CEC
completion deadline. No timer, retry or outgoing CEC queue is added.

Requests use the same `SystemControl` remote facade and original owner-local
lookup/transmission path. The current activity's audio device receives Volume
Up (`0x41`), Volume Down (`0x42`) and Mute (`0x43`), and its source receives
Select (`0x00`), Up/Down/Left/Right (`0x01`–`0x04`), Play (`0x44`), Stop (`0x45`)
and Pause (`0x46`). Play/Pause (`0x61`) retains the original toggle translation.
Power (`0x40`) selects system Standby through the original accepted-selection
path for both zero and positive durations, retaining its selection receipt and
input gate. The same request on the main controls an
activity whose equipment belongs to an added Amity; members expose no
additional control endpoint. Startup/selection input handling and activity
setup suppression remain in their existing layers. A positive-duration request
received during setup is discarded immediately without scheduling a later up.
The main checks the captured activity generation, accepted-selection receipt
and existing local input lifetime before forwarding either edge. A selection,
repair, configuration change or input reset prevents an old request from
releasing a newly pressed key. These local timer checks add no wire token.
Conditional cleanup uses the existing
bounded cleanup IPC reservation; it never falls back to an unfenced raw-key up.
Failed keys retain their
existing suppression until this request's final release, and cleanup does not
release an independently held different key. Unknown or duplicate fields,
noninteger values and keys/durations outside the supported ranges are rejected.
`{"ok":true}` acknowledges task admission (including final release admission
for a positive duration); fresh CEC completion and resulting equipment state
must be checked separately. This is ordinary product control reused by the
functional framework without a test-specific firmware surface.

## Additional Amity tab

The device list always includes the original main first, including a system with no added devices. Use the heading `<hostname> (this device)`, for example **amity-dut (this device)**, with the marker inline rather than on a separate status line. Align **Settings** to the right in the same title row. Added-device title rows likewise place the hostname on the left and **Settings** plus **Remove**, or **Replace** when applicable, on the right; **Update firmware** may remain below. This row identifies the controller serving the interface; it does not offer member Remove, Replace or Update firmware actions. Main firmware updates remain in the original Advanced workflow. There is no separate Location action or editable cosmetic device alias. Main hostname changes use the existing Advanced setting. Do not precede the list with a paragraph describing only added devices as manageable.

**Add** scans directly from the main for nearby unclaimed boards. Prompt the user to hold and release the candidate’s **FW RESET** after three seconds and before ten seconds; choose the candidate if several appear, enter its name, and add it. The normal build asks for no code. A build requiring code possession asks for the candidate’s existing printed Matter manual code or QR payload; it does not open full Matter commissioning or change role through Matter. Main and candidate must use the same admission profile, with no fallback. No separate Add code or member relay is introduced.

Device names equal actual hostnames. Add requires **Name**: 1–32 letters, digits or hyphens, without a leading/trailing hyphen, normalized to lowercase after trimming surrounding whitespace. Internal spaces or punctuation are rejected rather than slugified. Reject missing, invalid or already assigned names before committing admission or transferring network/membership records; scanning does not require a name. Apply the accepted name to the selected candidate’s actual persistent hostname through the encrypted admission session. Device rows and equipment choices use authenticated/assigned hostname metadata rather than old location labels; an unknown legacy name displays **Name unavailable**. Keep stable adapter numbers. Show unknown, unavailable, pending and retired states accurately; CEC replies and command ACKs determine whether communication succeeds. Add uses the candidate’s physical window and, when configured on both ends, its printed Matter code. Changing a network hostname does not change the stable device ID, authority or scoped equipment selectors; it is an operational network change, not a cosmetic presentation edit. This amendment adds no member-rename workflow. Existing actual hostnames and equipment configuration stay unchanged; do not convert old Location aliases into new network names during upgrade.

Removal previews actual dependent activities and revokes the selected actuator. Explicit **Remove Amity** factory-resets a responding added device using its authenticated reset journal and target-only restart. If it is unresponsive, drop it locally and explain that the user must hold its physical reset button for at least ten seconds. Keep saved equipment/activity selectors unchanged; current lookup reports unavailable while that owner is absent. A lost reply or failed native drain/persistence is unconfirmed, never proof of completed erasure; do not automatically repeat the request. The main has no Remove action. Generic retirement, authority loss and enrollment cancellation do not reset anything. [Removal lifecycle](13-faults-removal-and-recovery.md#intentional-removal).

Actuator replacement requires old-hardware isolation/revocation, fresh device identity and explicit selector-owner mappings, preserving valid logical activity IDs. It does not require a newly approved physical-route graph. It cannot promote a member, move the remote, clone credentials or reopen a member UI. Updates use exact-release compatibility and report new boot/image/config health separately. Retain original Advanced diagnostics, backup/restore, restart, hostname, password and reset functions in their baseline locations; the default front physical address moves only as specified below; security validation remains required behind the unchanged workflow.

If the main fails, support user-managed replacement through the existing **Advanced → Upload/Restore** control on freshly Matter-BLE-enrolled replacement hardware. Confirm that the old main is offline/isolated; restore portable activity/device IDs, names/order and valid saved choices without executing activity. The replacement establishes new main authority, the sole remote/home integrations are paired again, and each old headless member receives the existing ≥10-second physical factory reset and is re-added through its 3-to-below-10-second enrollment window, retaining its origin identity and factory Matter/admin credentials but using fresh operational keys and no old roster trust. No new wizard, automatic failover, planned transfer or credential cloning is added. [Manual recovery](21-original-controller-policy.md#manual-main-replacement-from-a-configuration-backup).

After an Advanced hostname change, the page periodically checks the new
address and opens it automatically once the same Amity has restarted and is
ready. Keep a link available for browsers that require certificate approval
at the new address; the user need not guess when to refresh. Preserve the
existing warning for unsaved activity edits.

## Per-device HDMI-adapter settings amendment — 20 October 2026

Each named device row, including the main, offers **Settings** in its title row. Read settings through the original main; added devices do not gain an HTTP server or UI. Opening or refreshing the dialog only reads metadata: no Scan HDMI, CEC probe, wake, route, identity claim, persistence or restart. Unknown legacy names, offline owners and unsupported firmware are shown honestly and cannot be edited.

Show slots in stable connector order within the hardware profile's advertised capacity: **1** on fixed one-adapter hardware, or **1** through **4** on four-adapter hardware (internal TentacleIds 0–3). Zero, one, several or four connectors may be configured; holes never compress the numbering. There are no separate adapter-presence inputs or attachment generations. A configured CEC wire remains usable, with actual CEC replies and command ACKs determining communication results. Do not add detection pulses or pull-downs to a live CEC line; [runtime contract](04-hardware-and-cec-runtime.md).

Use one table with the columns **Adapter**, **Default**, **Detected Address**
and **Selected Address**. Adapter cells contain stable numbers 1–4 within the
hardware capacity; Default cells contain the editable text fields. Avoid
repeating “front” or presenting each address as a separate panel. Do not show
“Configured HDMI adapters can be edited.” Detected follows the original
same-adapter back-device physical-address algorithm. At startup Selected is the
lowest usable detected address, otherwise that adapter's saved default.
Settings follow the connector, not a removable module moved to another one.

A configured adapter on the fresh named owner is editable, including a quiet wire with no responding CEC device. Unconfigured slots and offline/unsupported owners retain read-only defaults with their state explained. A blank value resets that slot to **1.0.0.0**; a nonblank value must be a valid dotted hierarchical CEC address. Do not infer a sibling slot's setting or address. Each slot starts independently at 1.0.0.0.

**Save** applies the changed defaults without rebooting. This 2 November 2026
amendment replaces the earlier settings restart workflow. Submit exact owner
BootId, settings revision and each changed configured slot. A busy owner, stale
boot/revision, expired authority or unconfigured slot rejects before writing.
Reserve the owner, drain current wire work and matching cleanup, then recheck
configured slots, boot, revision and authority immediately before the bounded
atomic write. A started flash write obtains its actual result even if it
crosses the original request deadline. After durable success, change a selected
address only if its actual startup selection used the default because no usable
address was found; retain a scanned-selected address even when it numerically
equals the previous default. Keep that source as transient per-adapter state,
without turning later observations into a new selection decision. Publish the
applied snapshot, resume normal admission and retain boot, connections and
caches. Do not scan or replay an activity. The dialog stays open and shows the
saved, detected and selected values. An unknown outcome requires an explicit
fresh read/reconciliation; do not resubmit automatically.

| Case | Required result |
|---|---|
| Four-adapter profile with no configured connectors | Four stable read-only rows; original administration remains healthy. |
| One fixed legacy connector | Only configured adapter 1 is listed and editable; unsupported slots 2–4 are not invented. |
| Sparse connectors, e.g. 1 and 4 | Rows and saved values retain indices 1 and 4; no renumbering or sibling fallback. |
| Configured wire with no CEC replies | Default address is editable; ordinary command attempts report their actual ACK/result. |
| Unconfigured slot | Read-only; no adapter or detector is invented. |
| Default edit on a fallback-selected adapter | Only that adapter's selected address adopts its new default; boot and caches remain intact. |
| Default edit on a scanned-selected adapter | Save the new fallback and retain the selected scanned address, including equal-number cases. |
| Later observation after fallback selection | Retain fallback provenance; the next explicit default edit still updates the selected address. |
| Native PA report on adapter 4 after startup | Only adapter 4's detected value changes; no sibling setting or selected provenance is overwritten. |
| Activity, held input, update or pending configuration | Busy response, no settings write; no interruption of another device. |
| Stale/revoked queued request | No persistence callback after cancellation and no automatic replay. |
| Flash completion crosses deadline | Return its actual durable outcome; never strand a successful write as a definitive failed save. |
| Applied response lost | Keep the live applied setting; client reports unknown and requires Refresh, never automatic retry. |

The exact [HTTP contract](12-control-surfaces-and-admin.md#per-device-settings-contract) and [storage/provenance rules](11-configuration-storage.md#per-adapter-default-front-address-preferences) apply. Electrical, multi-device and recovery qualification remain separate from browser and host tests.

## Amendment acceptance matrix

| ID | Required evidence |
|---|---|
| UX-01 | Eligible unclaimed or proven-original-main zero-fabric boot automatically opens bounded Matter setup without requiring a button or an empty activity configuration, including after factory reset; this permanent feature must remain in the actual boot path. Existing fabrics and headless/prepared/retired/recovery roles do not open it automatically. Last-fabric removal keeps main authority and permits automatic opening at its next boot. Fresh boot exposes no browser UI or Wi-Fi setup page. After Matter BLE enrollment establishes the original role, the original login and tab workflows remain, with exactly this order: Activities, Remote, Matter, HomeKit, Amity devices, Advanced. Advanced is last. Member BLE enrollment remains headless on the same firmware. |
| UX-02 | Activity creation/edit fields, Start/OK/Cancel and Save/Discard match the baseline. No screen-first wizard. |
| UX-03 | AVR/audio and optional switch/input settings remain expressible and keep legacy semantics. |
| UX-04 | Discovered-device lists show Adapter N only for multi-adapter builds, without connection-side descriptions; owner hostname appears when added Amities need distinguishing. Stale cosmetic board labels never override actual names. |
| UX-05 | Identical device names on distinct scopes select the intended attachment without an LA pin. |
| UX-06 | Rename/reorder preserve stable activities/slots; no hidden topology or authority mutation. |
| UX-07 | Original Remote pairing and button workflow controls the sole original-controller remote. |
| UX-08 | Original Matter/HomeKit tabs, identities and published controls persist without new zone/remote choices. |
| UX-09 | Configure, refresh and explicit Start preserve original local effects and ordinary lookup, subject to the 29 October scan/cache amendment. Activity saves do not disconnect devices or discard HDMI caches; explicit Scan HDMI bypasses lookup cooldown. No persistent activity approval is added. |
| UX-10 | Same-name LA churn and name/PA replacement follow original local lookup; missing equipment stays selected without persistent availability state. |
| UX-11 | One remote and original-controller browser can select activities on different screens through original activity handling. |
| UX-12 | Amity devices always lists the main first with the inline heading `<hostname> (this device)`, even without members. Settings is right-aligned in the title row; added-device Settings and Remove/Replace share that row, while Update firmware may remain below. Member Remove/Replace/Update firmware controls never appear on the main's row; Add requires a valid, unique Name that sets the actual hostname; no separate Location alias exists, and actuator status/remove/replace/update remain explicit. |
| UX-13 | Members have no UI/integrations/remote permission; foreign remote/promotion requests are rejected. |
| UX-14 | Original Advanced backup, restore, logs, restart and remaining settings retain their baseline workflow; only the former global default front address moves to the per-device Settings dialog. Fresh replacement-main recovery uses existing Upload/Restore with old-main isolation, preserved portable logical IDs/names/order, fresh authority and explicit re-pairing/member re-enrollment; no separate wizard. |
| UX-15 | Reboot/reconnect/stale input cannot replay activity or end a newer hold. Key operations use the current activity target through the normal resolver. |
| UX-17 | Per-device Settings meets the zero-to-four/sparse/configured-slot matrix above; reads have no CEC side effects and an applied durable save updates only default-selected addresses without reboot, cache loss or connection replacement. |
| UX-16 | Desktop/mobile, keyboard/focus/error behavior and actual baseline comparison are recorded; historical guided-wizard checks are not substitute evidence. |

Scoped identities, non-actuating configuration, explicit intent, stable references and bounded recovery remain backend requirements behind the unchanged interface. Validate baseline parity and [SC-01–SC-08](21-original-controller-policy.md); record physical remote/integration and observed-user results separately.
