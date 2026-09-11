#ifndef RTK_ROVER_CONFIG_H
#define RTK_ROVER_CONFIG_H
#include <Arduino.h>

// Deactivate brown out detection
#undef ESP_ERROR_CHECK
#define ESP_ERROR_CHECK(x)   do { esp_err_t rc = (x); if (rc != ESP_OK) { ESP_LOGE("err", "esp_err_t = %d", rc); assert(0 && #x);} } while(0);

/*
=================================================================================
                                Firmware version
=================================================================================
*/

// Semver base; bump on feature/breaking changes. The full FW_VERSION reported
// in telemetry heartbeats is FW_VERSION_BASE "+" <git hash>, composed in
// src/telemetry/telemetry.h from the build-time generated header.
#define FW_VERSION_BASE               "0.46.2"

/*
=================================================================================
                                Telemetry (PROJECT-PLAN.md par. 4-5)
=================================================================================
*/

#define TELEMETRY_RING_SIZE           4096  // bytes; drop-oldest on overflow.
                                            // 4 KB not 8: steady-state free heap
                                            // was ~2.3 KB with 8 KB (measured
                                            // 2026-07-29, OOM panic on BLE
                                            // connect). ~40 s backlog at 1 Hz
                                            // gnss_fix still fits.
#define TELEMETRY_TICK_MS             100   // drain task period
#define TELEMETRY_MAX_NOTIFY_PER_TICK 2     // pacing: caps telemetry at ~2 x
                                            // (MTU-3) bytes / tick so a backlog
                                            // drain can never crowd the
                                            // headtracker notifications
#define TELEMETRY_HEARTBEAT_MS        15000 // PROJECT-PLAN.md par. 4.3
#define GNSS_PIPE_STALL_MS            5000  // gnss_pipe_stall event: a pipeline
                                            // step (position-task checkUblox, or
                                            // one NTRIP-task iteration) exceeding
                                            // this emits a sev-1 error naming the
                                            // phase (2026-08-21 slowdown diagnosis)
#define GNSS_PIPE_STALL_GAP_MS        10000 // min spacing between stall events
                                            // per emit site (don't flood the ring
                                            // during a long episode)
#define TELEMETRY_NOTIFY_BUF          247   // upper bound for one notification
                                            // payload; effective cap is the
                                            // negotiated MTU-3 (iOS: ~182)
#define TELEMETRY_MAX_FRAME           192   // largest single frame incl. 3 B header
                                            // (error event worst case; fits MTU 185)

/*
=================================================================================
                                Serial settings
=================================================================================
*/

// Debug mode. Defaults to off (production); build the `featheresp32_debug` env
// or add -DDEBUGGING=1 to build_flags to turn serial logging on without editing
// this file.
#ifndef DEBUGGING
#define DEBUGGING 0
#endif
#define DBG \
  if (DEBUGGING) Serial

#if DEBUGGING
#define TESTING
#endif

#define BAUD                          115200

/*
=================================================================================
                                WiFi settings
=================================================================================
*/
#define DEVICE_TYPE                  "rtkrover"
// Cap radio TX power from the first radio-on. The default 19.5 dBm draws
// TX spikes big enough to brown out the 3V3 rail on battery power (boot
// loop until WiFi init, observed in field tests 2026-08). The hotspot is
// the wearer's own phone, so 11 dBm keeps ample link margin. Check
// heartbeat wifi_rssi before lowering further (WIFI_POWER_8_5dBm).
#define WIFI_TX_POWER                WIFI_POWER_11dBm

/*
=================================================================================
                                RTK settings
=================================================================================
*/
#define RTK_I2C_ADDR                0x42
#define RTK_SDA_PIN                 33
#define RTK_SCL_PIN                 32

