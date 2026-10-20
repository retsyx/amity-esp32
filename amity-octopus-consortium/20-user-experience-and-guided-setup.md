# 20 — User experience and guided setup

**Status:** proposed v0.1, 20 October 2026. **Audience:** agents implementing the product, interaction design, administration APIs and acceptance tests. This document specifies the intended experience; it is not a claim that the current firmware implements it.

**Ownership:** this is the normative owner of normal user journeys, vocabulary, equipment presentation, question order and usability acceptance. [Administration](12-control-surfaces-and-admin.md) owns adapters and HTTP contracts; [configuration](11-config-storage-and-migration.md) owns persistence and activation; [enrollment](09-membership-security-and-enrollment.md) and [BLE coexistence](10-ble-and-matter-coexistence.md) own security and radio state. Their invariants continue to apply.

## 1. Outcome: configure what the user wants to do

The user should be able to say **“Watch Apple TV on the projector”** and have Amity fill in the equipment, connection and control details it can establish. Adding another Amity should extend the existing system without requiring another independent configuration project.

The ordinary user must not need to understand CEC logical addresses, physical addresses, proxy identities, electrical domains, coordinator roles or qualified-name syntax. The system still needs those concepts internally. Simplification comes from maintaining an accurate model, making useful suggestions, and asking small, concrete questions when evidence is missing.

The normal sequence is:

1. Add the Amity and optionally give it a familiar location name.
2. Show the equipment it can identify, with useful context.
3. Ask which screen the user wants to use, unless already known.
4. Suggest source activities and a supported sound path for that screen.
5. Resolve only the missing connections needed for the selected activities.
6. Save, then offer an explicit **Try activity** action.

Do not require a complete house-wide cable diagram before the first activity works. Do not turn every discovered attribute into a confirmation question. A single supported choice is a default; multiple meaningful choices require a selection; uncertainty that affects correct control requires a focused question. An optional customization must not become a prerequisite.

Normal use requires neither a custom phone application nor a display generated on the HDMI screen. The existing Matter home application can handle first-board Wi-Fi commissioning; the Amity administration page handles system setup. A phone browser must have the same essential path as a desktop browser, including a manual setup-code alternative.

## 2. Names people see and names Amity stores

### 2.1 Use equipment titles with location context

Use a two-line equipment choice rather than presenting a filesystem-like address:

> **Apple TV**  
> Projector

In a mixed equipment picker, another source can appear as:

> **Apple TV**  
> Equipment rack

The first line answers **“Which piece of equipment?”** The second answers **“Which Amity is it associated with?”** It uses the octopus's user-visible location label. If two choices still look identical, add a connector detail such as **Projector · Amity connector 2**. Distinguish that board connector from **Projector HDMI 2**, an input on the actual projector. Use the hardware profile's real connector markings, with the word Amity when needed to avoid confusion. In a list already grouped under the heading **Projector**, omit the repeated location on every row.

For the user's proposed setup, a board called **Projector** could therefore show:

| Context | Primary text | Secondary text |
|---|---|---|
| Source picker spanning the system | Apple TV | Projector |
| Two Apple TVs on different tentacles of that board | Apple TV | Projector · Amity connector 1 / Projector · Amity connector 3, one per row |
| Screen picker, equipment reported as `Projector` | Projector | Display, if repeating the same location would add nothing |
| Equipment group headed Projector | Projector; Apple TV | Type or status only when useful |
| Advanced connection details | Exact reported OSD name | Complete attachment and diagnostic addresses |

If the user prefers **Display** as the projector's visible title, they may set that label; the source remains **Apple TV**. Neither label changes the equipment's reported OSD name. Do not generate `Projector/Projector` merely to show all address fields. When ambiguity requires context, retain enough visible context to distinguish the choices even if some text repeats.

Location is not a routing assertion. A board named Projector may control equipment on several cables, a receiver, or a device at the far end of a cable. Likewise, Apple TV associated with Equipment rack may be usable on both the TV and projector. Reachability must come from the confirmed HDMI model, never from the board label or string matching.

### 2.2 Defaults make naming optional

An octopus receives a stable generated label when added, such as **Amity 2**, plus its printed short hardware identifier in identification details. Offer an optional field labeled **Where is this Amity?** with examples such as Projector, TV cabinet and Equipment rack. If addition began while configuring a known screen, offer that screen's name as an editable suggestion. Do not infer a physical location merely because a screen is visible on CEC.

Let the user enter this label while enrollment proceeds. Do not make a separate mandatory naming page. Retain a generated label until it is edited; do not renumber boards when one is removed. Reused location labels are permitted, with a connector or short hardware identifier added when needed for distinction. Never silently rename another board to make the latest entry unique.

An equipment title defaults to the actual reported OSD name. A user override is optional. A missing name can use a clearly provisional role label such as **Unidentified display**, but that placeholder cannot be saved as though it were a reported OSD selector. Activities receive suggested titles from their source and screen. Within the Projector view, **Apple TV** is sufficient; in an all-activities view, render **Apple TV on Projector**. If those titles collide, automatically add source context such as **Source: Equipment rack** or **Source: Projector · Amity connector 2** beneath them. This distinguishes activities without requiring the user to rename anything. Explicit user titles survive later discovery and refresh.

