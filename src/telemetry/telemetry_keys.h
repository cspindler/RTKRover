/*******************************************************************************
 * @file telemetry_keys.h
 * @brief CBOR key table for the BLE telemetry feed.
 *
 * CROSS-REPO CONTRACT (PROJECT-PLAN.md par. 5.3). Mirrored in rwa-player as
 * TelemetryKeys.swift. Never renumber or reuse a retired key within a type;
 * new fields get new keys, new event types get the next free type value.
 ******************************************************************************/
#ifndef TELEMETRY_KEYS_H
#define TELEMETRY_KEYS_H

#include <stdint.h>

// Frame header: [u8 proto_version][u16 length LE][CBOR payload]
#define TELEMETRY_PROTO_VERSION       1

// Event types (common key 0)
enum TelemetryEventType : uint8_t {
  TELEM_TYPE_GNSS_FIX     = 1,
  TELEM_TYPE_HEARTBEAT    = 2,
  // 3 = ntrip_status: retired 0.48.0 (ADR-001), the app emits it as
  // source `phone`. Never reuse on the BLE leg.
  TELEM_TYPE_IMU_STATUS   = 4,
  TELEM_TYPE_ERROR        = 5,
};

// Common keys (every event)
enum TelemetryCommonKey : uint8_t {
  TELEM_KEY_TYPE     = 0,  // uint (TelemetryEventType)
  TELEM_KEY_SEQ      = 1,  // uint, monotonic per boot
  TELEM_KEY_T_DEV_MS = 2,  // uint, millis since boot
};

// gnss_fix (type 1)
enum TelemetryGnssFixKey : uint8_t {
  TELEM_GNSS_LAT         = 10,  // double, degrees
  TELEM_GNSS_LON         = 11,  // double, degrees
  TELEM_GNSS_HEIGHT_M    = 12,  // float, ellipsoidal
  TELEM_GNSS_FIX_TYPE    = 13,  // uint, UBX fixType
  TELEM_GNSS_CARR_SOLN   = 14,  // uint: 0 none, 1 RTK float, 2 RTK fixed
  TELEM_GNSS_H_ACC_MM    = 15,  // uint
  TELEM_GNSS_V_ACC_MM    = 16,  // uint
  TELEM_GNSS_NUM_SV      = 17,  // uint
  TELEM_GNSS_PDOP        = 18,  // float
  TELEM_GNSS_CORR_AGE_MS = 19,  // uint, 0xFFFFFFFF = never
};

// heartbeat (type 2)
enum TelemetryHeartbeatKey : uint8_t {
  TELEM_HB_UPTIME_MS       = 10,  // uint
  TELEM_HB_FREE_HEAP       = 11,  // uint
  // 12 = wifi_rssi, 13 = ntrip_connected: retired 0.48.0 (ADR-001), never reuse
  TELEM_HB_FW_VERSION      = 14,  // text
  TELEM_HB_DROPPED_FRAMES  = 15,  // uint, cumulative since boot
  TELEM_HB_BATT_MV         = 16,  // uint, LiPo pack millivolts (0 = unknown)
  TELEM_HB_HEAP_MIN        = 17,  // uint, lowest free heap since boot
  // 18 = loops_ntrip: retired 0.48.0 with the NTRIP task, never reuse
  TELEM_HB_LOOPS_POS       = 19,  // uint, position-task loop iterations since last heartbeat
  TELEM_HB_LOOPS_CORR      = 20,  // uint, corrections-task loop iterations since last heartbeat
};

// imu_status (type 4)
enum TelemetryImuStatusKey : uint8_t {
  TELEM_IMU_CALIB_STATUS   = 10,  // uint
  TELEM_IMU_REPORT_RATE_HZ = 11,  // float
  TELEM_IMU_RESETS         = 12,  // uint
};

// error (type 5)
enum TelemetryErrorKey : uint8_t {
  TELEM_ERR_SEVERITY = 10,  // uint: 1 warn, 2 error, 3 fatal
  TELEM_ERR_CODE     = 11,  // text, <= TELEMETRY_ERR_CODE_MAX bytes
  TELEM_ERR_MSG      = 12,  // text, <= TELEMETRY_ERR_MSG_MAX bytes
};

#define TELEMETRY_ERR_CODE_MAX  32
#define TELEMETRY_ERR_MSG_MAX   120  // BLE-leg cap (PROJECT-PLAN.md par. 5.2)

// CTRL characteristic commands (par. 5.4): [u8 cmd][args...]
enum TelemetryCtrlCommand : uint8_t {
  TELEM_CTRL_SET_VERBOSITY = 0x01,  // arg: u8 = min error severity emitted
  TELEM_CTRL_STATUS_DUMP   = 0x02,  // no args
};

#endif /*** TELEMETRY_KEYS_H ***/