/*
=================================================================================
                                BLE settings
=================================================================================
*/
#define SERVICE_UUID                            "713D0000-503E-4C75-BA94-3148F18D941E"
// Binary heading frame:
// ..0002 is the RWAHT firmware's legacy ASCII heading (this firmware no longer has it);
// ..0003 was historically assigned as TRACKERSERVICERX in the apps. Neither may be reused.
#define HEADTRACKER_BIN_CHARACTERISTIC_UUID     "713D0005-503E-4C75-BA94-3148F18D941E"
#define REALTIME_KINEMATICS_CHARACTERISTIC_UUID "713D0004-503E-4C75-BA94-3148F18D941E"
// Telemetry GATT service (PROJECT-PLAN.md par. 5.1) — cross-repo contract
#define TELEMETRY_SERVICE_UUID                  "713D0100-503E-4C75-BA94-3148F18D941E"
#define TELEMETRY_TX_CHARACTERISTIC_UUID        "713D0101-503E-4C75-BA94-3148F18D941E"
#define TELEMETRY_CTRL_CHARACTERISTIC_UUID      "713D0102-503E-4C75-BA94-3148F18D941E"
#define DATA_STR_DELIMITER           " "

// Heading notify pacing (see the block comment above bleGapHandler in main.cpp).
// The IMU is still drained every TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS; these
// bound only how often a frame is put on the wire. The working value is derived
// from the connection interval the central granted, one sensor tick short of it.
#define HEADING_NOTIFY_FALLBACK_MS    15   // until GAP reports the interval: fast
                                            // enough that a central which never
                                            // triggers the event costs no latency
#define HEADING_NOTIFY_MIN_MS         10   // never faster than the sensor tick
#define HEADING_NOTIFY_MAX_MS        120   // sanity bound only; a central asking
                                            // for a very long interval should get
                                            // one frame per event, not a stall
#define BLE_TX_CONGESTION_MAX_MS     500   // ignore a stuck "congested" flag after
                                            // this long: a missed CONGEST-cleared
                                            // event must not freeze head tracking

/*
=================================================================================
                                BNO080 settings
=================================================================================
*/
/*
INFO: The Qwiic VR IMU has onboard I2C pull up resistors; if multiple sensors are
connected to the bus with the pull-up resistors enabled, the parallel
equivalent resistance will create too strong of a pull-up for the bus to
operate correctly. As a general rule of thumb, disable all but one pair of
pull-up resistors if multiple devices are connected to the bus. If you need to
disconnect the pull up resistors they can be removed by removing the solder on
the corresponding jumpers labeled with "I2C" on the board.

BUT: we use here two I2C connections for real parallel computing on two cores.
*/
#define BNO080_I2C_ADDR                 0x4B
#define BNO080_SDA_PIN                  23
#define BNO080_SCL_PIN                  22
#define I2C_FREQUENCY_400K              400000  // 400 kHz
#define BNO080_ROT_VECT_UPDATE_RATE_MS  10      // Time between sensor readings
#define BNO080_LIN_ACCEL_UPDATE_RATE_MS 10      // 100 Hz
#define BNO080_DRAIN_MAX_REPORTS        8       // per-tick cap on the FIFO drain:
                                                // steady state is ~2 reports/tick
                                                // (rotation + lin accel @ 100 Hz);
                                                // the cap keeps a burst from
                                                // starving the notify cadence

/*
=================================================================================
                                FreeRTOS settings
=================================================================================
*/
#define RUNNING_CORE_0                                  0     // Low level WiFi code runs on core 0
#define RUNNING_CORE_1                                  1     // Use core 1 for all other tasks
// Each task is assigned a priority from 0 to ( configMAX_PRIORITIES - 1 ),
// where configMAX_PRIORITIES is defined within FreeRTOSConfig.h.
#define TASK_RTK_GET_CORR_DATA_PRIORITY                 2     // GNSS should have a lower priority than BNO080 data transmission
#define TASK_RTK_GET_POSITION_PRIORITY                  2
#define TASK_BNO080_VIA_BLE_PRIORITY                    3     // Headtracking: highest priority for believalbe binaural rendering
                                                              // (above the RTK tasks. A tie means round-robin time slicing,
                                                              // i.e. scheduling jitter on the notify cadence)
#define TASK_RTK_POSITION_VIA_BLE_PRIORITY              2     // Real Time Kinematics data to iOS app, (should not break head tracking)
#define TASK_TELEMETRY_PRIORITY                         1     // Lowest in the system: telemetry may starve, never compete (PROJECT-PLAN.md par. 5)
#define TASK_RTK_BLE_INTERVAL_MS                      100    // Send position to iPhone
#define TASK_RTK_GET_POSITION_INTERVAL_MS             100
#define TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS       10  // match BNO080_ROT_VECT_UPDATE_RATE_MS:
                                                           // a slower tick than the report rate
                                                           // grows the sensor-side FIFO backlog
