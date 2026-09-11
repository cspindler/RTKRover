# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(`FW_VERSION_BASE` in `src/RTKRoverConfig.h`, reported with the build's git hash
in every telemetry heartbeat). History before 0.44.0 predates this changelog.

## [Unreleased]

ADR-001: BLE-only transport, NTRIP proxied through the phone
(`ADR-001-ble-only-transport.md`). Breaking for rwa-player: the app becomes
the NTRIP client, and the telemetry contract loses the WiFi/NTRIP fields.

### Added

- **RTCM downlink over BLE: `713D0006`, write without response** (ADR-001
  par. 2; PROJECT-PLAN.md par. 5.6). The app writes the caster's byte stream
  in MTU-sized chunks. `src/corrections.{h,cpp}` takes each write on the
  Bluedroid task into a 4 KB drop-oldest chunk FIFO (the telemetry ring's
  class, reused: same "producer never blocks, single consumer" contract),
  and `task_gnss_corrections` pushes the queued chunks to the ZED-F9P with
  `pushRawData` under one bounded mutex take per 100 ms iteration. On a
  mutex timeout nothing is popped, so a slow-I2C stretch on the position
  task costs the *oldest* corrections, never the newest. RTCM3 is
  self-delimiting, so no framing on the BLE layer and no reassembly. The
  ADR's "straight to the receiver" holds in spirit, not in execution
  context: pushing from the write callback would put I2C and the GNSS mutex
  on the BT task, which also carries the heading notifies.
  Telemetry: heartbeat key 21 `rtcm_bytes` (bytes pushed since the previous
  heartbeat; bytes/s = value / 15), the assembly-side proof that corrections
  reach the receiver next to `corr_age_ms`; error code `rtcm_fifo_overflow`
  (severity 1, rate-limited to one per 10 s) when chunks were evicted
  unpushed. The heartbeat AUnit test covers the new counter.

- **GGA uplink over BLE: `713D0007`, notify** (ADR-001 par. 2, its "1 Hz
  notify of the GGA sentence" option; PROJECT-PLAN.md par. 5.6).
  `callbackGPGGA` notifies the receiver's own `$GPGGA` (CRLF stripped) for
  every sentence with a fix, so the app forwards exactly what the NTRIP task
  used to send: the 0.45.1 rule "no fixless GGA to the caster" stays, and
  the app seeds from CoreLocation until the first one. Chosen over having
  the app build GGA from `gnss_fix`: that event carries PDOP not HDOP and
  ellipsoidal not MSL height, and rides the lowest-priority telemetry drain,
  while the sentence itself is already parsed, free, and on the corrections
  task. Skipped, not split, while the MTU is still 23. `ggaFixQuality()`
  returns for the gate; no mutex in the callback any more (`notify()` only
  posts to the BT task).

### Removed

- **WiFi and the NTRIP client** (ADR-001 par. 2). The assembly no longer
  associates with the phone's hotspot and no longer talks to the caster.
  Gone: `handle_wifi.{h,cpp}` (association ladder, TX-power cap, chip-id
  helpers), `task_rtk_get_corrrection_data` (connect gates, backoff, ICY
  request, socket drain, GGA push, RTCM timeout), the hotspot path warmer,
  the `WIFI_*` / `NTRIP_*` / `HOTSPOT_*` constants and the 300 ms radio
  stagger in `setup()`. What that task did *for the receiver* survives as
  `task_gnss_corrections` (core 0, priority 2, 100 ms, 6 KB): the NMEA
  callback dispatch and the receiver watchdog / recovery ladder. The GGA
  callback keeps only the liveness timestamp; the fix-quality copy for the
  caster and its mutex take are gone with the caster. `GNSS_MUTEX_TIMEOUT_MS`
  moved to the watchdog section; `getChipId()` moved into `main.cpp`.
  Corrections reach the receiver over BLE from here on (next entries).
  Consequences the ADR states: no standalone RTK (the receiver converges only
  while RWA Player is connected), one failure domain (a BLE drop loses
  corrections too, as it already lost heading), and rwa-creator gets no
  corrections unless it proxies NTRIP itself.

### Changed

- **BLE connection interval: 15–30 ms requested on every connect**
  (`ble_link.cpp`, ADR-001 par. 5). The advertised preference moves from
  22.5–45 ms to 15–30 ms (0x0C–0x18: Apple's floor for a non-HID peripheral
  and its "max ≥ min + 15 ms" rule), and since iOS treats the preference as
  a hint, `esp_ble_gap_update_conn_params` asks for the same range from
  `ESP_GATTS_CONNECT_EVT` (slave latency 0, supervision timeout 4 s). The
  grant arrives in `ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT`, is logged with its
  status, and the heading pacing follows it as before: at a 15 ms grant the
  10 ms sensor tick is the floor, so up to two fresh frames ride each of the
  ~66 events/s. This retires the 0.46.0 rule "never request a fast
  interval": it was measured against WiFi coex (0 RTCM in 300 s with the
  request), and WiFi is gone. Expected (ADR-001 par. 5): mean link latency
  ~8–12 ms, delivery jitter ~20–25 ms, no beacon-wake tails. The measured
  grant and rates are in the 0.48.0 bench notes.

