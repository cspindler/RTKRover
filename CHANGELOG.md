# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(`FW_VERSION_BASE` in `src/RTKRoverConfig.h`, reported with the build's git hash
in every telemetry heartbeat). History before 0.44.0 predates this changelog.

## [Unreleased]

### Changed

- Don't park `task_rtk_get_rover_position`s first run: keep polling the
  receiver. Parking it until the first RTCM push deadlocked cold boots (observed
  2026-08-25, rwa-hs-1): no polling -> no fresh GGA -> VRS streams nothing -> no
  RTCM -> still parked. Fixless runs are cheap and gnss_fix telemetry now shows
  them.
- Don't send fixless GGA to the caster, and don't connect without one: a
  quality-0 sentence no longer reaches `ggaSentence` (`ggaFixQuality()` in
  `callbackGPGGA`), and a new connect gate next to the receiver-liveness gate
  requires a fix-quality GGA at most 30 s old (`NTRIP_GGA_FIX_MAX_AGE_MS`).
  Acquire first, connect second. Consequence: a unit losing fix for > 30 s stops
  reconnecting until fix returns, and a mid-session fix loss stops the GGA
  pushes (the RTCM timeout then closes the session) instead of repeating the
  last position.

### Fixed

- Read caster response until NUL-terminator, prevent reading past the received
  data into stale stack memory.
- Properly parse source-table response, reporting such as errors.

## [0.45.0] - 2026-08-25

### Added

- **GNSS receiver watchdog + recovery ladder** (bench 4.1, 2026-08-24: the
  ZED-F9P went mute, and stayed dead 14+ min with no self-recovery, while the
  firmware cycled dataless caster sessions every ~93 s). The receiver's GGA
  output (~1/s) is the liveness signal (`lastGgaHeard_ms`, fed by
  `callbackGPGGA`):
  - After 30 s of silence (`GNSS_SILENT_AFTER_MS`) caster connects are skipped.
    A VRS streams nothing without our GGA, so reconnecting a mute receiver is
    pure caster noise.
  - A recovery ladder runs every 60 s (`GNSS_RECOVERY_GAP_MS`), escalating per
    attempt: reconfigure -> GNSS software reset -> hard reset (cold start), each
    followed by `configureGNSS()` (single-attempt begin + full rover config,
    extracted from `setupGNSS()` which now retries around it). Every rung emits
    a sev-2 `gnss_degraded` error naming silence duration, attempt number and
    action. A real GGA resets the ladder and reopens the gate.

### Changed

- The NTRIP socket is drained completely each iteration (buffer-sized slices,
  16 KB backstop cap) instead of one 2 KB read: unread RTCM no longer piles
  up in lwIP (the ~7 kB free-heap dips) and the caster no longer sees a zero
  window from us.
- `gnss_fix` is emitted only when the receiver delivered a fresh solution: the
  stream now gaps during receiver stalls instead of repeating stale fixes
  (PROJECT-PLAN §4.3 note). `heartbeat.ntrip_connected` is updated at every
  connect/stop, not only at the loop top, so it can no longer report a stale
  `true` through a stalled iteration.
- The position-side `gnss_pipe_stall` probe measures the whole mutex-held
  `updatePosition` body (the 2026-08-24 crawl was invisible to the
  checkUblox-only probe).
- moved `telemetryBleStartTask()` before sensor setup, so failures in
  `setupGNSS()` and `setupBNO080()` become visible in diagnostics.

- **Docs only: telemetry contract v3 (PROJECT-PLAN.md §1.1, §4.2, §5.3).** The
  per-boot frame counter (CBOR key 1) is no longer a dedup key: the app maps it
  to `dev_seq` on the JSON leg and assigns the backend `seq` itself, so a reboot
  mid-session (counter restarts at 1, `t_dev_ms` restarts at 0) cannot collide
  any more. Glossary adopted across repos: *headset assembly* (this firmware
  makes it the *RTK headtracker*), *board* (bare Feather, flashing only),
  *phone*, *unit* (assembly + phone, label `rwa-hs-N` = `device_id`); *rover*
  names the RTK role of the receiver, not the hardware. Comments and tool
  messages updated accordingly (`telemetry.cpp`, `telemetry_keys.h`,
  `tools/*`, README, CLAUDE.md). No firmware behaviour or key-table change.