#define TASK_WIFI_RTK_DATA_INTERVAL_MS               1000  //200 Get fresh correction data from caster
#define MIN_ACCEPTABLE_ACCURACY_MM                   8000  // Device will only send if accuray is better than this
#define NAVIGATION_FREQUENCY_HZ                        10  // Solution output rate. 20 Hz is beyond the
                                                           // F9P's multi-GNSS RTK spec (20 Hz is GPS-only)
                                                           // and is the prime suspect for the receiver
                                                           // wedging into a slow-I2C / mute state
                                                           // (2026-08-21/24). All consumers sample at
                                                           // <= 10 Hz anyway (100 ms task intervals).
#define CONNECTION_TIMEOUT_MS                       10000

/*
=================================================================================
                    GNSS receiver watchdog / recovery (2026-08-24)
=================================================================================
Bench 4.1: the ZED-F9P went fully mute (no output at all) and stayed dead for
14+ min with no self-recovery, while the firmware politely cycled dataless
caster sessions. The receiver's GGA output (~1/s) is the liveness signal: on
silence, caster connects are skipped (a VRS streams nothing without our GGA
anyway) and a recovery ladder kicks in: reconfigure -> GNSS software reset ->
full hard reset (cold start), one rung per gap interval.
*/
#define GNSS_SILENT_AFTER_MS        30000  // no GGA for this long = receiver silent
#define GNSS_RECOVERY_GAP_MS        60000  // min spacing between recovery attempts
#define GNSS_RECOVERY_MUTEX_MS       5000  // mutex take bound for recovery I2C work
                                           // (position-task holds stay < 5 s)

/*
=================================================================================
                          NTRIP link management (2026-08-24)
=================================================================================
Field capture 2026-08-24: the fixed 10 s no-RTCM hangup killed every fresh
session while the NTRIP task sat 12-26 s in portMAX_DELAY mutex waits, and the
resulting ~2 reconnects/min risk caster throttling. These constants implement
the post-connect grace window, reconnect backoff, and bounded mutex takes.
*/
#define NTRIP_RTCM_TIMEOUT_MS       10000  // steady-state: hang up after this long without RTCM
#define NTRIP_CONNECT_GRACE_MS      30000  // first no-RTCM window after a (re)connect:
                                           // VRS spin-up + GGA round-trip need longer
#define NTRIP_BACKOFF_START_MS       5000  // reconnect-attempt delay after a failure
#define NTRIP_BACKOFF_MAX_MS        60000  // cap; doubled per consecutive failed or
                                           // dataless attempt, reset on received RTCM
#define NTRIP_DRAIN_MAX_BYTES       16384  // per-iteration socket drain cap (backstop
                                           // against a flooding caster; ~16 s of stream)
#define NTRIP_GGA_FIX_MAX_AGE_MS    30000  // connect gate: require a GGA with a fix
                                           // (quality >= 1) at most this old - a VRS
                                           // can't use a fixless GGA, so connecting
                                           // without one only cycles dataless sessions
#define GNSS_MUTEX_TIMEOUT_MS        2000  // NTRIP task's bounded mutexSem takes: on
                                           // timeout skip the I2C work, keep the link
#define GGA_MUTEX_TIMEOUT_MS          250  // GGA copy/callback takes: a fresh GGA
                                           // arrives every epoch, skipping one is free

// WiFi outage handling (bench 2+4, 2026-08-24): a full setupStationMode()
// per retry (driver deinit/init every ~12 s) leaked ~48 B/cycle = ~14.5 kB/h
// and transiently dipped free heap by several kB per cycle — OOM after ~1 h
// of continuous hotspot loss. The wait loop now nudges with WiFi.reconnect()
// (no teardown; auto-reconnect keeps retrying between nudges) and escalates
// to one full re-init only after minutes without success.
#define WIFI_RECONNECT_NUDGE_MS     10000  // soft WiFi.reconnect() kick cadence
#define WIFI_REINIT_AFTER_MS       300000  // full driver re-init only after this
                                           // long without association (wedged-
                                           // driver escape hatch)

