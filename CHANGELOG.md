# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(`FW_VERSION_BASE` in `src/RTKRoverConfig.h`, reported with the build's git hash
in every telemetry heartbeat). History before 0.44.0 predates this changelog.

## [0.44.0] - 2026-07-29 (not yet tagged)

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
    (free heap, WiFi RSSI, NTRIP state, fw version, dropped frames),
    `ntrip_status` on state transitions, `imu_status` every 60 s (measured
    report rate, calibration accuracy, reset count), `error` events with
    stable codes on the real failure paths (`wifi_disconnected`,
    `ntrip_connect_failed`, `ntrip_bad_response`, `ntrip_rtcm_timeout`,
    `i2c_bus_rtk_failed`, `i2c_gnss_not_detected`, `i2c_bno080_not_detected`)
    — one event per outage, never one per retry.
  - CTRL commands (§5.4): `0x01` set error-verbosity threshold, `0x02`
    immediate status dump.
- **Build-time version embedding**: `FW_VERSION` = semver + short git hash
  (+`-dirty`), generated into a gitignored header by `tools/git_version.py`.
- **On-device unit tests**: 19 AUnit tests (CBOR encoder byte-exactness,
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
- `platformio.ini`: shared `[env]` section; hardcoded `upload_port`/
  `monitor_port`/`test_port` removed (the port embeds one unit's serial
  number and goes stale on board swaps) — the tools discover the port.

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