- **One per-assembly fact left in the image: the BLE name.**
  `tools/gen_caster_secrets.py` → `tools/gen_assembly_config.py`,
  `src/CasterSecrets.h` → `src/AssemblyConfig.h` (generated, gitignored),
  carrying only `kBleName`. The name is the committed `tools/known-boards.txt`
  label, so a fresh clone with a known board attached builds a correctly
  named image with no secrets file at all; `tools/fleet-secrets.ini` is
  optional and read only for a `ble_name` override (uniqueness and the 29 B
  scan-response limit are still checked). Caster host / port / mount / user /
  password and the hotspot password no longer exist in the firmware: they
  are typed into RWA Player, and the fleet ini stays the record for
  provisioning the phones. The build's "keep the previous header" and
  placeholder behaviours are unchanged. ADR-002 moves the name into NVS and
  retires the generator; until then `RTK_BOARD` or the attached board still
  selects it at build time.

- **Docs follow the one-radio design.** `DOCUMENTATION.md` (task table with
  `task_gnss_corrections`, the RTCM FIFO under synchronisation, boot order
  without the WiFi step, the two new callback contexts), `README.md`
  (infrastructure, blink codes without the WiFi stages, the stale mklittlefs
  and serial-port paragraphs gone) and `CLAUDE.md` (hardware, operator needs,
  RAM and flash budgets, the memory escalation ladder marked superseded, work
  queue item 7).

- **Telemetry contract, breaking** (PROJECT-PLAN.md par. 4.3 and 5.3, v4).
  Retired on the BLE leg, never to be reused: heartbeat keys 12 `wifi_rssi`,
  13 `ntrip_connected`, 18 `loops_ntrip`; event type 3 `ntrip_status`; error
  codes `wifi_disconnected`, `ntrip_connect_failed`, `ntrip_rtcm_timeout`,
  `ntrip_bad_response`, `ntrip_request_overflow`. New: heartbeat key 20
  `loops_corr`, the corrections-task iteration count per interval (~150 per
  15 s), which takes over the `gnss_pipe_stall` liveness role `loops_ntrip`
  had. `ntrip_status` and `heartbeat.ntrip_connected` become app-created
  (`source` = `phone`): the app owns the caster session now. The rwa-player
  decoder and the Grafana panels keyed on the retired fields change with the
  fleet firmware; no dual-transport period. The AUnit heartbeat tests follow
  the new key set.

## [0.47.0] - 2026-09-11

Lean pass, conclusion / continuation of part 1

