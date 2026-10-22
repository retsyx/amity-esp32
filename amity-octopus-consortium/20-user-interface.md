# User interface: preserve the original Amity workflow

Normative interface contract dated **20 October 2026**, against baseline commit `d50af8026938797fc0820d0107f4c4e064c2d2bc`. [Original-controller policy](21-original-controller-policy.md) applies. The baseline forms/workflows remain the product contract; implementation and physical evidence are recorded separately.

**21 October 2026 navigation amendment:** The approved tab order is **Activities, Remote, Matter, HomeKit, Amity devices, Advanced**. Advanced is always last, immediately after Amity devices. This narrow ordering change supersedes the earlier no-reorganization wording; existing tab contents and workflows remain unchanged except for the separately approved per-adapter settings move.

**21 October 2026 activity editor amendment:** Show activity fields in one column, ordered **Name**, **Display**, **Source**, **Audio**, **Switch Device**, **Input**. Hide **Input** while **Switch Device** is empty, and show it when a switch device is entered. This requested layout and label change applies to the original and distributed interfaces, retaining the existing activity and switch behavior.

Activity and recommendation edit dialogs use a 480-pixel desktop width for expected activity names and device inputs. Retain the 90vw maximum width and scrollable height for smaller screens.

Tab panels have a default maximum width of 768 pixels. Header, main and footer use an 816-pixel outer width, allowing the existing 24-pixel padding on either side of the main content. Retain the existing 16-pixel header/main padding at mobile widths. Local browser checks cover all six tabs at 1280, 816, 768, 650, 390 and 320 pixels, confirming the cap, outer alignment and no horizontal overflow.

The login password panel wraps its form within a 400-pixel maximum width, centers horizontally and uses 32-pixel vertical margins. Preserve the existing internal padding and responsive width on smaller screens. Local checks at 1280, 816, 768, 650, 390 and 320 pixels confirm the panel dimensions, centering, margins and password field filling its content width; the existing login and activity browser fixture passes.

Validation for this amendment: the existing `tests/web_activities_ui.py` and `tests/web_system_ui.py` browser fixtures pass. A focused browser check at 1280- and 430-pixel widths confirms field order and vertical alignment, immediate Input visibility when setting/clearing Switch Device, and the existing switch-input value when opening an activity. Follow-up width checks cover both dialogs at 1280, 390 and 320 pixels, including long names, aligned fields and no horizontal dialog overflow. This is local browser evidence; no physical CEC or device deployment is involved.

## Exact baseline and only permitted changes

Before role enrollment, the single firmware image has no saved Wi-Fi network and hosts no UI. Complete Matter BLE enrollment to establish the original controller and its normal browser interface, or authenticated Amity BLE enrollment to add a permanently headless actuator. Reboot and removing Matter fabrics retain that chosen role. This startup eligibility overrides any earlier browser-first setup assumption; it does not add a new browser setup flow or firmware variant.

Use the existing UI at commit `d50af8026938797fc0820d0107f4c4e064c2d2bc` as the activity/device/remote/integration/administration contract. Preserve its HTML/CSS interaction structure, forms, labels, buttons and ordinary workflows. The permitted visible differences, including the per-adapter settings amendment, are:

1. Device listings include the owning Amity device's canonical hostname and HDMI-adapter context so identically named equipment can be distinguished. The underlying selector remains the full scoped OSD name; friendly context is never parsed back into identity.
2. Add exactly one **Amity devices** tab listing the main and added devices. It contains supported Add, status, per-device Settings, remove, replace and update operations. It does not replace the original tabs; place it immediately before **Advanced**, which remains last.
3. Move the former global Advanced default front physical address into each named device's Settings dialog, independently for its stable HDMI-adapter slots. Remove that former global form/API; other Advanced workflows retain their baseline locations.

Keep the original tab contents and use this exact navigation order: **Activities**, **Remote**, **Matter**, **HomeKit**, **Amity devices**, **Advanced**. Do not introduce Equipment/Settings navigation, a guided activity wizard, screen-first question sequence, batch-activity review, new remote owner/button mapping UI or a primary-zone selector. No custom phone application is required. Normal user vocabulary remains Amity device, HDMI adapter and Amity system; octopus/tentacle/consortium stay internal developer terms.

## Activities and device choices