// Association-attempt backoff. Every nudge is a full-power association burst,
// and a unit powered on before its phone's hotspot used to repeat them at a
// fixed 10 s forever. That cadence is worth paying while the hotspot is
// probably coming back (a walk in a tunnel); it is pure current burned on a
// sagging pack when the phone is simply switched off. Doubles per unanswered
// nudge, resets when the outage ends or the driver is re-inited.
#define WIFI_RECONNECT_NUDGE_MAX_MS 60000  // cap for the doubling nudge cadence
                                           // (reset on any WiFi.status() change,
                                           // so a hotspot appearing is not
                                           // made to wait out a long cooldown)

// Escape hatch for WL_CONNECT_FAILED.
#define WIFI_REINIT_AFTER_FAILED_MS 30000

// Grace before an outage becomes a `wifi_disconnected` event. Since the boot
// no longer blocks on association (see setup()), the NTRIP task's outage loop
// is now also the path a unit takes when it boots before its hotspot exists —
// the normal case in the field. Reporting that instantly would turn a routine
// power-on into a fleet-wide severity-1 rate spike and make the code useless
// as an alert dimension. Real outages last longer than this.
#define WIFI_LOSS_REPORT_AFTER_MS   30000

// Gap between the BT controller's radio-on and WiFi's. Both PHY inits pull a
// current step from the same 3V3 rail (AP2112K, shared with the ZED-F9P and
// the BNO080); back-to-back they land on the same bulk capacitance.
#define RADIO_START_STAGGER_MS        300

// Hotspot path warmer (diagnosed 2026-08-28, rwa-hs-1): iOS Personal Hotspot
// idles its upstream cellular data session when clients go quiet. During a
// caster outage the reconnect backoff leaves 5-60 s quiet gaps, the hotspot
// dozes deeper, TCP SYNs to the caster then fail, and the backoff grows —
// a self-sustaining doom loop (with the path proven alive the same minute,
// the same unit streamed 150 RTCM msgs/180 s; while dozed, near-all connects
// failed at -51 dBm RSSI with caster and account healthy). While WiFi is
// associated and the caster is disconnected, a small DNS query of the caster
// host every interval keeps the hotspot NAT/cellular context awake (and
// pre-warms DNS). The RTCM stream itself keeps the path awake, so no warmer
// runs while connected — and the warmer never touches the caster: refnet
// throttles reconnect floods, the backoff etiquette must stay.
#define HOTSPOT_WARM_INTERVAL_MS    15000  // path-warmer cadence while caster-disconnected
/*
The module supports RTK update frequencies ranging from 8 Hz (BeiDou, Galileo, GLONASS, GPS) to
20 Hz (GPS only), velocity and dynamic heading accuracies of 0.05 m/s and 0.3° respectively and a
convergence time of less than 10 s. RTK performance is characterised by a circular error probable (CEP)
to 10 mm + 1 ppm. The F9 engine supports a total of 184-channels (GPS L1C/A L2C, GLO L1OF L2OF,
GAL E1B/C E5b, BDS B1I B2I, QZSS L1C/A L1S L2C and SBAS L1C/A).

Source: Broekman A, Gräbe PJ. A low-cost, mobile real-time kinematic geolocation service for engineering and
research applications. HardwareX. 2021 May 19;10:e00203. doi: 10.1016/j.ohx.2021.e00203. PMID: 35607668;
PMCID: PMC9123378. https://www.ncbi.nlm.nih.gov/pmc/articles/PMC9123378/
*/

/*
=================================================================================
                          Battery settings
=================================================================================
*/
// A13 = GPIO35 = ADC1_CH7 on the Huzzah32, behind a 2:1 divider off BAT.
// ADC1, so WiFi never blocks the read (see src/battery.h).
#define BATTERY_ADC_PIN                      A13
#define BATTERY_DIVIDER_RATIO                2
#define BATTERY_ADC_SAMPLES                  8    // cheap noise average

#endif /*** RTK_ROVER_CONFIG_H ***/