- Partition table: `min_spiffs.csv`, two 1.92 MB OTA slots (work-queue item 5 closed)
- BLE connection state in one place: `src/ble_link`
- `gnss_pipe_stall` instrumentation reduced to the iteration time
- `task_rtk_get_corrrection_data` split by concern (WiFi ladder, GNSS recovery, NTRIP session)
- Position pipeline: one task, no queue, notify after the mutex
- `RTKRoverConfig.h` trimmed to one line of rationale per constant

### Changed

- **Partition table: `min_spiffs.csv`** (two OTA slots of 1.92 MB, `ota_0`/`ota_1`
  + `otadata`, 128 KB SPIFFS unused, 64 KB coredump) instead of `no_ota.csv`
  (one 2 MB slot). Closes work-queue item 5: the stock `default.csv` slots
  (1.25 MB) cannot hold the image, `min_spiffs.csv` leaves ~330 KB. The image is
  unchanged (1,633,317 B production, 1,671,081 B debug), it now reads as 83 % /
  85 % of the slot instead of 78 % / 80 % of the old one. **Every unit needs one
  USB flash** to receive the new table; a verified boot afterwards on rwa-hs-1
  (WiFi, caster, RTCM, `gnss_fix` all within 20 s). CLAUDE.md and
  PROJECT-PLAN.md §8.1 / §9.5 say what shipped instead of what was planned.

- **BLE link state has one owner: `src/ble_link.{h,cpp}`.** A connect used to be
  observed in three places: `MyServerCallbacks` in `main.cpp` (the `bleConnected`
  global), `telemetry_ble.cpp` (its own `linkConnected` / `peerMtu` /
  `connectionGeneration` atomics) and the raw `bleGattsHandler` (the interval),
  with the heading pacing flags (`bleConnIntervalUnits`, `bleTxCongested*`) as
  globals in between. The new module owns connected, connection generation, MTU,
  granted interval and TX congestion as atomics, brings up Bluedroid
  (`bleLinkBegin`, `bleLinkStartAdvertising`) and holds the `BLEServerCallbacks`
  and custom GAP/GATTS handlers. `main.cpp` and the telemetry drain read it
  through `bleLink*()`; `telemetryBleOnConnect/OnMtuChanged` are gone and only
  the CCCD reset (`telemetryBleOnDisconnect`) remains a callback. The stale
  congestion-flag timeout moved into `bleLinkTxCongested()`. No wire or timing
  change; it is also the one file a NimBLE port would rewrite. Verified with the
  phone connecting during a bench boot on rwa-hs-1: interval 24 units learned
  from `ESP_GATTS_CONNECT_EVT`, MTU 517, heading notifies at the 20 ms pacing,
  position stream at 10 Hz. +140 B flash.

- **`gnss_pipe_stall` reports the time, not a phase breakdown.** The event was
  added 2026-08-21 to find a slowdown whose causes are fixed (20 Hz nav rate,
  polled getters, unbounded mutex takes). The four phase accumulators in the
  NTRIP task (ten `phase_ms` timing sites around every mutex take, push and GGA
  write) and the `llh pass` split in `updatePosition()` are gone; the message is
  now `ntrip iter N ms` / `updatePosition held mutex N ms`. Same `code` string,
  same threshold and rate limit, so the Grafana dimension is unchanged. −300 B.

- **`task_rtk_get_corrrection_data` split by concern** (was one 670-line
  function interleaving four of them; now ~380 lines that read top to bottom).
  No behaviour change:
  - `wifiEnsureAssociated()` in `handle_wifi.cpp` owns the WiFi reconnect ladder
    (nudge / doubling backoff / full re-init escape hatch / `wifi_disconnected`
    after the grace) and returns whether it waited. The WiFi module now owns
    association at runtime, not only at boot.
  - `gnssRecoveryTick()` next to `configureGNSS()` owns receiver-silence
    detection and the reconfigure → software reset → hard reset ladder, with
    its stage / count / last-attempt state as statics; returns whether it ran a
    recovery so the caller can exclude it from the stall clock.
  - `gnssCheckUbloxLocked(timeout)` replaces the two byte-identical bounded
    `checkUblox` blocks; `ntripBuildRequest()` builds the GET + Basic-auth
    request into the caller's buffer and reports overflow.
  - The five copies of the backoff arming and the three `outageErrorEmitted`
    guards are two lambdas (`armBackoff`, `emitOutageError`) over the task's
    own state.
  - `blinkOneTime()` / `ledInit()` moved to `src/led.{h,cpp}` so the WiFi module
    can blink its 1.0 s / 0.1 s code without reaching into `main.cpp`.