### 2.3 Presentation is separate from device selection

The configured fully qualified name remains:

`{ consortium_id, octopus_id, tentacle_id, side, osd_name }`

The final element is the equipment's OSD name. Its current CEC logical address is resolved at runtime and is not part of the persisted selection. An activity references a stable component record containing its qualified OSD selectors. [Domain and address contract](03-domain-model-and-addressing.md), [name resolution](05-discovery-and-topology.md).

The octopus label belongs to OctopusId, and the equipment label belongs to ComponentId. Editing either is a presentation change. It must not rename a hostname, change the HTTPS origin or certificate, rewrite a selector, republish an external accessory, alter activity identity, or interrupt current control. The displayed two-line title is never parsed to reconstruct an address.

Show one equipment card per configured ComponentId. If the same physical receiver has several verified native attachments, show **Connections** within that card. Amity-created proxies do not appear as extra source choices. Equal OSD names alone do not prove two observations describe the same physical appliance.

Two distinct devices with the same OSD name on the same matching scope remain an identity ambiguity. Giving them different friendly labels does not fix it. The UI must offer a durable resolution—change a supported device-reported name, place them on distinct attachment scopes, or use an explicitly qualified matching rule. It must not offer “pick logical address 4” as a permanent fix. See [discovery](05-discovery-and-topology.md).

## 3. One system page with three ordinary destinations

The initial navigation is **Activities**, **Equipment**, and **Settings**.

**Activities** is the everyday destination. Group activities by screen when more than one screen exists. Show what is selected, whether the relevant equipment is reachable, and any conflict requiring a decision. Avoid exposing individual Amity boards as independent theaters by default.

**Equipment** shows screens, sources, receivers, and the Amity location context used to identify them. An **Amity boards** section supports Add, Identify, Rename, Connections, Replace, and Remove. A board detail shows the actual supported connector count and hardware markings. It must not show five active connectors on a single-tentacle board.

**Settings** contains remotes, home integrations, network, updates, backup, and advanced diagnostics. Rare technical details stay available there; they are not required fields in ordinary activity creation.

When setup is incomplete, use a contextual card such as **Finish setting up Projector**. Let the user continue using other working activities. The presence of an unfinished source or one unavailable board must not turn the whole system into an endless setup wizard.

## 4. First installation and existing installations

### 4.1 First new Amity

The user follows the hardware profile's printed **Set up your first Amity** instruction, adds it through their existing Matter home application to supply network access, and opens its local setup page. The UI establishes administration access once and uses the already initialized durable singleton system. Opening or refreshing the page must not create a second system. It does not ask whether the user wants an octopus or a consortium, which board should be coordinator, or which CEC controller owns an input.

For supported new button profiles, [document 10](10-ble-and-matter-coexistence.md) proposes a released short press before three seconds to request first-board Matter setup only while fresh and eligible. Longer holds select other operations; a short-press operation must not start while the user is still holding toward a longer gesture. Exact gestures, printed instructions, state exclusions, indicators and boundary tests are part of the hardware profile. This is a required implementation, not an existing universal hardware feature. Buttonless legacy hardware retains its explicit USB-assisted preparation path.

The product must supply a stable, discoverable local setup URL and explain how the supported browser reaches the authenticated page. The current local-certificate trust issue is still a product gate: a QR code or mDNS name does not by itself establish browser trust. Do not describe a certificate-warning bypass as an invisibly solved security mechanism. [Administration baseline and trust](12-control-surfaces-and-admin.md), [decision register](17-decisions-and-open-questions.md).

Once the page is open, show detected equipment immediately and continue with the screen/activity journey below. Keep the location field optional. Offer remote pairing and additional home integration after a browser-controlled activity is configured; neither is required to prove the HDMI setup.

### 4.2 An existing single-board Amity

Preserve activities, user labels, remote mappings, external selectors and existing integration identities during migration wherever the old configuration is representable. Resolve its existing OSD-name choices into the newly scoped records without asking the user to recreate them. Show a focused repair only for an actually ambiguous or unresolved choice.

If the existing board needs a one-time consortium preparation step, explain that step only when **Add Amity** is chosen. The legacy bootstrap and physical-presence rules remain those in [enrollment](09-membership-security-and-enrollment.md). Do not make a routine firmware update silently create a new setup secret or treat an existing operational installation as an unclaimed device.

The existing board remains the system's first location, with an optional rename. Existing users should not encounter a fresh-installation wizard merely because the internal model now supports more boards. [Migration contract](11-config-storage-and-migration.md).

## 5. Adding another octopus

Use **Add Amity** as the ordinary action; advanced documentation may call it **Add octopus**. Run this flow in the existing system page.

