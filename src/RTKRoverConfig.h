#ifndef RTK_ROVER_CONFIG_H
#define RTK_ROVER_CONFIG_H
#include <Arduino.h>

// Configuration for the rtk-rover firmware. One line of rationale per
// constant; the story behind a value lives in CHANGELOG.md under the version
// given, not here. Cross-repo contracts (UUIDs, telemetry) are marked.

/*
=================================================================================
                                Firmware version
=================================================================================
*/
// Semver base; bump on feature/breaking changes. Heartbeats report
// FW_VERSION_BASE "+" <git hash> (composed in src/telemetry/telemetry.h from
// the build-time generated header).
#define FW_VERSION_BASE               "0.47.0"

/*
=================================================================================
                                Telemetry (PROJECT-PLAN.md par. 4-5)
=================================================================================
*/
#define TELEMETRY_RING_SIZE           4096  // bytes, drop-oldest. 8 KB left ~2 KB free heap
                                            // and OOM'd on BLE connect (CHANGELOG 0.44.0)
#define TELEMETRY_TICK_MS             100   // drain task period
#define TELEMETRY_MAX_NOTIFY_PER_TICK 2     // a backlog drain must never crowd heading notifies
#define TELEMETRY_HEARTBEAT_MS        15000 // PROJECT-PLAN.md par. 4.3
#define GNSS_PIPE_STALL_MS            5000  // updatePosition() hold or NTRIP iteration over
                                            // this -> sev-1 gnss_pipe_stall
#define GNSS_PIPE_STALL_GAP_MS        10000 // min spacing between stall events per site
#define TELEMETRY_NOTIFY_BUF          247   // one notification payload; effective cap MTU-3
#define TELEMETRY_MAX_FRAME           192   // largest frame incl. 3 B header (error worst case)

/*
=================================================================================
                                Serial settings
=================================================================================
*/
// Off in production; the featheresp32_debug env sets -DDEBUGGING=1. Never
// edit this to toggle logging.
#ifndef DEBUGGING
#define DEBUGGING 0
#endif
#define DBG \
  if (DEBUGGING) Serial

#if DEBUGGING
#define TESTING                             // AUnit suites run in setup()
#endif

#define BAUD                          115200

/*
=================================================================================
                                WiFi settings
=================================================================================
*/
#define DEVICE_TYPE                  "rtkrover"  // BLE-name prefix on placeholder builds
#define WIFI_TX_POWER                WIFI_POWER_11dBm  // 19.5 dBm TX spikes browned out the
                                                      // 3V3 rail on battery (CHANGELOG 0.44.2);
                                                      // the hotspot is the wearer's own phone

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
                                BLE settings (cross-repo contract)
=================================================================================
*/
#define SERVICE_UUID                            "713D0000-503E-4C75-BA94-3148F18D941E"
// ..0002 is RWAHT's legacy ASCII heading, ..0003 was TRACKERSERVICERX in the
// apps: neither may be reused (PROJECT-PLAN.md par. 5).
#define HEADTRACKER_BIN_CHARACTERISTIC_UUID     "713D0005-503E-4C75-BA94-3148F18D941E"
#define REALTIME_KINEMATICS_CHARACTERISTIC_UUID "713D0004-503E-4C75-BA94-3148F18D941E"
#define TELEMETRY_SERVICE_UUID                  "713D0100-503E-4C75-BA94-3148F18D941E"
#define TELEMETRY_TX_CHARACTERISTIC_UUID        "713D0101-503E-4C75-BA94-3148F18D941E"
#define TELEMETRY_CTRL_CHARACTERISTIC_UUID      "713D0102-503E-4C75-BA94-3148F18D941E"
#define DATA_STR_DELIMITER           " "        // 713D0004 line: "<lat> <latHp> <lon> <lonHp>"

// Heading notify pacing: one frame per BLE connection event, derived from the
// interval the central granted (block comment in main.cpp; CHANGELOG 0.46.2).
#define HEADING_NOTIFY_FALLBACK_MS    15   // until the stack reports the interval
#define HEADING_NOTIFY_MIN_MS         10   // never faster than the sensor tick
#define HEADING_NOTIFY_MAX_MS        120   // sanity bound; a long interval still gets a frame per event
#define BLE_TX_CONGESTION_MAX_MS     500   // a "congested" flag older than this counts as a missed clear

/*
=================================================================================
                                BNO080 settings
=================================================================================
*/
// Own I2C bus (Wire) so the IMU never waits behind the GNSS bus (Wire1).
#define BNO080_I2C_ADDR                 0x4B
#define BNO080_SDA_PIN                  23
#define BNO080_SCL_PIN                  22
#define I2C_FREQUENCY_400K              400000  // both buses
#define BNO080_ROT_VECT_UPDATE_RATE_MS  10      // 100 Hz ARVR-stabilized rotation vector
#define BNO080_LIN_ACCEL_UPDATE_RATE_MS 10      // 100 Hz
#define BNO080_DRAIN_MAX_REPORTS        8       // per-tick FIFO drain cap; steady state is
                                                // ~2 reports/tick (CHANGELOG 0.46.0)