- **One task for the position pipeline.** `task_rtk_get_rover_position` now
  notifies the `713D0004` line itself, after releasing `mutexSem`:
  `updatePosition()` returns the coordinate when the solution is fresh and
  within `MIN_ACCEPTABLE_ACCURACY_MM`, and the task formats and notifies it.
  `task_send_rtk_position_via_ble`, the 2-deep `xQueueCoord` and the task's
  4 KB stack are gone, and with them the 100 ms producer/consumer phase offset
  (up to one period of added latency on the position stream). `notify()` only
  posts to the BT task, so calling it from core 0 is fine; the telemetry drain
  already notified from a third task. The "waiting for a phone" 0.1 s blink
  moved to the Arduino `loop()`, which otherwise sleeps 100 ms instead of
  spinning. Bench on rwa-hs-1 with the phone connected: position lines at the
  same 8–9/s as before the merge, position-task stack watermark 2.0 KB of 4 KB.
  `TASK_RTK_BLE_INTERVAL_MS` and `TASK_RTK_POSITION_VIA_BLE_PRIORITY` removed
  from the config header.

- **`RTKRoverConfig.h` is a config file again** (295 → 190 lines, every macro
  and value unchanged, byte-identical image). One line of rationale per
  constant plus a `CHANGELOG x.y.z` pointer replaces the bench dates, the doze
  doom loop, the brownout and the 2026-08-24 capture write-ups, all of which
  already live in this file; the HardwareX F9P quote and the Qwiic pull-up note
  are gone.

## [Lean pass part 1]

Lean pass, behaviour-preserving cleanups.

Net over the pass: 1064 lines deleted, 110 added; production image
1,646,457 -> 1,632,817 B (−13.6 kB), no behaviour change intended beyond the
items listed under Changed and Fixed.

### Changed

- `task_send_rtk_position_via_ble` formats the `713D0004` line with `snprintf`
  into a 32-byte stack buffer instead of four Arduino `String` temporaries per
  notification. Same bytes on the wire, but no heap allocation at 10 Hz on a
  heap with a measured 1.8 kB minimum, and `setValue()` gets the explicit
  length instead of a `std::string` temporary.

- The NTRIP task uses the `CasterSecrets.h` constants as `const char*` instead
  of copying five of them into `String`s, and parses the port once at task
  start rather than on every connect.

- A placeholder build (no fleet-secrets entry) parks the NTRIP task with
  `vTaskSuspend` instead of blinking the 2 s LED code forever; heading,
  position and telemetry run as before. The README LED table had already
  marked that code for removal.

- `bleConnected` is a `volatile bool`. It was declared `float` and read as a
  boolean by three tasks and the BLE callbacks.

- `src/hande_wifi.cpp` renamed to `handle_wifi.cpp`, matching its header.

- `DOCUMENTATION.md` rewritten from the code that runs: task table (core,
  priority, period, stack), synchronisation primitives, boot order, callback
  contexts.

- `.gitignore` covers `*.log` and `*.csv` in the repo root, where
  `tools/watch.sh` captures and heap-stat exports land.

### Removed

- **`src/utility/`** (imumaths, vector, matrix, quaternion; 772 lines). Nothing
  has included it since the Euler conversion moved to the apps with the binary
  heading frame (0.46.0); the production ELF linked zero bytes of it.