| Stage | What the user does or sees | What Amity handles |
|---|---|---|
| Identify the new board | Scan its **Add to Amity** code, import a photo of that code, or enter the grouped setup code. | Select the candidate by authenticated setup identity; do not make the user choose a MAC address from a scan list. |
| Open setup | Follow the new board's printed physical setup gesture. | Respect the bounded enrollment window and shared BLE workflow arbiter. |
| Connect | Optional location label; named progress such as **Connecting to your system**. | Choose a healthy eligible nearby helper, authenticate enrollment, use already available supported system Wi-Fi credentials when needed, and verify the new board on the LAN. |
| Add | **Projector added** after membership activation. | Resume or finish the durable enrollment transaction and synchronize policy. |
| Find equipment | **Finding equipment near Projector**, with results as they arrive. | Start bounded identity discovery; retain partial results and separate equipment readiness from membership success. |
| Continue | **Set up activities** or **Done for now**. | Keep an unfinished equipment setup task without undoing the successful membership addition. |

The existing page is the authority for this flow. The user must not commission every member into Matter, log into every board separately, re-enter Wi-Fi that the supported enrollment path already possesses, choose a coordinator, or choose a BLE relay. A board that is already on a suitable network retains it; moving an existing board to another network is an explicit network change, not a side effect of group addition.

The Matter home-application code and the Add to Amity code must have clearly different labels and instructions. If the wrong kind is entered, say **“This code is for your home app. Use the Add to Amity code on this board.”** Never log either secret or put it in a URL, browser history, durable setup draft, or general-purpose browser storage. Preserve the secure enrollment journal rather than retaining the user's raw code to retry later. [Enrollment protocol](09-membership-security-and-enrollment.md).

When no helper can reach the candidate, explain one concrete next step: **“Bring the new Amity near Equipment rack to finish adding it. You can install it by the projector afterward.”** Signal strength may help choose a transport helper; it must not establish ownership or authentication. If the radio is busy with another setup operation, show that operation and offer to wait. Offer Cancel only through that operation's supported cancellation path; do not preempt an active Matter PASE/fail-safe or bypass the radio arbiter. Do not tell the user to factory-reset the system as a general retry mechanism.

A label is cosmetic even when entered during joining. Enrollment success must not depend on whether it has been entered. If joining is interrupted after keys are prepared, recover the existing transaction after reconnect/login where the protocol permits; do not create a second board entry or demand the setup code for a completed step. Equipment questions can resume independently afterward.

## 6. Discovering and identifying equipment

### 6.1 Show useful results without demanding a full scan

Report progress by activity: **Looking for equipment**, **Apple TV found**, **Checking connections**, or **One connection needs your help**. Discovery is bounded and incremental. A known source should not vanish from the page while a different connector times out.

Use separate states for **Configured**, **Not responding**, **Not identified yet**, **Connection needs confirmation**, and **Ready for this activity**. Do not turn every discovery result green merely because the Amity joined Wi-Fi. A source can be identified while its route, proxy capacity, or display control is still unresolved.

Passive observation and bounded identity/status queries may run automatically. They must not send power, standby, input-change, active-source or experimental routing commands. An unanswered query does not prove a cable is missing, a device is off, or HDMI Control is disabled. Preserve configured equipment and present the least disruptive next check. [Scanning contract](05-discovery-and-topology.md).

CEC visibility does not reveal every cable endpoint. Several peers visible through a receiver are not all directly plugged into the observed connector. A reported device type can suggest **Display**; it cannot reliably tell the UI which physical screen the user calls Projector. The current hardware also cannot be assumed to read EDID or inspect the picture. These limits must affect question selection and status text, not become a technical lecture in the normal flow.

### 6.2 An identification ladder with bounded effects

Offer **Help me identify this** next to ambiguous equipment or connection choices. Use the least disruptive supported method:

1. **Show the associated Amity.** Display its location, printed short identifier and board-profile image. On a profile with a qualified visual indicator, offer a bounded **Identify Amity** flash. This must not actuate HDMI equipment. If the profile lacks an indicator, do not show a fake blink action.
2. **Show the connector.** Highlight the actual numbered connector and the markings the user can find on the hardware. In explanatory text, use phrases such as **From the source** and **To the display or receiver**; keep raw `front/back` terminology in connection details for technical users. The profile's real labels take precedence over generic artwork.
3. **Ask a concrete cable question.** For example, **“Where does the cable leaving Amity connector 2 go?”** Offer the known equipment and printed input/output names, plus **I'm not sure**. Reuse that answer for every later activity that uses the same connection.
4. **Offer a deliberate check if still needed.** Explain the equipment that may turn on or change input. An Amity-issued diagnostic operation requires already committed, valid bindings/capabilities and normal planner admission. For an unresolved draft candidate, use physical inspection or guide the user to select an input with the equipment's own remote until those prerequisites exist. Record what was actually observed or confirmed. Do not hide a power or source change inside Identify or Scan, and do not treat a saved draft as permission to test it.

The normal identification path must not require unplugging everything, repeatedly power-cycling a projector, interpreting raw CEC traffic, or seeing an Amity-generated overlay on the TV. HDMI image generation and video capture are not part of this product model.

### 6.3 Ask only what affects the current goal

Choose questions from unresolved prerequisites of the requested activity. Good questions name visible equipment and cable markings:

- **“Which screen is connected to this receiver output: TV or Projector?”**
- **“Which HDMI input on Projector receives the cable from the receiver?”**
- **“Are these two connections on the same physical receiver?”**

