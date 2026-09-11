# ADR-001: BLE-only transport, NTRIP proxied through the phone

- **Status:** Proposed (2026-09-11). Needs the bench A/B in §7 before acceptance.
- **Scope:** rtk-rover firmware, rwa-player (NTRIP client, RTCM writer), rwa-creator
  (heading consumer), PROJECT-PLAN.md (§4–5 contract). RWAHT is unaffected.
- **Supersedes on acceptance:** the NimBLE port (memory ladder step 1), the hotspot
  path warmer (`hotspot-path-warmer` branch), the custom OTA partition table question,
  and the "never request a fast BLE connection interval" rule in `setupBLE()`.

## 1. Context

The unit runs two radios on one ESP32: WiFi to a phone hotspot for NTRIP corrections
(RTCM in, GGA out), and BLE to the same phone for head orientation (100 Hz binary frame
on `713D0005`), position, and telemetry. Head orientation has the hardest requirement:
motion-to-sound latency must be low enough that a rendered source stays fused with real
sources while the head turns. Corrections tolerate seconds of age. The two radios share
core 0 and the antenna through the coexistence arbiter, and the Arduino framework ships
ESP-IDF precompiled, so coex and Bluedroid pool sizes are not tunable.

Measured facts driving this decision (rwa-hs-1, 2026-08-26 to 2026-08-28, 300 s captures):

| Observation | Value |
|---|---|
| BLE interval iOS grants without a request | ~45 ms (top of the advertised 22.5–45 ms preference) |
| Heading delivery jitter, WiFi associated + NTRIP streaming | ~50 ms floor, tails to 100–150 ms |
| Heading delivery jitter, WiFi off (`test-ntrip-off` firmware) | 52–59 ms, no tails |
| RTCM with a 15–30 ms conn-interval request (A/B/A) | 0 RTCM bytes in 300 s |
| RTCM without the request, identical firmware | ~265 pushes / 300 s, fix 190 mm |
| Free heap, WiFi on, steady / minimum | ~10–12 KB / 2–3 KB (below the 6–8 KB escalation threshold) |
| Free heap, WiFi off | 66 KB (WiFi driver ≈ 54 KB) |
| Flash, WiFi + lwIP + NTRIP client included | 1.65 MB, 78.7 % of the single 2 MB slot |
| Hotspot upstream doze | reconnect backoff gaps let iOS idle the cellular path; SYNs then fail (doom loop) |

The jitter tails are structural: WiFi beacon wakes (~102 ms cadence) preempt BLE
connection events through coex even when nothing is streaming. The conn-interval cliff
is structural too: doubling BLE radio events starves WiFi RX. Neither can be tuned away
in this toolchain, so today the head-tracking link is pinned at whatever iOS grants
unasked, and the position link depends on a hotspot that dozes.

## 2. Decision (proposed)

Remove WiFi from the firmware. The phone becomes the NTRIP client; the unit becomes a
pure BLE peripheral.

- **RTCM downlink.** New characteristic, write-without-response. Each written chunk goes
  straight to the ZED-F9P with the existing raw push; RTCM is a byte stream, so no
  reassembly and no framing on the BLE layer.
- **GGA uplink.** Either a 1 Hz notify of the GGA sentence, or the app builds GGA from
  the `gnss_fix` telemetry event, which already carries lat/lon, fix type, sats, hdop
  and altitude. Before the receiver has a fix the app seeds GGA from CoreLocation.
- **NTRIP client in rwa-player.** HTTP GET with the ICY response, basic auth, GGA write
  every ~10 s, on NWConnection. Runs under the background-audio mode the player
  already requires. Caster credentials become an app setting next to Unit ID; the
  firmware no longer embeds per-unit secrets, so one image serves the fleet.
- **Connection interval.** With coex gone, request the fastest interval iOS allows
  (§5). The `setupBLE()` warning is retired with this ADR.
- **`ntrip_status` telemetry** moves to the app side. `wifi_disconnected`,
  `ntrip_connect_failed`, `ntrip_rtcm_timeout` are retired; the app emits
  the caster state on the session channel. `rtcm_pushed` (bytes/s) stays device-side
  as the proof that corrections actually reach the receiver.

## 3. Why

- **The coex fight ends.** No beacon-wake tails, no interval cliff. The heading link
  can be tuned for latency instead of for WiFi survival.
- **Memory stops being a project risk.** Steady free heap goes from ~10 KB to ~66 KB.
  The connect-time OOM history, the 4 KB telemetry ring cap and the NimBLE port all
  lose their motivation.
- **Flash headroom.** WiFi, lwIP, WiFiClient/WiFiUDP and the NTRIP code are the largest
  removable block in the image. This is probably what makes a two-slot OTA table fit
  without a custom layout.
- **Hotspot dependence goes away entirely.** No SSID convention, no WiFi credentials in
  fleet-secrets, no cellular-doze doom loop, no path warmer. The phone keeps its own
  socket open, and its uploads stayed alive through the outage that starved the unit.
- **Cold-boot deadlock dissolves.** The VRS streams nothing until it gets a position;
  the app can seed GGA from CoreLocation before the F9P has a fix.
- **Power and brownouts.** WiFi TX bursts are the largest current transients on the
  AP2112K rail. The brownout regression is not attributed yet, but removing the largest
  transient is the cheapest experiment against it.

## 4. Bandwidth check

VRS epochs in the logs are 1.0–1.4 KB at 1 Hz. iOS writes without response at MTU 185
carry ~182 B of payload, so one epoch is 6–8 packets. BLE carries both directions inside
one connection event, and an event can hold several packets each way, so the RTCM burst
and the heading notifies do not queue behind each other the way WiFi RX and BLE do. At
a 15 ms interval there are ~66 events/s for ~8 RTCM packets/s and ≤2 heading packets
per event. Position (10 Hz) and telemetry (2 notifies / 100 ms) add a few packets per
second. Nothing here is within an order of magnitude of the link capacity.