- **The reboot button** (`rebootButton`, `buttonHandler`, `REBOOT_BUTTON_PIN`)
  and the Button2 dependency. The object was constructed and a handler defined,
  but nothing ever called `setPressedHandler()` or `rebootButton.loop()`, so a
  press did nothing. The EN pin gives a hardware reset. −8.5 kB flash.

- **AUnit from the production image.** `TestsRTKRover.h` was included
  unconditionally, so its two placeholder tests (`correct` / `incorrect`)
  registered at static-init time and linked the test runner into every build.
  The include is under `#ifdef TESTING` now and the placeholders are gone; the
  telemetry suites are unchanged.

- The non-ESP32 base64 path (`#else` on `ARDUINO_ARCH_ESP32`). The board is
  fixed in `platformio.ini` and the branch could not have compiled here
  (`Base64.h` is not a dependency). The ESP32 path no longer copies the encoded
  `String` into a VLA before printing it.

- The per-task manual stack-measurement scaffolding: four unused
  `uxHighWaterMark` locals, their commented-out print blocks, and the `setup()`
  comment asking to uncomment them. The debug build's `loop()` has printed every
  task's watermark every 10 s since 2026-07-29, which is the procedure the
  remaining comment describes.

- Dead declarations: the `beginClient()` and `wipeWiFiCredentials()` prototypes
  (never defined; there is no filesystem), the `lastTime` global, and four
  config macros without a use (`DEFAULT_KEY`, `PAYLOAD_BUF_LEN`,
  `I2C_FREQUENCY_100K`, `BNO080_STEP_CNT_UPDATE_RATE_MS`).

- The `ESP_ERROR_CHECK` redefinition in `RTKRoverConfig.h`. Its comment claimed
  it deactivated brownout detection; it re-implemented IDF's default
  log-and-assert, and nothing in `src/` calls it.

- README: the "Dependencies (currently not in use)" section. Neither library is
  in the tree or in `platformio.ini`.

### Fixed

- The NTRIP credentials check rejects a port that does not parse. Previously a
  non-numeric port passed the check and the task connected to port 0.

- Debug builds no longer print the caster credentials. The NTRIP task logged
  `user:password` in clear text before encoding it, and then the whole request
  including the `Authorization: Basic` header (the same secret, base64).
  `tools/watch.sh` captures land in the repo root and get pasted around; only
  the request line (`GET /<mount> HTTP/1.0`) is printed now.

- The WiFi nudge log line reports the same heap measure as `logFreeHeap()` and
  the heartbeat (`esp_get_free_heap_size()`, the 8-bit-capable internal heap).
  It used `ESP.getFreeHeap()`, which also counts 32-bit-only IRAM and read
  ~36 kB next to the loop report's ~18 kB in the same capture.

[0.46.2] - 2026-09-11

### Changed