Avoid asking **“What is the physical address?”**, **“Which bus owns this proxy?”**, or **“Draw your topology.”** If a guided manual input selection produces useful CEC evidence, consume it, but do not assume every device reports the same event. A human-confirmed connection remains human-confirmed evidence.

Ask about one shared receiver only when that identity is needed and cannot be established already. Matching OSD names are a suggestion to investigate, not a merge rule. Preserve one receiver's shared input and power resources once its identity is confirmed. [Topology and resource model](03-domain-model-and-addressing.md).

Provide **I'm not sure** and **Set up later** without discarding the rest of the draft. Explain the consequence narrowly: **“Apple TV on Projector needs this connection before it can run.”** A technical advanced editor may help an expert, but it must not be the only way to finish an otherwise supported installation.

### 6.4 Help with placement before demanding connection answers

If the hardware is not wired yet, offer **Help me connect it** with the selected equipment and an image of that board's actual numbered connectors. Explain one cable connection at a time. Preserve the tentative activity goal while the user connects equipment, then fill the identity fields from discovery. A manually entered equipment description remains a placeholder until the required identity and capabilities are established.

Explain source protection with a concrete placement instruction: **“To prevent Game console from changing the receiver's input, its source cable needs an Amity connection before it reaches Receiver.”** A tentacle on a receiver output alone cannot intercept all sources before the receiver reacts. Reuse available tentacles and show the supported board capacity; do not suggest that adding a software activity creates another physical pass-through or transports video between boards.

Equipment control and protection against source interruptions have separate readiness. If a source can be controlled but its traffic is not intercepted at the necessary boundary, show that specific limitation. Do not claim the whole system is protected merely because every display has an Amity nearby. Wiring suggestions must use the physical/source-isolation rules in [octopus architecture](01-octopus-architecture.md) and [CEC policy](06-cec-roles-and-message-policy.md).

## 7. Creating an activity

### 7.1 Start with the screen

If there is one usable screen, select it automatically. If the user begins from the Projector screen, retain that context. Otherwise ask **“Where do you want to watch?”** and show screen names with enough location context to identify them.

Then show sources that the physical HDMI model can deliver to that screen. Group fully supported suggestions separately from **Needs a connection check**. Do not silently omit a likely source when one answer could establish its route; show the missing requirement. Conversely, do not offer a TV-only direct source as a working projector activity just because the consortium can send it CEC commands.

For a selected screen with several reachable sources, offer a suggested activity list with clear choices, such as **Apple TV** and **Game console**, so the user can add several in one review. If only one source is suitable, preselect it and let the user change it. Avoid a repeated form asking for display, source, switch and audio by raw name for every activity.

### 7.2 Default the sound path when it is known

Select the sole verified audio path automatically and summarize it as **Sound: Receiver** or **Sound: Projector speakers**. If there are genuinely supported alternatives, let the user choose. Carry the established preference to later activities only when their actual routes support it.

Do not infer receiver audio for a source plugged directly into a screen without a supported return-audio route. ARC/eARC and built-in TV applications remain outside the initial qualified scope. A nice default must not conceal an unsupported path. [Scope](02-consortium-architecture.md), [activity planning](07-activity-planning-and-execution.md).

### 7.3 Resolve the remaining prerequisites, then review once

The planner validates display input, receiver input/output capabilities, source isolation, domain identity capacity and required control bindings. It returns a small set of actionable blockers. The UI asks the concrete questions from section 6, retaining answers already collected.

Two HDMI outputs do not prove that a receiver can select two independent inputs, enable both outputs, or expose CEC control on each. A silent receiver output can still be part of a video path if that path and the necessary control bindings are independently established. Do not require a CEC response from every physical socket; do require evidence for the controls the plan needs.

For an unsupported receiver output mode or missing driver, say what is missing and keep the activity as an unfinished draft, or offer a supported route. This version does not introduce a generic “manual step” execution engine. Do not mark an activity fully automatic merely because the user can operate the receiver manually. A separately specified and qualified fixed-state assumption or control driver is required before such a route can be advertised as supported.

Review should read like this:

| Item | Choice |
|---|---|
| Activity | Apple TV on Projector |
| Picture | Apple TV → Receiver → Projector |
| Sound | Receiver |
| Remote | Projector remote, if one is already assigned |

The **Change** action beside a choice opens just that choice. Do not force the user through every completed page again. The activity title is suggested and editable; no typing is required for an ordinary source-and-screen activity.

### 7.4 Save and try are distinct

**Save activity** validates and activates configuration. It does not turn equipment on or start playback. After success, offer **Try activity** with a concise effects description, for example **“Turn on Projector and Receiver, then select Apple TV.”** That action submits a separate normal planner intent. Clear action text is enough; do not add a second generic “Are you sure?” prompt.

If another session makes the action disruptive, show the specific conflict before proceeding: **“The receiver is playing Game console on TV. Switch it to Apple TV for Projector?”** Offer a choice that explicitly changes the conflicting session, or keeps current playback. Revalidate at execution time because another remote may have changed the state since the page was drawn.