/*
=================================================================================
                                FreeRTOS settings
=================================================================================
*/
#define RUNNING_CORE_0                                  0     // WiFi/lwIP core: NTRIP + position tasks
#define RUNNING_CORE_1                                  1     // heading, telemetry, loop()
// Priorities: a tie means round-robin time slicing, i.e. jitter on the
// higher-rate task. Head tracking strictly highest; telemetry strictly lowest
// (PROJECT-PLAN.md par. 5).
#define TASK_RTK_GET_CORR_DATA_PRIORITY                 2
#define TASK_RTK_GET_POSITION_PRIORITY                  2
#define TASK_BNO080_VIA_BLE_PRIORITY                    3
#define TASK_TELEMETRY_PRIORITY                         1
#define TASK_RTK_GET_POSITION_INTERVAL_MS             100  // position read + 713D0004 notify
#define TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS       10  // = sensor report rate, or the FIFO backs up
#define TASK_WIFI_RTK_DATA_INTERVAL_MS               1000  // NTRIP task iteration
#define MIN_ACCEPTABLE_ACCURACY_MM                   8000  // 713D0004 goes quiet above this (app falls
                                                           // back to internal GPS)
#define NAVIGATION_FREQUENCY_HZ                        10  // 20 Hz is GPS-only on the F9P and wedged
                                                           // the receiver (CHANGELOG 0.45.0)
#define CONNECTION_TIMEOUT_MS                       10000  // caster response wait after the request

/*
=================================================================================
                    GNSS receiver watchdog / recovery (CHANGELOG 0.45.0)
=================================================================================
*/
// GGA output (~1/s) is the liveness signal. Silence skips caster connects
// (a VRS streams nothing without our GGA) and runs the ladder: reconfigure ->
// GNSS software reset -> hard reset, one rung per gap.
#define GNSS_SILENT_AFTER_MS        30000  // no GGA for this long = receiver silent
#define GNSS_RECOVERY_GAP_MS        60000  // min spacing between recovery attempts
#define GNSS_RECOVERY_MUTEX_MS       5000  // mutex bound for the recovery I2C work

/*
=================================================================================
                          NTRIP link management (CHANGELOG 0.45.0)
=================================================================================
*/
#define NTRIP_RTCM_TIMEOUT_MS       10000  // steady state: hang up after this long without RTCM
#define NTRIP_CONNECT_GRACE_MS      30000  // first no-RTCM window after a connect (VRS spin-up)
#define NTRIP_BACKOFF_START_MS       5000  // reconnect delay after a failed/dataless attempt
#define NTRIP_BACKOFF_MAX_MS        60000  // cap of the doubling backoff; reset on received RTCM
#define NTRIP_DRAIN_MAX_BYTES       16384  // per-iteration socket drain cap (flooding-caster backstop)
#define NTRIP_GGA_FIX_MAX_AGE_MS    30000  // connect gate: a fix-quality GGA at most this old
#define GNSS_MUTEX_TIMEOUT_MS        2000  // NTRIP task's bounded mutex takes: skip the I2C, keep the link
#define GGA_MUTEX_TIMEOUT_MS          250  // GGA copy takes: a fresh GGA arrives every epoch

/*
=================================================================================
                          WiFi outage handling
=================================================================================
*/
// Soft WiFi.reconnect() nudges instead of a driver re-init per retry (which
// leaked ~48 B/cycle, CHANGELOG 0.45.0); the nudge cadence doubles so a unit
// whose phone is off does not burn its pack (0.46.1).
#define WIFI_RECONNECT_NUDGE_MS     10000  // first nudge cadence
#define WIFI_RECONNECT_NUDGE_MAX_MS 60000  // cap; reset on any WiFi.status() change
#define WIFI_REINIT_AFTER_MS       300000  // full driver re-init: wedged-driver escape hatch
#define WIFI_REINIT_AFTER_FAILED_MS 30000  // ...sooner while WL_CONNECT_FAILED (0.46.2)
#define WIFI_LOSS_REPORT_AFTER_MS   30000  // grace before wifi_disconnected: booting before the
                                           // hotspot is up is normal, not an alert (0.46.1)
#define RADIO_START_STAGGER_MS        300  // BT radio-on to WiFi radio-on: two PHY current
                                           // steps on one 3V3 rail (0.46.1)
#define HOTSPOT_WARM_INTERVAL_MS    15000  // DNS-query path warmer while caster-disconnected:
                                           // iOS hotspot dozes its cellular path when idle
                                           // (0.46.0)

/*
=================================================================================
                          Battery settings
=================================================================================
*/
// A13 = GPIO35 = ADC1_CH7 on the Huzzah32, behind a 2:1 divider off BAT.
// ADC1, so WiFi never blocks the read (src/battery.h).
#define BATTERY_ADC_PIN                      A13
#define BATTERY_DIVIDER_RATIO                2
#define BATTERY_ADC_SAMPLES                  8    // cheap noise average

#endif /*** RTK_ROVER_CONFIG_H ***/
