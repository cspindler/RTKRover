# rtk-rover runtime architecture

What actually runs on the ESP32, from `src/main.cpp`, `src/ble_link.cpp` and
`src/telemetry/`.
Constants live in `src/RTKRoverConfig.h` (FreeRTOS section); keep this table in
step with them.

## Tasks

| Task | Core | Prio | Period | Stack | Does |
|---|---|---|---|---|---|
| `task_rtk_get_corrrection_data` | 0 | 2 | 1000 ms | 9 KB | WiFi association ladder, GNSS receiver watchdog and recovery ladder, NTRIP session: RTCM from the caster pushed to the ZED-F9P, GGA pushed to the caster every 10 s. |
| `task_rtk_get_rover_position` | 0 | 2 | 100 ms | 4 KB | `updatePosition()` under `mutexSem`: reads the streamed NAV packets, posts the coordinate to `xQueueCoord`, emits the 1 Hz `gnss_fix` event. |
| `task_bno_orientation_via_ble` | 1 | 3 | 10 ms | 4 KB | Drains the BNO080 FIFO to the newest report, puts one binary heading frame on `713D0005` per BLE connection event, emits `imu_status` every 60 s. Highest priority in the system. |
| `task_send_rtk_position_via_ble` | 1 | 2 | 100 ms | 4 KB | Pops `xQueueCoord`, notifies the ASCII position on `713D0004`. |
| `task_telemetry_drain` | 1 | 1 | 100 ms | 4 KB | Pops the telemetry ring into TX notifications (at most 2 per tick, MTU-3 bytes each), emits the 15 s `heartbeat`. Lowest priority: may starve, never competes. |
| Arduino `loop()` | 1 | 1 | – | – | Production: idle. Debug: AUnit runner plus a 10 s free-heap / stack-watermark report. |

Priorities: head tracking (3) above the two RTK tasks and the position notify (2)
above telemetry (1). A tie would time-slice and jitter the heading cadence.

## Synchronisation

- `mutexSem` (non-recursive) guards every I2C access to `myGNSS`. The position task
  takes it with `portMAX_DELAY`; the NTRIP task uses bounded takes and skips the I2C
  work on timeout so the link never stalls behind a slow bus. `callbackGPGGA` takes
  it too, so `myGNSS.checkCallbacks()` must never run while holding it.
- `xQueueCoord`, depth 2, latest position wins (a full queue drops its oldest entry).
- Telemetry ring (`TelemetryBuffer`, 4 KB, static): producers push under a `portMUX`
  critical section, drop-oldest on overflow; single consumer is the drain task.
- BLE link state (connected, connection generation, MTU, granted connection
  interval, TX congestion) has one owner, `src/ble_link.cpp`: `std::atomic`s written
  by the Bluedroid callbacks, read through `bleLink*()` by the heading, position
  and telemetry-drain tasks. It is also the only file that touches the raw
  Bluedroid API (custom GAP/GATTS handlers, `esp_ble_*` types).
- Other cross-task scalars are `std::atomic` (telemetry module) or `volatile`
  (`lastGgaHeard_ms`, `lastFixGgaHeard_ms`, `ggaSentenceComplete`).

## Boot order (`setup()`)

1. LED, `batteryInit()`; debug builds wait for a key on serial.
2. `reportResetReason()`: brownout / panic / watchdog become `error` events waiting in the ring.
3. Debug builds run the AUnit suites here (100 passes), not in `loop()`.
4. `setupBLE()`: `bleLinkBegin()` brings up Bluedroid, then the tracker service +
   telemetry service are created and `bleLinkStartAdvertising()` runs. BLE first.
5. `telemetryBleStartTask()`: started before the sensor setups so their failures are visible.
6. 300 ms radio stagger, then `setupWiFi()`: one 10 s bounded attempt, boot continues regardless.
7. `setupGNSS()`: retries forever until the ZED-F9P answers (emits `i2c_*` errors once).
8. Mutex, queue, the four tasks above.

The BNO080 is initialised inside the heading task after the first BLE connection, so
IMU faults become visible only once a phone connects (matches the `imu_status` contract).

## Callback contexts

- `callbackGPGGA` runs inside `myGNSS.checkCallbacks()` on the NTRIP task.
- The BLE server callbacks and custom GAP/GATTS handlers (`ble_link.cpp`) and the
  telemetry CTRL `onWrite` run on the Bluedroid BTC task: they only set atomics,
  toggle advertising and return.