A CEC acknowledgement is not proof that the user sees a picture or hears sound. When a functional test is needed, ask **“Can you see Apple TV on Projector?”** and, if audio is relevant, **“Can you hear sound through Receiver?”** Record human confirmation separately from protocol observations. **Not now** leaves the activity configured but not user-tested; it does not invent a failed test or a success.

If the test fails, retain the activity and show a short diagnosis tied to the failed stage: display not responding, input connection unconfirmed, output mode unavailable, or sound path not established. Ask the next useful check instead of reopening the entire setup process. Repeating a test is explicit; reconnecting the browser must not repeat a previous power or routing command.

## 8. Worked TV, projector and receiver example

Suppose an existing Amity called **TV cabinet** controls a TV and a direct-connected console. The user adds **Projector** near a projector with its own direct Apple TV, and **Equipment rack** with a receiver and another Apple TV. The receiver has cables to both displays. Actual capabilities are still subject to qualification.

The equipment page can show the two Apple TVs with identical titles and different location subtitles. A verified shared receiver has one card with its TV and Projector connections inside it. The projector and TV are separate screen choices, not separate copies of the same receiver.

To create the first projector activity:

1. If setup began from the Projector screen, or confirmed topology identifies it as the sole applicable screen, suggest **Set up Projector** without asking for the same screen again. Naming the board Projector alone is insufficient; otherwise ask the screen question once.
2. The source list shows **Apple TV — Projector** for the direct source and **Apple TV — Equipment rack** for the receiver source, if its route is confirmed or can be resolved. It explains **“Through Receiver”** only where that helps distinguish the route.
3. For the direct source, select projector speakers only if they are an established usable audio path. Do not assume sound can travel back to the receiver.
4. For the rack source, reuse the receiver identity, input and audio answers already collected. Ask which receiver output/cable leads to Projector only if that connection is still unknown.
5. Suggest **Apple TV on Projector**, save, and offer the explicit test. If both Apple TVs become activities on that screen, automatically show their source-location subtitles so they remain distinct. A custom title is optional.

When later creating a TV activity for the rack Apple TV, reuse the source/receiver information and ask only about the TV branch if still unresolved. The console attached directly to TV does not become a projector source without an actual video path.

If both screens request the same receiver source, permit it only when simultaneous output, control and resource compatibility are established. If they request different sources from a receiver with one selector, explain the conflict in terms of those two activities. Turning off Projector must preserve a receiver still needed by TV. The user should never have to discover a shared-resource race by trying to operate two independent receiver entries. [Worked physical topology](02-consortium-architecture.md), [resource arbitration](07-activity-planning-and-execution.md).

## 9. Remotes and home integrations

After a browser-controlled activity is usable, offer **Add a remote**. The normal flow identifies the supported remote, uses an eligible nearby Amity as its owner and asks **“Which screen should this remote control?”** only if there is a real choice. Reuse the screen already being configured as the default. Do not make the remote's radio owner determine its screen; a remote near TV cabinet may control Projector through the consortium.

Version 1 supports one managed BLE remote per board. If the chosen owner already has one, explain the limit and offer a suitable owner or the explicit move/re-pair flow. Do not silently replace a bond or choose a new owner on every signal-strength change. Pairing waits for the shared BLE workflow to be available. [Remote contract](12-control-surfaces-and-admin.md).

The initial Matter/HomeKit adapters publish one primary zone. Preserve that selection for migrated installations. For a new multi-screen setup, ask once **“Which screen should appear in your home app?”** and explain that the other screen remains available in Amity and through its assigned remote. Do not commission every satellite or republish accessories after renaming a board. Changing the primary external zone is an explicit integration change with its own effect preview, not a hidden consequence of adding a projector.

## 10. Changes, interruption and recovery

### 10.1 Routine changes

| Event | Expected user experience |
|---|---|
| User renames an Amity or equipment label | The new presentation appears everywhere; activities, browser origin and current control keep their identities. |
| A logical address changes | No setup prompt. Amity resolves the unchanged qualified OSD selector and updates fenced runtime bindings automatically. |
| Equipment stops responding | Retain its card and activity choices. Show Not responding and a targeted check; do not remove or replace it automatically. |
| The equipment's actual OSD name changes | Offer one focused identity repair when necessary. Update the selector only with explicit confirmation or independently sufficient recorded evidence; a remembered LA is insufficient. |
| A cable moves to another connector | Detect changed attachment evidence, suspend affected control when required, and ask the one remapping question needed. Preserve unrelated choices and activity identities. |
| A new source appears | Show it in Equipment and suggest adding activities for supported screens. Do not change the currently playing activity. |
| A scan discovers a same-scope name ambiguity | Explain the ambiguity, preserve the draft, and require a durable resolution. Never pick the first observation or silently rebind an activity. |

### 10.2 Setup survives navigation and interruptions

Retain a single bounded nonsecret setup draft on the coordinator, as specified in [configuration storage](11-config-storage-and-migration.md). It records selected components, answered connection questions, incomplete prerequisites and the revisions/evidence used. Refresh, login expiry, closing the page, or moving to a supported second browser should recover the unfinished task after authentication.

