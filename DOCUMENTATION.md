# rtk-rover runtime architecture

What actually runs on the ESP32, from `src/main.cpp` and `src/telemetry/`.
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
- Cross-task scalars are `std::atomic` (telemetry module) or `volatile` (`bleConnected`,
  `lastGgaHeard_ms`, `lastFixGgaHeard_ms`, `ggaSentenceComplete`, BLE pacing state).

## Boot order (`setup()`)

1. LED, `batteryInit()`; debug builds wait for a key on serial.
2. `reportResetReason()`: brownout / panic / watchdog become `error` events waiting in the ring.
3. Debug builds run the AUnit suites here (100 passes), not in `loop()`.
4. `setupBLE()`: tracker service + telemetry service, advertising starts. BLE first.
5. `telemetryBleStartTask()`: started before the sensor setups so their failures are visible.
6. 300 ms radio stagger, then `setupWiFi()`: one 10 s bounded attempt, boot continues regardless.
7. `setupGNSS()`: retries forever until the ZED-F9P answers (emits `i2c_*` errors once).
8. Mutex, queue, the four tasks above.

The BNO080 is initialised inside the heading task after the first BLE connection, so
IMU faults become visible only once a phone connects (matches the `imu_status` contract).

## Callback contexts

- `callbackGPGGA` runs inside `myGNSS.checkCallbacks()` on the NTRIP task.
- BLE server callbacks, the custom GAP/GATTS handlers and the telemetry CTRL `onWrite`
  run on the Bluedroid BTC task: they only set atomics or volatiles and return.
