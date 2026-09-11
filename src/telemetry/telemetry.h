/*******************************************************************************
 * @file telemetry.h
 * @brief Telemetry event emitters + frame queue (PROJECT-PLAN.md par. 4-5).
 *
 * Producers (GNSS / corrections / IMU tasks) call telemetryEmit*() — each call CBOR-
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
/// heapMin = lowest free heap since boot (esp_get_minimum_free_heap_size()).
/// The per-interval counters (telemetryNote*Loop, the RTCM byte count) are
/// read-and-reset internally by this call.
bool telemetryEmitHeartbeat(uint32_t freeHeap, uint32_t heapMin, uint32_t battMv);
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

/// Pipeline liveness counters (heartbeat keys 20/19): each task bumps its
/// counter at the top of every loop iteration; the heartbeat emitter reads
/// and resets them. A healthy 15 s interval shows ~150 for both; a collapse
/// to ~1 is the field signature of the 2026-08-21 slowdown.
void telemetryNoteCorrectionsLoop();
void telemetryNotePositionLoop();

/// RTCM bookkeeping (par. 4.3): called after each pushRawData into the
/// receiver; gnss_fix reads the age, the heartbeat the bytes per interval.
void telemetryNoteRtcmPushed(uint32_t numBytes);
uint32_t telemetryCorrAgeMs();       // 0xFFFFFFFF = never received

#endif /*** TELEMETRY_H ***/