Retain Scan HDMI, the original discovered-device/recommendation area, activity list and reorder behavior, Add activity, Save activities and Discard changes. Show the activity dialog fields in one column: **Name**, **Display**, **Source**, **Audio**, **Switch Device**, and **Input** 0–255, with **Input** visible only when **Switch Device** is nonempty and the original Start/OK/Cancel controls. Keep the receiver/audio and switch behavior represented by those fields. The backend adapter translates the existing workflow into scoped schema2 records and guarded planner operations; it must not remove AVR/switch support merely because the direct-route wizard was removed.

Device entries use the owning Amity's actual canonical hostname as location context, followed by the stable adapter number where needed. UI ordering, duplicate names and current logical addresses cannot silently select a different attachment. Save stable ComponentId/ActivityId/scoped selectors internally. Rename/reorder and ordinary unique OSD-name LA churn retain identity; same-scope ambiguity remains unavailable. Hardware roles/physical paths must still be validated through existing saved configuration or explicit management/connection evidence. Matching an old form does not authorize guessed topology, raw CEC forwarding or an automatic wake.

Saving/editing/scanning/recommendations do not start activities. The existing explicit Start/selection flow uses ordinary planner admission, exact generation/deadline checks and visible errors. Restart keeps logical requests suspended; no input or activity is replayed automatically. Resource conflicts and unavailable equipment must not be hidden as success, but they do not justify a replacement activity-creation UX.

## Remote and integrations

Preserve the original Remote tab's Pair/Unpair/status workflow and original button behavior. The sole remote pairs only to the original first-setup controller and may select activities/screens throughout the system. Added devices have no remote owner/mapping UI or input-gateway permission. Preserve the Matter and HomeKit tabs, existing setup windows, controller/fabric management and original integration identities. The internal primary projection stays compatible; do not add a new primary-zone choice to the baseline UI.