- Navigation rate 20 -> 10 Hz (`NAVIGATION_FREQUENCY_HZ`): 20 Hz is beyond the
  F9P's multi-GNSS RTK spec (20 Hz is GPS-only) and the prime suspect for the
  receiver wedging into the slow-I2C / mute states of 2026-08-21/24. All
  consumers sample at <= 10 Hz (100 ms task intervals); halves I2C traffic and
  module CPU load.

- `setI2CTransactionSize(128)` (library default 32; the library itself
  recommends 128 on ESP32): 4x fewer I2C start/stop cycles for the same data.

- NMEA callback dispatch moved to the top of the NTRIP task loop so the GGA
  liveness clock ticks in every iteration, including receiver-silent ones.

### Fixed

- **Heap leak during WiFi outages** (bench captures 2026-08-24: −4 B/s ≈
  −14.5 kB/h, linear, plus several-kB transient dips per cycle, meaning OOM after
  ~1 h of continuous hotspot loss): both WiFi wait loops (boot in `setup()`,
  outage in the NTRIP task) ran a full `setupStationMode()` per ~12 s retry,
  i.e. a complete WiFi driver deinit/init cycle each time, which arduino-esp32
  2.0.x leaks on. The loops now wait softly: auto-reconnect keeps retrying,
  kicked by a `WiFi.reconnect()` nudge every 10 s (plain disconnect+connect,
  no teardown; `WIFI_RECONNECT_NUDGE_MS`), and escalate to one full driver
  re-init only after 5 min without association (`WIFI_REINIT_AFTER_MS`,
  wedged-driver escape hatch).

- **NTRIP death spiral under a degraded receiver** (field captures 2026-08-21
  and 2026-08-24): when the ZED-F9P entered a slow-I2C state, the position task
  held `mutexSem` 12–26 s per pass, the NTRIP task's `portMAX_DELAY` takes
  starved it past its own fixed 10 s no-RTCM window, and every fresh caster
  session was killed in the iteration that opened it (~2 reconnects/min,
  `bytes_rx` frozen, GGA never sent). Four changes break the spiral:
  - Every `mutexSem` take in the NTRIP task is bounded (`GNSS_MUTEX_TIMEOUT_MS`
    2 s; GGA copy and `callbackGPGGA` 250 ms). On timeout the I2C work of that
    slice is skipped (dropping a redundant correction slice beats stalling the
    link) while socket drain, GGA push and the heartbeat flag keep running.
  - The no-RTCM hangup window is 30 s after a (re)connect
    (`NTRIP_CONNECT_GRACE_MS`, VRS spin-up + GGA round-trip) and 10 s
    steady-state; a GGA is pushed immediately on connect (gate expired) since
    the VRS streams nothing before it.
  - Reconnect backoff: 5 s doubling to 60 s per consecutive failed or dataless
    attempt, reset by received RTCM (caster etiquette; dataless "successful"
    connects count as failures).
  - `updatePosition` pays at most three I2C passes per mutex hold: one
    freshness check per streamed packet (HPPOSLLH/HPPOSECEF/PVT), then pure
    cached reads. Previously every stale getter re-ran a hidden ~1 s
    checkUblox pass (up to ~13 per emit second) which was the actual
    12–26 s holder (invisible to the checkUblox-only probe).

## [0.44.3] - 2026-08-20

### Fixed

- **NTRIP caster-response timeout wedged the correction task until reboot**
  (`task_rtk_get_corrrection_data` in `main.cpp`): when the caster did not
  answer the mount-point request within `CONNECTION_TIMEOUT_MS`, the timeout
  branch stopped the client and issued `continue` - but inside the
  response-wait `while`, not the task loop (the block came from SparkFun's
  Example15-NTRIPClient, where it was a `return`).

  With the client stopped, `available()` stays 0, so the task spun on "Caster
  timed out!" forever: no corrections, `beginPositioning` never set, hence no
  RTK position frames and no `gnss_fix` telemetry for the rest of the boot.

  The timeout now breaks out of the wait loop and retries the connection from
  the top of the task loop, and emits a once-per-outage `ntrip_connect_failed`
  error ("caster response timeout").