If discovery changes, invalidate only decisions that depend on the changed evidence. A newly ambiguous Apple TV choice should not discard a confirmed receiver-to-projector cable answer. If another administrator edits the draft, report a conflict and offer the current saved choices; do not overwrite them silently. Present who/where changed it only if that information is actually available.

Enrollment progress comes from its security journal; execution progress comes from the planner/session state. A draft is not an alternative authority for either. After a reboot or uncertain execution, show **Resume activity** or **Try again** when appropriate under the recovery contract. Do not replay a saved click, a key press, or a setup test automatically.

### 10.3 Removal and replacement

**Remove Amity** shows the named equipment, activities, remote and integration roles that will be affected, then requests one explicit removal decision. An unplugged board is simply offline until deliberately removed. Keep unaffected activities usable. If the board is the current coordinator or integration gateway, guide the supported transfer/recovery branch before removal; do not imply every member is interchangeable without preparation.

Removing an inline board from the consortium does not physically reconnect its isolated CEC wires. If the user also wants it out of the HDMI path, show the specific cable reconnection instructions appropriate to that board. Do not claim ordinary HDMI Control behavior has been restored merely because software membership was removed.

**Replace Amity** starts from the old location so the user does not re-enter every activity. Enroll the new hardware with its own credentials, suggest mappings from old connectors to new confirmed equipment, and request confirmation only for uncertain mappings. Preserve logical component/activity references where the mapping is established. Do not clone operational keys, Matter/HomeKit identities or BLE bonds. Explain required remote/home-integration re-pairing before the user commits a replacement that changes those roles. [Lifecycle contract](13-faults-removal-and-recovery.md).

### 10.4 Updates, backups and network changes

Offer system updates as one managed task with a brief effect preview for activities that may pause. Preserve labels, selectors, drafts and compatible configuration across supported updates. A firmware restart does not mean “first setup” or permission to reopen Matter commissioning. Required activity resume remains explicit. [OTA](14-ota-and-compatibility.md).

Backup/restore should describe what is retained: equipment configuration, activities and labels. Do not claim a portable configuration backup restores security identities or home pairings. Restore into different hardware uses the same guided mapping as replacement. A network change is a dedicated task with success/rollback status; it must not emerge as a surprise side effect of adding a board. [Storage and export](11-config-storage-and-migration.md), [network ownership](10-ble-and-matter-coexistence.md).

## 11. Progress, errors and accessibility

Prefer short statements naming the affected equipment and the next action. Keep technical codes available through **Details** for support, without making them the only explanation.

| Situation | Useful text or action |
|---|---|
| New member is enrolled but HDMI incomplete | **Projector added. One connection needs your help.** |
| Expected source is absent from current replies | **Apple TV is not responding. Check that it has power, then check again.** Do not say it was removed. |
| Missing cable mapping | **Which HDMI input on Projector receives this cable?** |
| Unknown AVR output behavior | **Amity has not established how to select this receiver output. This activity needs a supported output configuration.** |
| Existing activity conflict | **Changing Receiver to Apple TV will stop Game console on TV.** |
| Same-scope identity ambiguity | **Two devices report Apple TV on this connection. Amity needs a way to tell them apart before controlling either.** |
| Board out of range during enrollment | **Move the new Amity near Equipment rack to finish adding it.** |
| Coordinator unavailable | **System control is unavailable. Check Equipment rack's power and network.** Only identify that board if the stored role mapping is known. |

Do not show invented percentages for an unknown scan duration, claim a picture is working from a CEC ACK, or show a generic **Success** for a partial step. Distinguish **Added**, **Configured**, **Command sent**, **Observed**, and **Confirmed by you** where the distinction changes what the user should do next.

All setup paths must work on a narrow phone screen, without hover, drag-only graph editing or color-only status. Use visible text labels, logical keyboard/focus order, accessible progress announcements and touch-sized controls. Long or identical OSD names need a complete accessible label and expandable text; ellipsis must not hide the only distinguishing connector. QR scanning is optional; manual code entry and image import remain available. Preserve progress when the user zooms, rotates the screen, navigates back, or temporarily leaves to use the home application.

## 12. Implementation contracts for agents

### 12.1 Presentation and choice models

Build view models from authoritative snapshots rather than assembling device identity from rendered text. Required conceptual fields are:

| Record | Minimum information and semantics |
|---|---|
| Amity summary | OctopusId, stable default label, optional explicit label, printed short identifier, supported connector/indicator profile, membership/network/equipment readiness. |
| Equipment choice | ComponentId for configured equipment, or an opaque discovery-candidate reference tied to an observation snapshot; title, context, optional connector detail, kind, readiness, reasons, and connection references. |
| Configured selection | The component's persisted QualifiedDeviceName selectors, verified alias relationships and relevant capabilities; no LA selection key. |
| Suggested activity | Proposed source, screen, sound and route by stable references; retained defaults; unresolved prerequisites; current capability/resource constraints; proposed title. |
| Setup question | Stable question identifier, affected goal, choices referenced by stable IDs or scoped candidate refs, evidence provenance, dependencies, and a reason the answer is needed. |
| Setup draft | One bounded server record with revision, completed choices, relevant config/presentation revisions, snapshot references and outstanding decisions. No raw setup code, Wi-Fi password, private key or bearer token. |
| Test result | Requested ActivityId/session, execution outcome, observations, optional user confirmation, time/revision scope, and any unresolved check. A successful test is evidence for that route/state, not every possible route. |