The original remote behavior explicitly includes Siri swipes: gesture recognition, thresholds, timing, generated press/release sequences and cancellation stay unchanged. The consortium layer delivers those existing events to their owning output; no new gesture configuration or interpretation is introduced. [Algorithm-preservation rule](21-original-controller-policy.md#original-algorithms-and-functionality-amendment--21-october-2026).

The original controller hosts every tab and browser/admin session. Added devices host no UI, login, recovery/status page or home-control integration. Their management goes through the one extra Amity tab and authenticated peer channels. Controller absence makes user control unavailable; no member offers an alternative UI.

## Additional Amity tab

The device list always includes the original main first, including a system with no added devices. Use the heading `<hostname> (this device)`, for example **amity-dut (this device)**, with the marker inline rather than on a separate status line. Align **Settings** to the right in the same title row. Added-device title rows likewise place the hostname on the left and **Settings** plus **Remove**, or **Replace** when applicable, on the right; **Update firmware** may remain below. This row identifies the controller serving the interface; it does not offer member Remove, Replace or Update firmware actions. Main firmware updates remain in the original Advanced workflow. There is no separate Location action or editable cosmetic device alias. Main hostname changes use the existing Advanced setting. Do not precede the list with a paragraph describing only added devices as manageable.

Device names equal actual hostnames. Add requires **Name**: 1–32 letters, digits or hyphens, without a leading/trailing hyphen, normalized to lowercase after trimming surrounding whitespace. Internal spaces or punctuation are rejected rather than slugified. Reject missing, invalid or already assigned names before starting radio enrollment; apply the accepted name to the candidate's actual persistent hostname through authenticated bootstrap profile 2. Device rows and equipment choices use authenticated/assigned hostname metadata rather than old location labels; an unknown legacy name displays **Name unavailable**. Keep stable adapter numbers. Show unknown, unavailable, pending and retired states accurately; absent CEC replies are not physical-presence proof. Add uses the candidate's explicit physical/setup-code authorization. Changing a network hostname does not change the stable device ID, authority or scoped equipment selectors; it is an operational network change, not a cosmetic presentation edit. This amendment adds no member-rename workflow. Existing actual hostnames and equipment configuration stay unchanged; do not convert old Location aliases into new network names during upgrade.

Removal previews actual dependent activities and revokes the selected actuator. Explicit **Remove Amity** factory-resets a responding added device using its authenticated reset journal and target-only restart. If it is unresponsive, drop it locally and explain that the user must hold its physical reset button for at least ten seconds. Keep saved equipment/activity references unavailable for repair. A lost reply or failed native drain/persistence is unconfirmed, never proof of completed erasure; do not automatically repeat the request. The main has no Remove action. Generic retirement, authority loss and enrollment cancellation do not reset anything. [Removal lifecycle](13-faults-removal-and-recovery.md#intentional-removal).

Actuator replacement requires old-hardware isolation/revocation, fresh device identity, and confirmed attachment/equipment mappings, preserving valid logical activity IDs. It cannot promote a member, move the remote, clone credentials or reopen a member UI. Updates use exact-release compatibility and report new boot/image/config health separately. Retain original Advanced diagnostics, backup/restore, restart, hostname, password and reset functions in their baseline locations; the default front physical address moves only as specified below; security validation remains required behind the unchanged workflow.

If the main fails, support user-managed replacement through the existing **Advanced → Upload/Restore** control on freshly Matter-BLE-enrolled replacement hardware. Confirm that the old main is offline/isolated; restore portable activity/device IDs, names/order and valid saved choices without executing activity. The replacement establishes new main authority, the sole remote/home integrations are paired again, and each old headless member receives the existing ≥10-second physical factory reset and is re-added through its 3-to-below-10-second enrollment window, retaining its origin/Add code but using fresh operational keys and no old roster trust. No new wizard, automatic failover, planned transfer or credential cloning is added. [Manual recovery](21-original-controller-policy.md#manual-main-replacement-from-a-configuration-backup).

## Per-device HDMI-adapter settings amendment — 20 October 2026

Each named device row, including the main, offers **Settings** in its title row. Read settings through the original main; added devices do not gain an HTTP server or UI. Opening or refreshing the dialog only reads metadata: no Scan HDMI, CEC probe, wake, route, identity claim, persistence or restart. Unknown legacy names, offline owners and unsupported firmware are shown honestly and cannot be edited.

Show slots in stable connector order within the hardware profile’s advertised capacity: **HDMI adapter 1** on fixed one-adapter hardware, or **HDMI adapter 1** through **HDMI adapter 5** on five-adapter hardware (internal TentacleIds 0–4). Zero, one, several or five connectors may be configured/populated; holes never compress the numbering. A fixed legacy connector may be present by its explicit board construction. A development connector is **present** only with qualified independent presence evidence, **absent** with explicit negative evidence, otherwise **unknown**. CEC silence, a floating input, a pull-up on a removable module and saved equipment records alone are not presence proof. Do not add an internal CEC pull-down or detection pulse. A separate defined-state GPIO detector must be explicitly configured and electrically qualified; [runtime contract](04-hardware-and-cec-runtime.md).

Show each slot's saved **Default front physical address**, **Detected front physical address** and **Selected front physical address** separately. Detected is a fresh usable same-connector immediate-peer report. Selected uses that report first, otherwise that connector's saved default as a startup/identity hint. A fallback by itself never confirms topology, owns a proxy identity or grants routing/playback. Explicit absence or an unreadable configured detector clears detected/selected runtime values; the saved default remains. With no detector, physical presence stays unknown even if native CEC observations exist. Settings follow the connector, not a removable physical module moved to another connector.

Only a configured, positively present adapter on the fresh named owner is editable. Absent, unknown, unconfigured, offline and unsupported slots retain read-only defaults with their state explained. A blank value resets that slot to **1.0.0.0**; a nonblank value must be a valid dotted hierarchical CEC address. Do not infer a sibling slot's setting or address. The old single fallback migrates only to adapter 1; other slots start independently at 1.0.0.0.

**Save and restart Amity** explicitly confirms restart of the selected device only. Submit exact owner BootId, settings revision and each changed slot's presence generation. A busy owner, stale boot/revision, expired authority or detector removal/reinsertion rejects before writing. Reserve the owner, close positive admission, cancel discovery, drain current wire work and matching cleanup, then recheck physical generations immediately before the bounded atomic write. A started flash write obtains its actual result even if it crosses the original request deadline. On durable success keep old authority closed, attempt the applied response, and schedule the selected device's restart even if response delivery fails. Admission alone never permits restart. If the browser loses the result, show an unknown outcome and require explicit fresh read/reconciliation; do not resubmit automatically. Failure before persistence restores eligibility only for fresh requests. Restart does not resume an activity or held input.

| Case | Required result |
|---|---|
| Five-adapter profile with no configured connectors | Five stable read-only rows; original administration remains healthy. |
| One fixed legacy connector | Only adapter 1 is listed, present by explicit construction and editable; unsupported slots 2–5 are not invented. |
| Sparse connectors, e.g. 1 and 5 | Rows and saved values retain indices 1 and 5; no renumbering or sibling fallback. |
| Detector absent or unavailable | Presence unknown; no edit and no claimed physical absence from CEC silence. |
| Explicit removal/reinsertion while dialog is open | Presence generation advances; old Save rejects despite returning to present; reopening captures a fresh generation. |
| Native PA report on adapter 5 | Only adapter 5's detected/selected values change; its saved default stays unchanged. |
| Activity, held input, update or pending configuration | Busy response, no settings write/restart; no interruption of another device. |
| Stale/revoked queued request | No persistence callback after cancellation, no restart, no automatic replay. |
| Flash completion crosses deadline | Return its actual durable outcome; never strand a successful write as a definitive failed save. |
| Applied response lost | Selected device still restarts after the transmission attempt; client reports unknown, never automatic retry. |

The exact [HTTP contract](12-control-surfaces-and-admin.md#per-device-settings-contract) and [storage/provenance rules](11-config-storage-and-migration.md#per-adapter-default-front-address-preferences) apply. Physical detector/electrical, multi-device and recovery qualification remain separate from browser and host tests.

## Amendment acceptance matrix

| ID | Required evidence |
|---|---|
| UX-01 | Fresh boot exposes no browser UI or Wi-Fi setup page. After Matter BLE enrollment establishes the original role, the original login and tab workflows remain, with exactly this order: Activities, Remote, Matter, HomeKit, Amity devices, Advanced. Advanced is last. Member BLE enrollment remains headless on the same firmware. |
| UX-02 | Activity creation/edit fields, Start/OK/Cancel and Save/Discard match the baseline. No screen-first wizard. |
| UX-03 | AVR/audio and optional switch/input settings remain expressible and keep legacy semantics. |
| UX-04 | Discovered-device lists retain baseline behavior plus canonical Amity-hostname/adapter context; stale cosmetic board labels never override actual names. |
| UX-05 | Identical device names on distinct scopes select the intended attachment without an LA pin. |
| UX-06 | Rename/reorder preserve stable activities/slots; no hidden topology or authority mutation. |
| UX-07 | Original Remote pairing and button workflow controls the sole original-controller remote. |
| UX-08 | Original Matter/HomeKit tabs, identities and published controls persist without new zone/remote choices. |
| UX-09 | Save, scan and device-list refresh do not wake/route; explicit Start remains separate. |
| UX-10 | Unique same-name LA churn resolves automatically; ambiguous/missing equipment stays unavailable. |
| UX-11 | One remote and original-controller browser can select activities on different screens subject to resources. |
| UX-12 | Amity devices always lists the main first with the inline heading `<hostname> (this device)`, even without members. Settings is right-aligned in the title row; added-device Settings and Remove/Replace share that row, while Update firmware may remain below. Member Remove/Replace/Update firmware controls never appear on the main's row; Add requires a valid, unique Name that sets the actual hostname; no separate Location alias exists, and actuator status/remove/replace/update remain explicit. |
| UX-13 | Members have no UI/integrations/remote permission; foreign remote/promotion requests are rejected. |
| UX-14 | Original Advanced backup, restore, logs, restart and remaining settings retain their baseline workflow; only the former global default front address moves to the per-device Settings dialog. Fresh replacement-main recovery uses existing Upload/Restore with old-main isolation, preserved portable logical IDs/names/order, fresh authority and explicit re-pairing/member re-enrollment; no separate wizard. |
| UX-15 | Reboot/reconnect/stale input cannot replay activity or retarget an exact held gesture. |
| UX-17 | Per-device Settings meets the zero-to-five/sparse/presence-generation matrix above; reads have no CEC side effects and only an applied durable save restarts the selected owner after response attempt. |
| UX-16 | Desktop/mobile, keyboard/focus/error behavior and actual baseline comparison are recorded; historical guided-wizard checks are not substitute evidence. |

Scoped identities, non-actuating configuration, explicit intent, stable references and bounded recovery remain backend requirements behind the unchanged interface. Validate baseline parity and [SC-01–SC-08](21-original-controller-policy.md); record physical remote/integration and observed-user results separately.