### Changed

- **Fleet-configured BLE name** (`ble_name` in `tools/fleet-secrets.ini`,
  generated into `CasterSecrets.h` as `kBleName`): the advertised BLE name now
  defaults to the board label (e.g. `rwa-hs-3`), so sticker, phone hotspot and
  BLE scan all show the same identity. You can override it with `ble_name` in a
  board's section. The generator enforces fleet-wide unqiue names and the
  29-byte scan-response limit, and migrates kept headers by appending an empty
  `kBleName`. An empty `kBleName` (placeholder builds, kept pre-migration
  headers) falls back to the previous chip-id scheme (`rtkrover-<chipid>`,
  `getBleName()` in `main.cpp`), so unprovisioned builds keep working.

  Background: units renamed across firmware generations (captive-portal names
  like `rtkrover_3` -> chip-id names) exposed iOS's persistent GAP-name cache:
  `peripheral.name` kept returning the old name, so exact-name matching in the
  iOS apps could never see the device's real name. The apps now also match the
  advertised local name (rwa-player / rwa-receiver, changelogs there).

## [0.44.2] - 2026-08-13

### Added

- **Reset-reason telemetry** (`reportResetReason()` in `main.cpp`): the boot
  cause is read via `esp_reset_reason()` at the top of `setup()` and, for
  abnormal causes, emitted as a severity-2 `error` event with stable codes
  `reset_brownout`, `reset_panic`, `reset_wdt`; the message carries the raw
  reason number and the battery voltage at boot. Power-on / software reset /
  deep-sleep wake emit nothing. The event waits in the telemetry ring until BLE
  connects, so field reboots show up in the Diagnostics tab and Grafana instead
  of only as a repeating boot blink pattern. Additive `error.code` values per
  PROJECT-PLAN.md §4.4, no schema change, no decoder change.

### Changed

- **Brownout mitigation at WiFi init** (field tests 2026-07..08: units on
  battery power boot-looped at first radio-on; the AP2112K 3V3 rail, also
  carrying the ZED-F9P and BNO080, sags below the brownout threshold under
  full-power TX spikes):

  - WiFi TX power is capped at 11 dBm (`WIFI_TX_POWER` in `RTKRoverConfig.h`)
    from the first radio-on, roughly half the peak TX current of the 19.5 dBm
    default. The hotspot is the wearer's own phone, so link margin is OK;
    check heartbeat `wifi_rssi` before lowering further. The cap is re-applied
    in `setupStationMode()` because `WiFi.disconnect(true)` stops the driver,
    which silently resets TX power to default.

  - `setupWiFi()` no longer pre-scans for the hotspot: the old
    wait-until-visible loop ran a full-power all-channel active scan every
    second: for a unit powered on before the phone's hotspot, minutes of
    repeated worst-case current spikes. `WiFi.begin()` now probes only the
    target SSID; if the hotspot is not up the attempt times out after 10 s and
    the existing retry loop in `setup()` blinks and tries again.
    `checkNetworkAvailable()` is gone (`setupWiFi()` was its only caller).

### Removed

- **RTK accuracy characteristic (`713D0006-...`)** and its plumbing
  (`xQueueAccuracy`, the send-if-changed block in `updatePosition`). No
  consumer was left: rwa-player reads accuracy from the telemetry
  `gnss_fix` event (`h_acc_mm`, 1 Hz), and rwa-receiver dropped its
  subscription in the same change. The position accuracy value itself is
  still read every cycle — it gates the `713D0004` position stream
  (`MIN_ACCEPTABLE_ACCURACY_MM`) and feeds the telemetry sample.

## [0.44.1] - 2026-08-04

### Changed