- One heading frame per BLE connection event (`task_bno_orientation_via_ble`):
  Sampling and transmission are now separate: the IMU is still drained every
  10 ms and the cached frame always holds the newest sample, but a frame goes on
  the wire only once per connection event.

  Nothing can leave the device between connection events: the central anchors
  the link at the negotiated interval and `notify()` only enqueues for the next
  one. Notifying faster therefore never made data arrive sooner; it queued 2–3
  frames that rode out in the same burst, where the app renders the newest and
  discards the rest. Each discarded frame still cost airtime (a longer
  connection event is a longer WiFi blackout through radio coex) and still held
  a Bluedroid TX buffer, which is heap: 0.46.0 roughly doubled the notify rate
  and cost every unit in the fleet 2–6 kB of p10 free heap (backend query
  2026-09-11, five units, same direction on all of them).

  The pacing adapts rather than guessing: the interval the central granted is
  read from `ESP_GATTS_CONNECT_EVT` (`conn_params.interval`), and the notify
  period is set one sensor tick short of it. `ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT`
  is also watched, for a later renegotiation — but it is *not* the primary
  source, because with iOS it never fires: the phone accepts the advertised
  preference at connect and runs no update procedure, which left the first
  bench build stuck on its fallback. No connection-interval *request* is made,
  and none may be added — a 15–30 ms request killed the NTRIP stream outright.
  Until an interval is known the period falls back to 15 ms, so a central that
  reports nothing costs no latency.

  Measured on rwa-hs-1 with RWA Player attached: iOS granted 24 units (30.00 ms),
  sampling held at 75 ticks/s, transmission fell from ~73.5 to ~37.6 frames/s.
  Against 33.3 connection events/s that is ~1.1 frames per event, down from ~2.2
  — close to the one-per-event target, with the residual coming from phase drift
  between the notify clock and the link's anchor points.

  `ESP_GATTS_CONGEST_EVT` now feeds back as well: a congested TX queue means
  earlier frames have not gone out, so the tick skips rather than deepening a
  backlog the app would discard. That replaces the silent frame loss behind the
  `esp_ble_gatts_send_notify: rc=-1` storms seen on bench-1. A stuck congestion
  flag is ignored after 500 ms so a missed "cleared" event cannot freeze head
  tracking.

  Contract effects, PROJECT-PLAN.md §5.5 updated: the frame rate is now the
  connection interval (~22–45 Hz on iOS) rather than ~100 Hz, `t_dev_ms` is
  stamped at sample time so the app can see real sample age, and `seq` counts
  frames put on the wire so drop detection keeps its meaning. Consumers were
  already told to derive rotation speed from `t_dev_ms` deltas rather than an
  assumed rate.

### Fixed

- A false `i2c_gnss_not_detected` (severity 3) on boots where the ZED-F9P
  answered `begin()` but did not acknowledge one of the configuration
  writes on the first pass (2 of 2 captures on 2026-09-11; the retry always
  succeeded). `configureGNSS()` now returns the name of the failing step
  instead of a bare false. `setupGNSS()` raises the fatal code only when
  `begin()` itself fails; an unacknowledged write is retried as before and
  reported once per setup as `gnss_config_retry` (severity 1, `msg` names
  the step). The recovery ladder's `gnss_degraded` message names the
  failing step too.
- The association backoff no longer outlasts the hotspot coming back. The ladder
  added in 0.46.1 doubled on elapsed time alone, so a unit could sit out a 60 s
  cooldown with the hotspot already up and visible. Any change in
  `WiFi.status()` now resets the ladder and retries at once: a changed status
  means the radio picture changed. Backoff still applies while the status sits
  unchanged, which is the case it was for.

- `WL_CONNECT_FAILED` escalates to a full re-init after 30 s
  (`WIFI_REINIT_AFTER_FAILED_MS`) instead of waiting out the 300 s wedged-driver
  timeout. arduino-esp32 latches that status and stops its own auto-reconnect
  for auth-class disconnect reasons, and `WiFi.reconnect()` only re-issues
  `esp_wifi_connect()` into the same wedged config, so the soft path cannot
  clear it. A status that merely passes *through* 4 while an iOS hotspot wakes
  up (bench-4 oscillated 4↔6) does not trigger this; only a constant one does.

- The nudge log line printed the delay that had just elapsed, labelled as the
  next one. It now prints the real next delay plus free heap at the attempt.

### Known issue

- Association transiently costs ~10–13 kB of heap, and since the reorder it runs
  at ~15 kB free instead of ~41 kB. Min-ever free heap reached **1836 B** during
  association during testing. Association itself succeeds, but the margin is
  thin enough that a concurrent Bluedroid GATT connection allocation is the
  known connect-time OOM panic path.

[0.46.1] - 2026-09-11

### Changed

