#include "telemetry/telemetry.h"

#include <atomic>

#include "telemetry/CborWriter.h"
#include "telemetry/TelemetryBuffer.h"

// Static allocation (CLAUDE.md constraint): the ring is the telemetry
// subsystem's entire RAM footprint besides per-call stack buffers.
static uint8_t ringStorage[TELEMETRY_RING_SIZE];
static TelemetryBuffer ringBuffer(ringStorage, sizeof(ringStorage));

// Monotonic per boot; gaps after drops are fine (and diagnostic) — the
// backend dedup key (device_id, session_id, seq) only needs uniqueness.
static std::atomic<uint32_t> seqCounter{0};

// Encode-overflow drops (distinct from ring evictions, which TelemetryBuffer
// counts itself). Both roll up into telemetryDroppedFrames().
static std::atomic<uint32_t> encodeDrops{0};

/**
 * @brief One telemetry frame under construction on the caller's stack.
 *
 * Writes the common prefix (type, seq, t_dev_ms) on construction; the caller
 * appends type-specific pairs and calls commit(), which patches the length
 * header and pushes into the ring. Total cost is CBOR encoding + one bounded
 * memcpy — no locks a producer can sleep on.
 */
struct FrameBuilder
{
  uint8_t frame[TELEMETRY_MAX_FRAME];
  CborWriter w;

  FrameBuilder(TelemetryEventType type, uint8_t typePairs)
      : w(frame + 3, sizeof(frame) - 3)
  {
    w.mapHeader(3 + typePairs);
    w.key(TELEM_KEY_TYPE);
    w.uintVal(type);
    w.key(TELEM_KEY_SEQ);
    w.uintVal(seqCounter.fetch_add(1, std::memory_order_relaxed) + 1);
    w.key(TELEM_KEY_T_DEV_MS);
    w.uintVal(millis());
  }

  bool commit()
  {
    if (!w.ok())
    {
      // A frame that exceeds TELEMETRY_MAX_FRAME is a firmware bug (the cap
      // is sized to the worst-case error event), but never propagate it.
      encodeDrops.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    size_t len = w.length();
    frame[0] = TELEMETRY_PROTO_VERSION;
    frame[1] = (uint8_t)(len & 0xFF);
    frame[2] = (uint8_t)(len >> 8);
    return ringBuffer.push(frame, 3 + len);
  }
};

bool telemetryEmitGnssFix(const TelemetryGnssFix &fix)
{
  FrameBuilder b(TELEM_TYPE_GNSS_FIX, 10);
  b.w.key(TELEM_GNSS_LAT);
  b.w.doubleVal(fix.lat);
  b.w.key(TELEM_GNSS_LON);
  b.w.doubleVal(fix.lon);
  b.w.key(TELEM_GNSS_HEIGHT_M);
  b.w.floatVal(fix.heightM);
  b.w.key(TELEM_GNSS_FIX_TYPE);
  b.w.uintVal(fix.fixType);
  b.w.key(TELEM_GNSS_CARR_SOLN);
  b.w.uintVal(fix.carrSoln);
  b.w.key(TELEM_GNSS_H_ACC_MM);
  b.w.uintVal(fix.hAccMm);
  b.w.key(TELEM_GNSS_V_ACC_MM);
  b.w.uintVal(fix.vAccMm);
  b.w.key(TELEM_GNSS_NUM_SV);
  b.w.uintVal(fix.numSv);
  b.w.key(TELEM_GNSS_PDOP);
  b.w.floatVal(fix.pdop);
  b.w.key(TELEM_GNSS_CORR_AGE_MS);
  b.w.uintVal(fix.corrAgeMs);
  return b.commit();
}

bool telemetryEmitHeartbeat(uint32_t freeHeap, int wifiRssi, bool ntripConnected)
{
  FrameBuilder b(TELEM_TYPE_HEARTBEAT, 6);
  b.w.key(TELEM_HB_UPTIME_MS);
  b.w.uintVal(millis());
  b.w.key(TELEM_HB_FREE_HEAP);
  b.w.uintVal(freeHeap);
  b.w.key(TELEM_HB_WIFI_RSSI);
  b.w.intVal(wifiRssi);
  b.w.key(TELEM_HB_NTRIP_CONNECTED);
  b.w.boolVal(ntripConnected);
  b.w.key(TELEM_HB_FW_VERSION);
  b.w.textVal(FW_VERSION, 32);
  b.w.key(TELEM_HB_DROPPED_FRAMES);
  b.w.uintVal(telemetryDroppedFrames());
  return b.commit();
}

bool telemetryEmitNtripStatus(TelemetryNtripState state, uint32_t reconnects,
                              uint32_t bytesRx)
{
  FrameBuilder b(TELEM_TYPE_NTRIP_STATUS, 3);
  b.w.key(TELEM_NTRIP_STATE);
  b.w.uintVal(state);
  b.w.key(TELEM_NTRIP_RECONNECTS);
  b.w.uintVal(reconnects);
  b.w.key(TELEM_NTRIP_BYTES_RX);
  b.w.uintVal(bytesRx);
  return b.commit();
}

bool telemetryEmitImuStatus(uint8_t calibStatus, float reportRateHz,
                            uint32_t resets)
{
  FrameBuilder b(TELEM_TYPE_IMU_STATUS, 3);
  b.w.key(TELEM_IMU_CALIB_STATUS);
  b.w.uintVal(calibStatus);
  b.w.key(TELEM_IMU_REPORT_RATE_HZ);
  b.w.floatVal(reportRateHz);
  b.w.key(TELEM_IMU_RESETS);
  b.w.uintVal(resets);
  return b.commit();
}

static std::atomic<uint8_t> verbosity{1};
static std::atomic<bool> ntripConnected{false};

void telemetrySetVerbosity(uint8_t minSeverity)
{
  verbosity.store(minSeverity, std::memory_order_relaxed);
}

uint8_t telemetryVerbosity()
{
  return verbosity.load(std::memory_order_relaxed);
}

void telemetrySetNtripConnected(bool connected)
{
  ntripConnected.store(connected, std::memory_order_relaxed);
}

bool telemetryNtripConnected()
{
  return ntripConnected.load(std::memory_order_relaxed);
}

bool telemetryEmitError(uint8_t severity, const char *code, const char *msg)
{
  if (severity < telemetryVerbosity()) return false;  // CTRL 0x01 gate
  FrameBuilder b(TELEM_TYPE_ERROR, 3);
  b.w.key(TELEM_ERR_SEVERITY);
  b.w.uintVal(severity);
  b.w.key(TELEM_ERR_CODE);
  b.w.textVal(code, TELEMETRY_ERR_CODE_MAX);
  b.w.key(TELEM_ERR_MSG);
  b.w.textVal(msg, TELEMETRY_ERR_MSG_MAX);
  return b.commit();
}

size_t telemetryPopFrame(uint8_t *out, size_t outCap)
{
  return ringBuffer.pop(out, outCap);
}

uint32_t telemetryDroppedFrames()
{
  return ringBuffer.droppedCount() +
         encodeDrops.load(std::memory_order_relaxed);
}