These are responsibilities, not a new wire schema. Implement them through the bounded administration APIs in [document 12](12-control-surfaces-and-admin.md), with storage and comparison rules from [document 11](11-config-storage-and-migration.md). Draft and staged candidate data share the existing pending-configuration budget. Do not add an unbounded wizard history or shadow configuration database.

### 12.2 Question selection and revalidation

Question selection is a deterministic dependency walk over the chosen goal:

1. Reuse accepted configuration and current discovery evidence.
2. Determine the requested screen, source and supported route; apply a sole valid choice automatically.
3. Collect unresolved prerequisites that actually block that goal.
4. Prefer one concrete question whose answer resolves several prerequisites, such as identifying a shared receiver or a cable endpoint.
5. Present the next useful question, with its visible consequence and a defer option.
6. Revalidate the selected candidates, route and capabilities on save; revalidate live conflicts again on Try.

Do not require an LLM or cloud service for this workflow. Do not ask the same question once per activity if the answer describes a reusable component or cable. A confirmed answer is not permanent proof against physical change: recabling and contradictory new evidence can make it stale. Preserve unrelated answers when this happens.

Snapshot-scoped discovery selections are optimistic. If another native candidate creates ambiguity before save, reject that selection with a repairable reason; do not silently substitute it. Current logical addresses may be displayed in advanced diagnostics but cannot be persisted as a workaround. A completed label edit must not restart discovery, enrollment or active equipment control.

### 12.3 No hidden actuation

The following actions have no power/standby/routing effect: opening a page, scanning identities, editing a label, viewing or saving a setup draft, validating a candidate, saving an activity, resuming a setup page, and adding a board. Identity discovery may send its permitted bounded queries. Policy changes may cancel unsafe stale work or release old holds under the established activation contract; they do not authorize a new activity.

**Try activity**, ordinary activity selection, explicit resume, and deliberately described diagnostic checks use the normal planner. All conflict previews, supported-capability checks, fresh authority and execution fences apply. A lost browser response does not authorize replay of an uncertain action. Record desired, applied, observed and user-confirmed state separately.

### 12.4 Product gates before claiming the simple experience

The interface depends on functioning enrollment, scoped discovery, conflict planning, bounded draft persistence and the exact supported hardware profile. In addition, the following must be resolved and demonstrated:

- First-board physical setup entry and printed instructions, including buttonless legacy preparation and release/hold boundary behavior.
- A usable first-browser entry/trust path on the supported phone and desktop browsers; local HTTPS identity and camera permissions cannot be waved away.
- Recognizable board and connector markings, with a qualified Identify indicator only where hardware supports it.
- Per-model HDMI routing/audio/output capabilities and adequate proxy capacity for the advertised activities.
- Full-load memory/storage capacity for the proposed setup views and draft alongside Matter, BLE, HTTPS and consortium control.

If a gate is not closed, narrow the supported product claim and show the actual limitation. A polished screen cannot substitute for an unresolved hardware or security dependency. [Engineering gates](17-decisions-and-open-questions.md), [release evidence](15-validation-and-release-gates.md).

## 13. Acceptance scenarios

These are future release requirements. Passing document/schema checks does not satisfy them. Record the hardware, firmware, browser, topology, participants, observations and failure cases in the implementation's evidence. [Validation ownership](15-validation-and-release-gates.md).