- **BLE no longer waits for WiFi** (`setup()` in `main.cpp`). `setupBLE()` used to
  sit behind an unbounded `while (!WiFi.isConnected())`, so a unit powered on
  before its phone's hotspot didn't call `BLEDevice::init()`: the app
  had nothing to discover and the
  wearer had to bring the hotspot up first, then open RWA Player. No more waiting:
  BLE advertises within a second of boot and `setupWiFi()` makes one
  bounded attempt (10 s) before setup continues regardless of the result. Any
  power-on order now works.

  The second reconnect loop this removes was redundant, not load-bearing. WiFi
  has exactly one consumer, `task_rtk_get_corrrection_data`, and that task
  already owned the identical nudge/re-init ladder for a hotspot that is missing
  or lost.

  This also un-hides the firmware's own diagnostics: `reset_brownout` and the
  `i2c_*` events wait in the telemetry ring for a BLE central, so under the old
  order exactly the boots that died before associating could never report why.
  Field brownout counts before this change are a lower bound.

  BLE and WiFi radio-on are separated by `RADIO_START_STAGGER_MS` (300 ms): both
  PHY inits pull a current step from the same AP2112K 3V3 rail, shared with the
  ZED-F9P and BNO080. `setupWiFi()` still runs ahead of the sensor tasks, so the
  association burst keeps the relatively quiet radio it has always had.

- **Association-attempt backoff** (`WIFI_RECONNECT_NUDGE_MAX_MS`, 60 s). The
  NTRIP task's outage loop nudged `WiFi.reconnect()` at a fixed 10 s forever;
  each nudge is a full-power association burst. Now the gap doubles per
  unanswered nudge up to the cap, resetting when the outage ends or the driver
  is re-inited. This matters more than it used to: the blocking boot loop was
  incidentally acting as a power limiter for a unit with no hotspot, and
  removing it hands that unit's idle time to this loop instead.

- **`wifi_disconnected` now waits 30 s** (`WIFI_LOSS_REPORT_AFTER_MS`) before
  emitting. With the boot no longer blocking, a power-on before the hotspot
  exists reaches the same outage path, and reporting on entry would fire the
  code on nearly every startup — useless as an alert dimension. Real outages
  outlast the grace. Same code and severity, no schema change; PROJECT-PLAN.md
  §4.3's description updated.

[0.46.0] - 2026-08-28

### Changed

- Binary head-orientation frames (breaking): heading now streams as a 16-byte
  quaternion frame on a new characteristic (`713D0005-...`) instead of ASCII
  integer degrees on `713D0002`: floating point precision plus a frame counter
  and device timestamp for drop/latency monitoring. The ASCII characteristic is
  gone from this firmware (it remains the plain RWAHT headtracker's format).
  Needs the matching rwa-player / rwa-creator decoders: fleet firmware and apps
  ship together. Spec: PROJECT-PLAN.md §5.5.

- Head tracking samples fresh at the sensor rate: the IMU FIFO is drained to the
  newest report every 10 ms tick. Previously one report was consumed per 12 ms
  (and a second one read and thrown away), so the sensor's queue backed up and
  delivered stale orientation: measured ~34 Hz stale, now 75 Hz fresh.
  Supporting changes: head-tracking task at highest priority, IMU I2C at 400
  kHz, unused raw-accelerometer report disabled.

- No BLE connection-interval request, decided by measurement: asking the phone
  for 15–30 ms starved WiFi through radio coexistence and killed the NTRIP
  stream. The interval stays the iOS central's choice; `setupBLE()` documents
  the edge.

### Added

- Hotspot path warmer: while WiFi is associated but the caster is
  disconnected, a minimal DNS query every 15 s keeps the phone hotspot's
  cellular path awake. iOS might let it doze during the reconnect backoff's quiet
  phases, which might fail the next connect, pushing the backoff furher
  (self-sustaining outage).
- Per-second IMU poll/consume statistics in debug builds (backlog, misses,
  I2C read cost), instrumentation for verification of the latency fixes.

### Fixed

- A single missed IMU poll no longer freezes head tracking for a full second.

## [0.45.1] - 2026-08-26

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
