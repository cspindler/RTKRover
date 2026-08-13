# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(`FW_VERSION_BASE` in `src/RTKRoverConfig.h`, reported with the build's git hash
in every telemetry heartbeat). History before 0.44.0 predates this changelog.

## [Unreleased]

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
