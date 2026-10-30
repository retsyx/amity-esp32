# Loss, removal and manual recovery

The original controller remains the only UI, input and membership authority. The transport protects original local execution; it does not create a successor controller, persistent activity approval or automatic failover. [Role policy](21-original-controller-policy.md), [transport](08-consortium-transport-and-routing.md), [storage](11-configuration-storage.md) and [membership](09-membership-security-and-enrollment.md) apply.

## Missing equipment or owner

Keep saved qualified selectors and original activity choices. Resolve owner, adapter, side and OSD name through normal runtime handling. A missing recipient receives the original local failure/discovery treatment; it does not rewrite an activity to disabled. Later return does not need Edit/Save/reapprove. Do not select a similarly named recipient on another port or replay a missed relative input.

Current held-key recipients and cleanup stay exact. Link/authority loss prevents stale new work and ends affected live contexts under the existing contract; it does not authorize a new repeat cadence, renewal stream, outgoing retry queue or later retry of a dropped CEC release. Preserve already admitted FIFO behavior and safe end-of-stream release.

## Controller loss and restart

A member does not host a fallback UI, accept a remote, elect/promote itself or choose an activity. It retains local isolation and waits for its original controller. Preserve the original startup and explicit activity controls. Do not restore executable plans, discovered addresses, valid grants, held input or a current-activity/resume journal from storage.

Reconnect establishes current authenticated identity and transport context. Old messages cannot acquire a new lifetime or target. Equipment returning allows ordinary current lookup; it does not replay uncertain work or transfer controller authority.

Under the **29 October 2026 scan/cache amendment**, reconnect itself does not enumerate HDMI devices. Activity configuration changes do not disconnect members or clear their device caches/observations. Actual owner removal or boot replacement retains the necessary identity invalidation; boot is one of the three allowed scan triggers. An uncached communication recipient follows its owner/adapter/side's 120-second lookup cooldown, while explicit UI scans and boot bypass eligibility and refresh their side timestamps. See [discovery](05-discovery-and-topology.md).

## Intentional removal

Only an explicit original-controller Remove command revokes a selected member. Keep reviewed membership revision and target boot checks. Prevent stale queued positive work, drain the owner's bounded current wire/cleanup state, then commit the established reset journal on a responding target before acknowledging reset scheduling. The selected target restarts only after the response transmission attempt.

Roster removal and target erasure are separate outcomes. Offline/unconfirmed removal must explain that the user must hold that device's physical reset button for at least ten seconds before adding it again. Do not automatically replay an uncertain remove/reset request. A member lacking compatible reset support must be updated before a claimed remote reset.

Generic authority loss, link failure, retirement, failed setup or enrollment cancellation does not request factory reset. Saved activities/equipment references remain saved; lookup reports unavailable while their selected owner is absent. The main has no Remove action.

## Replacing an actuator

Isolate/revoke the old hardware before using replacement hardware in its place. Establish fresh device identity and operational trust through authorized enrollment. Explicitly map the saved owner/adapter selectors to the chosen replacement. Preserve component/activity IDs, labels, role choices, input values, order and slots. Do not clone private keys, remote/home pairings, old operational grants or discovered addresses.

Ordinary qualified lookup on the new owner supplies current equipment data. Replacement does not require rebuilding an approved physical-route graph or enabling activities. Unmapped references remain saved and unresolved rather than guessed.

## Manual main replacement from a configuration backup

1. Isolate the old main so it cannot return and control the same installation.
2. Matter-enroll replacement hardware as a new main. Its own checked role/identity establishes authority; the backup cannot do so.
3. Use the existing Advanced upload/restore control to restore portable saved intent. Preserve valid IDs, labels, original activity fields, selectors, order, slots. Rebase owners only through explicit proven hardware mappings. Reject unsupported saved formats and obsolete enabled/route/binding/resource/domain/PA fields; do not import live addresses or current-activity state. Import does not itself start an activity.
4. Pair the sole remote and Matter/HomeKit integrations again. Do not copy their pairing stores, Wi-Fi/admin credentials or private keys from the old main.
5. Factory-reset each old headless member with the existing at-least-ten-second hold, then release a three-to-below-ten-second hold to reopen its candidate window and re-add it through Amity devices → Add on the replacement main. Its origin ID and factory Matter/admin credentials survive; operational keys, network and roster trust are fresh. Only the stricter matched build requires the existing printed Matter code.

This remains manual recovery with no live authority transfer or member promotion. A returning old main stays isolated until explicitly reset/reclaimed. Original activity handling, including AVR audio and receiver-input fallback, uses the new explicit selectors with live lookup.

## Evidence

Exercise loss/reconnect during presses, queued commands, full FIFO and final releases; old boot/input messages cannot reach a replacement endpoint. Verify missing-name return changes runtime outcome without configuration writes. Removal/reset tests must distinguish roster completion, durable reset acknowledgment and actual reboot. Host tests do not prove physical isolation, AV behavior or ecosystem re-pairing.