| ID | Scenario and required result |
|---|---|
| UX-01 | **First installation:** on each supported fresh hardware profile, a novice follows the printed setup action, reaches the supported Matter/network and browser path, identifies equipment and configures one activity. No custom app, HDMI overlay, CEC address entry or coordinator selection is required. Verify physical-entry state exclusions and browser-trust instructions. |
| UX-02 | **Existing installation:** migrate an eligible single-board system while retaining its activities, labels, external selectors and pairings. Ask only about genuinely unresolved identity/topology or required legacy consortium preparation; do not repeat first setup. |
| UX-03 | **Additional board:** from the existing page, add with code plus physical action; automatically select a helper and use available supported network credentials as appropriate. Optional naming does not block membership. Test photo/manual alternatives, busy radio, out-of-range guidance and no satellite Matter/login requirement. |
| UX-04 | **Names and qualified selectors:** select Apple TV using its title/location, with connector detail only when needed. Distinguish Amity connectors from appliance inputs and retain source context when activity titles collide. Rename board and equipment labels; verify unchanged OSD selectors, ComponentIds, activities, HTTPS origin, external identities and current control. Test repeated names, slashes and long names. |
| UX-05 | **Duplicates and proxies:** two Apple TVs on different scopes remain distinguishable; a verified shared receiver has one card and multiple Connections; proxies are absent from source choices. Two distinct same-scope native name matches block control until durable resolution. Cosmetic names and remembered LAs never bypass it. |
| UX-06 | **Discovery and identification:** partial results remain useful; missing replies do not erase equipment; physical device and connector identification work on supported indicator and indicatorless profiles. Discovery/Identify-Amity cause no HDMI power or routing changes. Ask only concrete cable/equipment questions needed by the goal; wiring guidance locates source protection before the shared switching boundary and does not claim output-only placement protects every source. |
| UX-07 | **Screen-first activities:** default the sole screen/source/audio path, retain screen context, and reuse existing connection answers. Filter by actual video paths, explain likely-but-unresolved sources, and reject unsupported audio/output assumptions. One review can create several supported source activities. |
| UX-08 | **Shared receiver and two screens:** exercise same-source output sharing where qualified, different-source conflict on one selector, CEC-silent output with independent valid control, and stopping one activity while another needs shared equipment. Explain the affected activities before a disruptive change. |
| UX-09 | **Save and Try:** scan, label edits, draft updates, config save and browser resume cause no new activity or hidden power/routing command. Explicit Try uses activated config and current planner admission. ACK alone never marks picture/audio confirmed; Skip test retains an honest untested state. No uncertain test is replayed automatically. |
| UX-10 | **Address churn and repair:** move a known OSD selector to another LA with no user prompt or activity edit; reject stale work. Exercise OSD rename, recabling and late same-scope ambiguity with a focused repair that retains unrelated choices. |
| UX-11 | **Interrupted setup:** refresh, expire login, reconnect and reboot at supported enrollment/draft stages. Resume completed choices and journaled membership without duplicate members or persisted raw secrets. Handle concurrent edits and stale evidence explicitly; no saved click is replayed. |
| UX-12 | **Offline, removal and replacement:** retain offline devices; preview the actual effects of removal; keep unaffected activities. Replace from the old location, confirm only uncertain mappings, preserve allowed logical identities, and explain physical cable work plus any required re-pairing. |
| UX-13 | **Remote and home integration:** pair only through admitted BLE workflow, default the known screen, and support a remote owner away from its target screen. Enforce one remote per board and one primary external zone; preserve migrated integration identity and make role changes explicit. |
| UX-14 | **Accessible phone setup:** complete the essential paths on a narrow screen by touch and keyboard/assistive navigation without hover, color-only meaning or a drag-only graph. Long/duplicate labels remain distinguishable. Code entry works without camera access and progress survives navigation. |
| UX-15 | **Capacity and unsupported cases:** reject unsupported node/port/draft/config limits before losing saved data. Show qualified connector counts, indicator availability and control capabilities accurately. Unsupported identity, AVR output, browser or hardware assumptions never produce a false Ready/automatic-control claim. |
| UX-16 | **Observed novice usability:** run the journeys below with representative new users on real hardware. Record required decisions, typed fields, help requests, repeated questions, wrong equipment selections, completion and recovery. Any unexplained address entry, repeated known question, mandatory optional label or avoidable complete restart fails the simplification requirement. |

For UX-16, distinguish a **decision** from a physical/code/authentication action. The initial target budgets for an otherwise supported and unambiguous installation are:

| Journey | Required setup decisions beyond physical actions/authentication | Optional choices |
|---|---|---|
| Add a satellite to an existing reachable system | No relay, network, coordinator or additional Matter decision when the supported path already has what it needs. | Location label; continue equipment setup now or later. |
| Configure one screen with one supported source and one sound path | No equipment naming or repeated screen/source/audio selection; accept one review. | Activity title; Try now or later. |
| Add another activity through a known receiver path | Select a new source, or accept suggested sources together; no repeated cable/receiver identity questions. | Alternate supported sound path or custom title. |
| Choose between two identically named sources | One selection using useful location/connector context; add a focused identification step only if needed. | Custom labels for future convenience. |
| Repair an actual OSD rename or moved cable | One focused mapping decision when it establishes the repair; no recreation of unaffected activities. | Further label cleanup. |

These are design targets to test, not measured results. Identity, routing or capability uncertainty can require additional questions, but each question must name the missing fact and why the requested activity needs it. Review the evidence for unnecessary questions before adding more automation or expanding the supported hardware matrix.

## 14. Implementation order and handoff

1. Deliver the presentation model and equipment picker over fixtures containing duplicate OSD names, multi-attachment receivers, proxies, unavailable devices and five-connector profiles.
2. Add bounded server drafts and the dependency-based question flow through the configuration/validation APIs; test refresh and stale-choice repair.
3. Integrate first-board and additional-member onboarding with the actual security/radio journals and hardware profile, including the browser-entry gate.
4. Implement screen-first activity suggestions, supported audio defaults, conflict previews, separate Save/Try and evidence-based results.
5. Apply the same vocabulary and retained references to remote setup, integrations, recabling, removal, replacement, update and recovery.
6. Run UX-01 through UX-16 with the protocol, hardware and failure evidence required by document 15. Reduce supported scope where the evidence cannot justify the simple path.

[Agent work packages](16-agent-work-breakdown.md) assign code ownership. Do not implement the UI in isolation from the underlying name-resolution, membership, topology and planner contracts; those contracts are what make the uncomplicated user experience reliable.
