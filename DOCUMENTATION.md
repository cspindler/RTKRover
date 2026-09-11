# rtk-rover runtime architecture

What actually runs on the ESP32, from `src/main.cpp`, `src/ble_link.cpp`,
`src/corrections.cpp` and `src/telemetry/`.
Constants live in `src/RTKRoverConfig.h` (FreeRTOS section); keep this table in
step with them.

BLE is the only radio (ADR-001, 0.48.0): the phone is the NTRIP client and
proxies the correction loop over two characteristics (PROJECT-PLAN.md par. 5.6).

## Tasks

| Task | Core | Prio | Period | Stack | Does |
|---|---|---|---|---|---|
| `task_gnss_corrections` | 0 | 2 | 100 ms | 8 KB | Per iteration: `myGNSS.checkCallbacks()` (dispatches `callbackGPGGA`, which stamps receiver liveness and notifies fix-quality GGA sentences on `713D0007`), `gnssRecoveryTick()` (receiver watchdog + recovery ladder), then the RTCM downlink: every chunk the `713D0006` write callback queued is pushed to the ZED-F9P under one bounded `mutexSem` take; evicted chunks become `rtcm_fifo_overflow`. |
| `task_rtk_get_rover_position` | 0 | 2 | 100 ms | 4 KB | `updatePosition()` under `mutexSem`: reads the streamed NAV packets, emits the 1 Hz `gnss_fix` event; then, mutex released, notifies the ASCII position on `713D0004` when a trusted coordinate came back and a central is connected. |
| `task_bno_orientation_via_ble` | 1 | 3 | 10 ms | 4 KB | Drains the BNO080 FIFO to the newest report, puts one binary heading frame on `713D0005` per BLE connection event (at the 15 ms grant: one per sensor tick), emits `imu_status` every 60 s. Highest priority in the system. |
| `task_telemetry_drain` | 1 | 1 | 100 ms | 4 KB | Pops the telemetry ring into TX notifications (at most 2 per tick, MTU-3 bytes each), emits the 15 s `heartbeat`. Lowest priority: may starve, never competes. |
| Arduino `loop()` | 1 | 1 | 100–200 ms | – | Blinks the "no phone" code while no BLE central is connected, otherwise idle. Debug: AUnit runner plus a 10 s free-heap / stack-watermark report. |

Priorities: head tracking (3) above the two GNSS tasks (2) above telemetry (1). A tie would time-slice and jitter the heading cadence.

## Synchronisation

- `mutexSem` (non-recursive) guards every I2C access to `myGNSS`. The position task
  takes it with `portMAX_DELAY`; the corrections task uses bounded takes and skips
  the I2C work on timeout (queued RTCM waits in its FIFO), so nothing stalls behind
  a slow bus. `myGNSS.checkCallbacks()` runs outside it; the callbacks do no I2C.
- Telemetry ring (`TelemetryBuffer`, 4 KB, static): producers push under a `portMUX`
  critical section, drop-oldest on overflow; single consumer is the drain task.
- RTCM FIFO (`corrections.cpp`, the same `TelemetryBuffer` class, 4 KB, static):
  producer is the `713D0006` write callback on the Bluedroid task, consumer the
  corrections task; drop-oldest at chunk granularity.
- BLE link state (connected, connection generation, MTU, granted connection
  interval, TX congestion) has one owner, `src/ble_link.cpp`: `std::atomic`s written
  by the Bluedroid callbacks, read through `bleLink*()` by the heading, position,
  corrections (GGA notify) and telemetry-drain tasks. It is also the only file that
  touches the raw Bluedroid API (custom GAP/GATTS handlers, `esp_ble_*` types, the
  connection-parameter request on connect).
- Other cross-task scalars are `std::atomic` (telemetry module) or `volatile`
  (`lastGgaHeard_ms`).

## Boot order (`setup()`)

1. LED, `batteryInit()`; debug builds wait for a key on serial.
2. `reportResetReason()`: brownout / panic / watchdog become `error` events waiting in the ring.
3. Debug builds run the AUnit suites here (100 passes), not in `loop()`.
4. `setupBLE()`: `bleLinkBegin()` brings up Bluedroid, then the tracker service
   (heading, position, RTCM write, GGA notify) + telemetry service are created and
   `bleLinkStartAdvertising()` runs. The phone connects within ~1 s when RWA Player
   is open; `ble_link` then requests the 15–30 ms connection interval.
5. `telemetryBleStartTask()`: started before the sensor setup so its failures are visible.
6. `setupGNSS()`: retries forever until the ZED-F9P answers (emits `i2c_*` errors once).
7. Mutex, the three tasks above.

The BNO080 is initialised inside the heading task after the first BLE connection, so
IMU faults become visible only once a phone connects (matches the `imu_status` contract).

## Callback contexts

- `callbackGPGGA` runs inside `myGNSS.checkCallbacks()` on the corrections task
  (directly and via `gnssCheckUbloxLocked()`); it may `notify()` (a post to the BT
  task) but takes no mutex.
- The BLE server callbacks and custom GAP/GATTS handlers (`ble_link.cpp`), the
  telemetry CTRL `onWrite` and the RTCM `onWrite` (`corrections.cpp`) run on the
  Bluedroid BTC task: they only set atomics, copy into a FIFO, toggle advertising
  and return.