- **Per-unit credentials are generated at build time, not hand-edited**:
  `src/CasterSecrets.h` is now a build artifact produced by
  `tools/gen_caster_secrets.py` (extra_script) from two sources joined by
  board label: the committed fleet map `tools/known-boards.txt` and the
  gitignored `tools/fleet-secrets.ini` (shared `[caster]` settings plus one
  section per unit: `caster_user` = `...01`..`05`, `wifi_pw`, optional
  per-board overrides). Board selection: `RTK_BOARD` env var (label or
  serial; `flash.sh` exports its board argument), else the single attached
  unit. Incomplete secrets fail the build only when the board was requested
  explicitly or an upload is queued; a plain build keeps the previous header
  (or writes an empty placeholder on a fresh clone), so hardware-less builds
  still work.
- **WiFi SSID convention**: SSID = board label: name each phone hotspot
  after its unit (e.g. `rwa-hs-2`) and the SSID needs no configuration;
  `wifi_ssid` in a board section overrides it for hotspots not yet renamed.

### Removed

- `kDeviceName` from `CasterSecrets.h`. It's dead code; the BLE name is derived
  from the chip ID at runtime (`getDeviceName`).
- `src/CasterSecrets_example.h`, superseded by
  `tools/fleet-secrets_example.ini`.

## [0.44.0] - 2026-07-30

### Added

- **Telemetry subsystem** (PROJECT-PLAN.md §4–5): device events stream to the
  RWA Player over BLE as length-prefixed CBOR frames.
  - New GATT service `713D0100-…` with TX notify (`…0101`) and CTRL write
    (`…0102`) characteristics; the §5.3 integer key table lives in
    `src/telemetry/telemetry_keys.h`, mirrored in rwa-player.
  - 4 KB drop-oldest ring buffer; producers never block (bounded-memcpy
    critical sections only). Dropped frames are counted and reported.
  - Drain task at the lowest priority in the system, paced to ≤ 2
    notifications per 100 ms tick so a backlog drain cannot crowd the
    head-tracking stream. MTU-aware (Bluedroid truncates oversized
    notifications silently); partial frames never leak across reconnects.
  - Event emitters: `gnss_fix` at 1 Hz (high-res lat/lon, fix/carrier type,
    accuracies, SIV, PDOP, correction age), `heartbeat` every 15 s
    (free heap, WiFi RSSI, NTRIP state, fw version, dropped frames,
    battery millivolts),
    `ntrip_status` on state transitions, `imu_status` every 60 s (measured
    report rate, calibration accuracy, reset count), `error` events with
    stable codes on the real failure paths (`wifi_disconnected`,
    `ntrip_connect_failed`, `ntrip_bad_response`, `ntrip_rtcm_timeout`,
    `i2c_bus_rtk_failed`, `i2c_gnss_not_detected`, `i2c_bno080_not_detected`)
    — one event per outage, never one per retry.
  - CTRL commands (§5.4): `0x01` set error-verbosity threshold, `0x02`
    immediate status dump.
- **Battery reporting** (`src/battery.{h,cpp}`): pack voltage ships in every
  heartbeat as `batt_mv` (§4.3/§5.3 key `16`, uint millivolts, 0 = unknown).
  Raw millivolts, not a percentage - the discharge curve is a display concern
  and does not belong in the wire format. Needs the matching key in
  rwa-player's `TelemetryKeys.swift`.
  - The long-standing "how to measure battery" note in `main.cpp` (ADC2 is
    arbitrated against WiFi, so use an LC709203F fuel gauge or interleave
    WiFi and ADC access) does not apply to this board: the Huzzah32's 2:1
    divider is on A13 = GPIO35 = **ADC1**_CH7, which the WiFi driver never
    blocks. No extra hardware, no interleaving. Note replaced with the
    finding. Measured on hardware with WiFi associated: 4198–4200 mV across
    heartbeats, i.e. stable to ±2 mV.
  - Costs ~6.9 KB flash (78.0 % → 78.4 % of the 2 MB slot) for the ADC
    calibration driver — relevant to the open OTA partition decision.
