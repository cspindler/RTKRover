/*******************************************************************************
 * @file telemetry.h
 * @brief Telemetry event emitters + frame queue (PROJECT-PLAN.md par. 4-5).
 *
 * Producers (GNSS / NTRIP / IMU tasks) call telemetryEmit*() — each call CBOR-
 * encodes one event into a stack buffer and pushes the framed result into the
 * drop-oldest ring buffer. Emitters never block and never touch BLE; the
 * telemetry drain task pops frames via telemetryPopFrame() and notifies them
 * out at its own (lowest) priority.
 *
 * Frame layout on the wire: [u8 proto=1][u16 len LE][CBOR payload].
 ******************************************************************************/
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <Arduino.h>

#include "telemetry/telemetry_keys.h"
#include "telemetry/fw_version_autogen.h"
#include <RTKRoverConfig.h>

// Semver base + build-time git hash, reported in every heartbeat (par. 4.3).
#define FW_VERSION FW_VERSION_BASE "+" FW_GIT_HASH

struct TelemetryGnssFix
{
  double lat;           // degrees
  double lon;           // degrees
  float heightM;        // ellipsoidal
  uint8_t fixType;      // UBX fixType
  uint8_t carrSoln;     // 0 none, 1 RTK float, 2 RTK fixed
  uint32_t hAccMm;
  uint32_t vAccMm;
  uint8_t numSv;
  float pdop;
  uint32_t corrAgeMs;   // 0xFFFFFFFF = never received a correction
};

bool telemetryEmitGnssFix(const TelemetryGnssFix &fix);
bool telemetryEmitHeartbeat(uint32_t freeHeap, int wifiRssi, bool ntripConnected,
                            uint32_t battMv);
bool telemetryEmitNtripStatus(TelemetryNtripState state, uint32_t reconnects,
                              uint32_t bytesRx);
bool telemetryEmitImuStatus(uint8_t calibStatus, float reportRateHz,
                            uint32_t resets);
bool telemetryEmitError(uint8_t severity, const char *code, const char *msg);

/// Consumer side (drain task): oldest frame into out, returns length, 0 if
/// none. outCap must be >= TELEMETRY_MAX_FRAME.
size_t telemetryPopFrame(uint8_t *out, size_t outCap);

/// Cumulative frames dropped since boot (ring overflow / encode overflow);
/// reported in every heartbeat.
uint32_t telemetryDroppedFrames();

/// Last assigned seq = total events emitted since boot (diagnostics).
uint32_t telemetrySeqNow();

/// Verbosity = minimum error severity emitted (par. 5.4 CTRL 0x01).
/// Default 1: everything. telemetryEmitError() drops below-threshold events.
void telemetrySetVerbosity(uint8_t minSeverity);
uint8_t telemetryVerbosity();

/// NTRIP link state, mirrored by the NTRIP task each loop; consumed by the
/// heartbeat emitter (and by ntrip_status events, work-queue step 5).
void telemetrySetNtripConnected(bool connected);
bool telemetryNtripConnected();

/// RTCM bookkeeping (par. 4.3): the NTRIP task calls this after each
/// pushRawData; gnss_fix reads the age, ntrip_status the byte total.
void telemetryNoteRtcmPushed(uint32_t numBytes);
uint32_t telemetryCorrAgeMs();       // 0xFFFFFFFF = never received
uint32_t telemetryRtcmBytesTotal();

#endif /*** TELEMETRY_H ***/
