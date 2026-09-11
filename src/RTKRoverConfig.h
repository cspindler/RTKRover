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
#define GNSS_PIPE_STALL_MS            5000  // updatePosition() hold or corrections iteration over
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
#define DEVICE_TYPE                             "rtkrover"  // BLE-name prefix on placeholder builds
#define SERVICE_UUID                            "713D0000-503E-4C75-BA94-3148F18D941E"
// ..0002 is RWAHT's legacy ASCII heading, ..0003 was TRACKERSERVICERX in the
// apps: neither may be reused (PROJECT-PLAN.md par. 5).
#define HEADTRACKER_BIN_CHARACTERISTIC_UUID     "713D0005-503E-4C75-BA94-3148F18D941E"
#define REALTIME_KINEMATICS_CHARACTERISTIC_UUID "713D0004-503E-4C75-BA94-3148F18D941E"
#define RTCM_CHARACTERISTIC_UUID                "713D0006-503E-4C75-BA94-3148F18D941E"  // app -> assembly, write w/o response (ADR-001)
#define GGA_CHARACTERISTIC_UUID                 "713D0007-503E-4C75-BA94-3148F18D941E"  // assembly -> app, notify: the receiver's GGA for the caster
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
#define RUNNING_CORE_0                                  0     // the GNSS side: corrections + position tasks
#define RUNNING_CORE_1                                  1     // heading, telemetry, loop()
// Priorities: a tie means round-robin time slicing, i.e. jitter on the
// higher-rate task. Head tracking strictly highest; telemetry strictly lowest
// (PROJECT-PLAN.md par. 5).
#define TASK_GNSS_CORRECTIONS_PRIORITY                  2
#define TASK_RTK_GET_POSITION_PRIORITY                  2
#define TASK_BNO080_VIA_BLE_PRIORITY                    3
#define TASK_TELEMETRY_PRIORITY                         1
#define TASK_RTK_GET_POSITION_INTERVAL_MS             100  // position read + 713D0004 notify
#define TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS       10  // = sensor report rate, or the FIFO backs up
#define TASK_GNSS_CORRECTIONS_INTERVAL_MS             100  // RTCM FIFO drain, NMEA callbacks, receiver watchdog
#define MIN_ACCEPTABLE_ACCURACY_MM                   8000  // 713D0004 goes quiet above this (app falls
                                                           // back to internal GPS)
#define NAVIGATION_FREQUENCY_HZ                        10  // 20 Hz is GPS-only on the F9P and wedged
                                                           // the receiver (CHANGELOG 0.45.0)

/*
=================================================================================
                    GNSS receiver watchdog / recovery (CHANGELOG 0.45.0)
=================================================================================
*/
// GGA output (~1/s) is the liveness signal. Silence runs the ladder:
// reconfigure -> GNSS software reset -> hard reset, one rung per gap.
#define GNSS_SILENT_AFTER_MS        30000  // no GGA for this long = receiver silent
#define GNSS_RECOVERY_GAP_MS        60000  // min spacing between recovery attempts
#define GNSS_RECOVERY_MUTEX_MS       5000  // mutex bound for the recovery I2C work
#define GNSS_MUTEX_TIMEOUT_MS        2000  // corrections task's bounded mutex takes: skip the I2C, keep going

/*
=================================================================================
                Corrections over BLE (ADR-001, PROJECT-PLAN.md par. 5.6)
=================================================================================
*/
#define CORRECTIONS_RTCM_FIFO_SIZE   4096  // chunk FIFO, drop-oldest: ~3 VRS epochs, rides out a mutex stall
#define CORRECTIONS_RTCM_CHUNK_MAX    520  // one write payload: ATT MTU - 3 (517 on iOS), rounded up

/*
=================================================================================
                          Battery settings
=================================================================================
*/
// A13 = GPIO35 = ADC1_CH7 on the Huzzah32, behind a 2:1 divider off BAT
// (src/battery.h).
#define BATTERY_ADC_PIN                      A13
#define BATTERY_DIVIDER_RATIO                2
#define BATTERY_ADC_SAMPLES                  8    // cheap noise average

#endif /*** RTK_ROVER_CONFIG_H ***/