- **Build-time version embedding**: `FW_VERSION` = semver + short git hash
  (+`-dirty`), generated into a gitignored header by `tools/git_version.py`.
- **On-device unit tests**: 20 AUnit tests (CBOR encoder byte-exactness,
  drop-oldest/wrap-around buffer semantics, wire framing). Debug builds run
  them in `setup()` before the WiFi wait, so they execute even with no
  hotspot in range.
- **Bench tooling** (`tools/`): `find-board.sh` resolves the attached unit
  against `known-boards.txt` (CP2104 serial → label; unknown/ambiguous/absent
  are three distinct errors), `flash.sh` builds + uploads, `watch.sh` +
  `serial-capture.py` capture bounded, timestamped serial windows without a
  TTY (`pio device monitor` cannot run non-interactively) and nudge debug
  builds past the boot keypress prompt.
- **Debug diagnostics**: 10 s heap/stack-watermark report in `loop()`,
  free-heap logging on BLE connect/disconnect, MTU-change logging.

### Changed

- `featheresp32_debug` build env added; `DEBUGGING` is now driven by
  `-DDEBUGGING=1` from the env instead of editing `RTKRoverConfig.h`.
- Task stacks right-sized from measured watermarks: 35 KB → 21 KB total,
  freeing ~14 KB of heap. The NTRIP task was *grown* 7 → 9 KB — it had been
  running with a 280-byte margin.
- `getBatteryVolts()` moved out of `main.cpp` into the new battery module (the
  telemetry task needs it) and now reads via `analogReadMilliVolts()`, which
  applies the per-chip eFuse ADC calibration. The previous
  `analogRead() * 3.3 / 4095` is off by 100+ mV on the ESP32's non-linear ADC.
  Reads are averaged over 8 samples; pin, divider ratio and sample count are
  in `RTKRoverConfig.h`.
- `platformio.ini`: shared `[env]` section; hardcoded `upload_port`/
  `monitor_port`/`test_port` removed (the port embeds one unit's serial
  number and goes stale on board swaps) — the tools discover the port.
- Stream nav messages instead of polling to unblock the position task
  - Polled GNSS getters block on an I2C poll round-trip per message; the 1 Hz
    gnss_fix telemetry burst measured up to ~2 s, stalling the position task
    and everything behind mutexSem. Enable auto delivery for NAV-PVT,
    NAV-HPPOSLLH and NAV-HPPOSECEF (getPositionAccuracy polled the latter
    every 100 ms too), so the getters become non-blocking cached reads.
  - Verified on hardware: getters now 0-1 ms every emit (was bursts to ~2 s),
    gnss_fix steady at 1 Hz, position frames and GGA push unaffected, heap
    unchanged (auto mode reuses the packet structs polling already allocated).
- Send GGA to the caster periodically, not just once per connection:
  `checkCallbacks()` was only invoked in the NTRIP connect success path, so
  `callbackGPGGA` never fired again after connect, `ggaSentenceComplete` stayed
  false, and the 10 s GGA-push block never ran. The VRS caster dropped the
  session every ~40 s for lack of GGA, forcing a silent reconnect that
  interrupted RTCM delivery.

### Fixed

- **Connect-time OOM panics** (`assert failed: vQueueDelete` in the BT stack,
  `abort()` in `lock_init_generic`): steady-state free heap was ~2 KB, and
  the GATT allocations at BLE connect pushed it over. Fixed by the stack
  right-sizing above and capping the telemetry ring at 4 KB; steady-state
  free heap is now ~10–18 KB depending on load.
- NTRIP request assembly could overflow its 512 B stack buffer
  (`strncat` bounded by destination size instead of remaining space,
  `-Wstringop-overflow`). Truncation is now detected, reported as
  `ntrip_request_overflow`, and a malformed request is never sent.
- NTRIP credentials buffer was sized with `sizeof(String)` (the object, not
  the text) and passed a `String` object through `%s` varargs (undefined
  behavior); passwords longer than ~16 characters would have silently
  truncated and failed authentication.