## 5. Minimum stable head-tracking transmission interval

The transmission interval is the BLE connection interval. The firmware notifies at 100
Hz, but notifies queue in the controller and flush together each connection event, so
the app sees bursts of ⌈interval / 10 ms⌉ frames once per interval and acts on the
newest. The link's contribution to motion-to-sound latency is therefore about half an
interval on average, one interval worst case, plus a few ms of stack on each side, and
the delivery jitter is bounded by one interval. Measured: 45 ms interval gives 52–59 ms
jitter with WiFi off, i.e. interval + ~10 ms.

**Today, WiFi on: 45 ms.** That is the measured floor. Any request of 15–30 ms kills the
NTRIP stream. A gentle 30/30 ms request might shave 15–20 ms and is untested; it needs
the RTCM A/B before adoption and would still leave the 100–150 ms beacon tails.

**BLE-only: 15 ms.** Apple's accessory guidelines set the floor for non-HID
peripherals at 15 ms, with Interval Min a multiple of 15 ms and Interval Max at least
15 ms above it. 11.25 ms is reserved for HID and is not worth chasing. So the request
is 15–30 ms (0x0C–0x18, slave latency 0), which is exactly the request that starved
WiFi and is harmless once WiFi is gone. iOS may still grant the upper end or lengthen
the interval when other peripherals are attached (a watch, for instance; the visitor
headphones are wired so A2DP is not in the path), which is why the interval is measured
rather than assumed (§7).

**Firmware-side stability at 15 ms.** Bluedroid handles 66 events/s without difficulty;
the heading task already produces at 100 Hz and each event carries at most two 16 B
frames, so nothing accumulates. A 100 Hz notify cadence into a 15 ms interval is
therefore both the minimum and the sensible setting: any faster notify cadence cannot
be delivered, any slower one adds hold time before the next event. If a later
measurement shows congestion returns from `notify()`, the fix is to notify once per
event from the freshest frame, not to slow the sensor.

**Expected link numbers at 15 ms:** mean link latency ~8–12 ms, worst case ~20 ms,
delivery jitter ~20–25 ms, versus ~25/45/55 ms today without tails and up to 150 ms
with them.

**Where that lands in the whole chain.** Perceptual thresholds for head-tracking
latency in binaural reproduction are reported around 50–70 ms for most listeners, with
sensitive listeners near 30 ms. The chain today: BNO080 fusion and 100 Hz report
(~10–15 ms), BLE link (25–150 ms), the player's 10 ms resampling timer, 21.3 ms audio
IO buffer, and the 256-sample convolution grid plus 512-sample crossfade (~17 ms). The
BLE link is the dominant term and the only one with tails. At 15 ms the link becomes a
minor term and the chain lands around 60–80 ms, which is at the threshold rather than
under it. Getting clearly under it also needs the deferred rwa-player stage 3 (audio
buffer and crossfade), but that work only pays off once the link stops dominating.

## 6. Consequences

- **One failure domain.** A BLE drop now loses corrections too. Heading already dies
  with BLE and the installation is unusable without it, so this collapses two
  independent failure modes into one rather than adding a new one.
- **No standalone RTK.** The unit only converges while the app is connected. Fine for
  the installation; to be stated in PROJECT-PLAN.
- **rwa-creator.** A unit connected to the creator gets no corrections unless the
  creator also proxies NTRIP. Acceptable for authoring if it only needs heading; decide
  explicitly.
- **OTA.** OTA over WiFi is off the table before it existed. OTA over BLE is feasible
  but slow (~5 min per image), so USB stays the primary path.
- **Time.** If anything used NTP over WiFi, the app pushes wall-clock over CTRL.
  Telemetry timestamps are already device-relative plus GNSS time.
- **Contract change.** PROJECT-PLAN §4–5, the rwa-player decoder and the Grafana
  alert dimensions that key on the retired error codes all change together. No
  dual-transport period: fleet firmware and app ship together, as with 0.46.0.

## 7. Verification before acceptance

1. **App side first**, since it is the only unknown: NTRIP client in rwa-player against
   the caster with one unit's credentials on cellular. Pass: ICY 200 and a steady
   stream for 30 min in background-audio mode.
2. **Firmware on a branch from `test-ntrip-off`**: add the RTCM characteristic and the
   15–30 ms conn-param request, then `tools/watch.sh 300`. Pass criteria, against the
   WiFi baseline in §1: RTCM pushes ≥ 250 / 300 s, fix converges to ≤ 200 mm, heading
   delivery jitter ≤ 30 ms with no tails, heap minimum ≥ 40 KB, and the granted
   interval logged at connect.
3. **Two-radio regression check**: brownout count per hour on the same two units that
   showed the 0.46.0 regression.
4. Only then delete WiFi and NTRIP from the firmware, retire the listed error codes,
   and revisit the OTA partition choice with the smaller image.

## 8. Alternatives considered

- **Keep WiFi, port BLE to NimBLE.** Frees ~30–50 KB heap and ~100 KB flash but leaves
  coex, the interval cliff and the hotspot doze untouched. Fixes the memory symptom,
  not the latency cause.
- **Keep WiFi, gentle 30/30 ms request.** Maybe 15–20 ms gained, tails remain, needs
  the same A/B. Worth it only if this ADR is rejected.
- **Separate WiFi hotspot hardware.** Removes the doze but not coex, and adds a device
  per visitor.
- **Rebuild as Arduino-as-IDF-component for coex tuning.** Unlocks knobs whose effect
  is unknown, at a large toolchain cost, for a fight that need not exist.
