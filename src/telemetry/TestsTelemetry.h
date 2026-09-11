/*******************************************************************************
 * @file TestsTelemetry.h
 * @brief AUnit tests: CBOR encoder, drop-oldest buffer, frame layout.
 *
 * Included from TestsRTKRover.h in TESTING builds only. Expected byte
 * sequences follow RFC 8949; the frame layout tests pin the wire contract
 * from PROJECT-PLAN.md par. 5.2-5.3.
 ******************************************************************************/
#ifndef TESTS_TELEMETRY_H
#define TESTS_TELEMETRY_H

#include <AUnit.h>

#include "telemetry/CborWriter.h"
#include "telemetry/TelemetryBuffer.h"
#include "telemetry/telemetry.h"

// --- helpers ---------------------------------------------------------------

static bool bytesEqual(const uint8_t *got, size_t gotLen,
                       const uint8_t *want, size_t wantLen)
{
  return gotLen == wantLen && memcmp(got, want, wantLen) == 0;
}

/// Decode one CBOR uint at p (immediate/1/2-byte forms only, enough for
/// test-range values). Returns bytes consumed, 0 on anything else.
static size_t cborReadUint(const uint8_t *p, uint32_t *out)
{
  if (*p < 0x18)
  {
    *out = *p;
    return 1;
  }
  if (*p == 0x18)
  {
    *out = p[1];
    return 2;
  }
  if (*p == 0x19)
  {
    *out = (uint32_t)(p[1] << 8) | p[2];
    return 3;
  }
  return 0;
}

static void drainTelemetry()
{
  uint8_t scratch[TELEMETRY_MAX_FRAME];
  while (telemetryPopFrame(scratch, sizeof(scratch)) > 0) {}
}

static bool containsBytes(const uint8_t *hay, size_t hayLen,
                          const uint8_t *needle, size_t needleLen)
{
  if (needleLen > hayLen) return false;
  for (size_t i = 0; i + needleLen <= hayLen; i++)
  {
    if (memcmp(hay + i, needle, needleLen) == 0) return true;
  }
  return false;
}

// --- CBOR encoder ----------------------------------------------------------

test(cbor_uint_boundaries)
{
  uint8_t buf[32];
  CborWriter w(buf, sizeof(buf));
  w.uintVal(0);
  w.uintVal(23);
  w.uintVal(24);
  w.uintVal(255);
  w.uintVal(256);
  w.uintVal(65535);
  w.uintVal(65536);
  const uint8_t want[] = {0x00, 0x17, 0x18, 0x18, 0x18, 0xFF, 0x19, 0x01,
                          0x00, 0x19, 0xFF, 0xFF, 0x1A, 0x00, 0x01, 0x00,
                          0x00};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_uint_64bit)
{
  uint8_t buf[16];
  CborWriter w(buf, sizeof(buf));
  w.uintVal(0x100000000ULL);
  const uint8_t want[] = {0x1B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_negative_int)
{
  uint8_t buf[16];
  CborWriter w(buf, sizeof(buf));
  w.intVal(-1);
  w.intVal(-500);
  w.intVal(5);
  const uint8_t want[] = {0x20, 0x39, 0x01, 0xF3, 0x05};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_bool)
{
  uint8_t buf[4];
  CborWriter w(buf, sizeof(buf));
  w.boolVal(false);
  w.boolVal(true);
  const uint8_t want[] = {0xF4, 0xF5};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_float32)
{
  uint8_t buf[8];
  CborWriter w(buf, sizeof(buf));
  w.floatVal(1.5f);
  const uint8_t want[] = {0xFA, 0x3F, 0xC0, 0x00, 0x00};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_float64)
{
  uint8_t buf[16];
  CborWriter w(buf, sizeof(buf));
  w.doubleVal(1.1);
  const uint8_t want[] = {0xFB, 0x3F, 0xF1, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9A};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_text)
{
  uint8_t buf[16];
  CborWriter w(buf, sizeof(buf));
  w.textVal("abc", 32);
  w.textVal("abcdef", 3);  // maxLen truncates
  const uint8_t want[] = {0x63, 'a', 'b', 'c', 0x63, 'a', 'b', 'c'};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_map_header)
{
  uint8_t buf[8];
  CborWriter w(buf, sizeof(buf));
  w.mapHeader(3);
  w.mapHeader(13);
  w.mapHeader(24);
  const uint8_t want[] = {0xA3, 0xAD, 0xB8, 0x18};
  assertTrue(w.ok());
  assertTrue(bytesEqual(buf, w.length(), want, sizeof(want)));
}

test(cbor_overflow_latches)
{
  uint8_t buf[3];
  buf[2] = 0xEE;  // canary just past what a 2-byte write may touch
  CborWriter w(buf, 2);
  w.uintVal(0x12345);  // needs 5 bytes into cap 2
  assertFalse(w.ok());
  assertLessOrEqual(w.length(), (size_t)2);
  w.uintVal(1);  // latched: later writes must stay no-ops
  assertFalse(w.ok());
  assertEqual(buf[2], 0xEE);
}

// --- drop-oldest buffer ----------------------------------------------------

test(buffer_drop_oldest)
{
  uint8_t storage[64];
  TelemetryBuffer rb(storage, sizeof(storage));

  uint8_t frame[20];
  for (uint8_t tag = 1; tag <= 3; tag++)
  {
    memset(frame, tag, sizeof(frame));
    assertTrue(rb.push(frame, sizeof(frame)));
  }
  // 3 x 22 B > 64: the first frame must have been evicted, counted once.
  assertEqual(rb.droppedCount(), (uint32_t)1);

  uint8_t out[32];
  assertEqual(rb.pop(out, sizeof(out)), (size_t)20);
  assertEqual(out[0], 2);  // frame 1 evicted, oldest survivor is 2
  assertEqual(rb.pop(out, sizeof(out)), (size_t)20);
  assertEqual(out[0], 3);
  assertEqual(rb.pop(out, sizeof(out)), (size_t)0);
}

test(buffer_rejects_never_fitting_frame)
{
  uint8_t storage[64];
  TelemetryBuffer rb(storage, sizeof(storage));
  uint8_t frame[63];
  memset(frame, 0xAB, sizeof(frame));
  assertFalse(rb.push(frame, sizeof(frame)));  // 63 + 2 > 64
  assertEqual(rb.droppedCount(), (uint32_t)1);
  assertEqual(rb.bytesUsed(), (size_t)0);
}

test(buffer_discards_frame_larger_than_out)
{
  uint8_t storage[64];
  TelemetryBuffer rb(storage, sizeof(storage));
  uint8_t frame[20];
  memset(frame, 0xCD, sizeof(frame));
  assertTrue(rb.push(frame, sizeof(frame)));

  uint8_t out[10];
  assertEqual(rb.pop(out, sizeof(out)), (size_t)0);  // discarded, not wedged
  assertEqual(rb.droppedCount(), (uint32_t)1);
  assertEqual(rb.bytesUsed(), (size_t)0);
}

test(buffer_wraps_around)
{
  uint8_t storage[64];
  TelemetryBuffer rb(storage, sizeof(storage));
  uint8_t frame[24];
  uint8_t out[32];
  // Repeated push/pop walks head/tail across the wrap point several times;
  // payload integrity proves the wrapped copies are correct.
  for (uint8_t tag = 0; tag < 10; tag++)
  {
    memset(frame, tag, sizeof(frame));
    assertTrue(rb.push(frame, sizeof(frame)));
    assertEqual(rb.pop(out, sizeof(out)), sizeof(frame));
    assertEqual(out[0], tag);
    assertEqual(out[sizeof(frame) - 1], tag);
  }
  assertEqual(rb.droppedCount(), (uint32_t)0);
}

// --- frame layout (wire contract) ------------------------------------------

test(frame_gnss_fix_layout)
{
  drainTelemetry();
  TelemetryGnssFix fix = {};
  fix.lat = 47.3847;
  fix.lon = 8.5417;
  fix.numSv = 12;
  fix.corrAgeMs = 0xFFFFFFFF;
  assertTrue(telemetryEmitGnssFix(fix));

  uint8_t frame[TELEMETRY_MAX_FRAME];
  size_t n = telemetryPopFrame(frame, sizeof(frame));
  assertMore(n, (size_t)3);

  assertEqual(frame[0], TELEMETRY_PROTO_VERSION);
  uint16_t len = (uint16_t)(frame[1] | (frame[2] << 8));  // little-endian
  assertEqual((size_t)len, n - 3);
  assertEqual(frame[3], 0xAD);  // map, 13 pairs (3 common + 10 fields)
  assertEqual(frame[4], TELEM_KEY_TYPE);
  assertEqual(frame[5], TELEM_TYPE_GNSS_FIX);
  assertEqual(frame[6], TELEM_KEY_SEQ);
}

test(frame_seq_increments)
{
  drainTelemetry();
  TelemetryGnssFix fix = {};
  assertTrue(telemetryEmitGnssFix(fix));
  assertTrue(telemetryEmitGnssFix(fix));

  uint8_t a[TELEMETRY_MAX_FRAME], b[TELEMETRY_MAX_FRAME];
  assertMore(telemetryPopFrame(a, sizeof(a)), (size_t)0);
  assertMore(telemetryPopFrame(b, sizeof(b)), (size_t)0);

  // Common prefix: [3]=map [4]=key0 [5]=type [6]=key1 [7..]=seq uint
  uint32_t seqA = 0, seqB = 0;
  assertMore(cborReadUint(a + 7, &seqA), (size_t)0);
  assertMore(cborReadUint(b + 7, &seqB), (size_t)0);
  assertEqual(seqB, seqA + 1);
}

test(frame_heartbeat_carries_fw_version)
{
  drainTelemetry();
  assertTrue(telemetryEmitHeartbeat(123456, 9876, 3900));

  uint8_t frame[TELEMETRY_MAX_FRAME];
  size_t n = telemetryPopFrame(frame, sizeof(frame));
  assertMore(n, (size_t)3);
  assertEqual(frame[5], TELEM_TYPE_HEARTBEAT);

  // The embedded version string must appear in the payload: proves the
  // build-time git hash header made it into the frame.
  const char *needle = FW_VERSION_BASE "+";
  size_t nl = strlen(needle);
  bool found = false;
  for (size_t i = 3; i + nl <= n && !found; i++)
  {
    found = memcmp(frame + i, needle, nl) == 0;
  }
  assertTrue(found);
}

test(frame_heartbeat_carries_batt_mv)
{
  drainTelemetry();
  assertTrue(telemetryEmitHeartbeat(123456, 9876, 3900));

  uint8_t frame[TELEMETRY_MAX_FRAME];
  size_t n = telemetryPopFrame(frame, sizeof(frame));
  assertMore(n, (size_t)4);

  // batt_mv is the last pair in the map: key 16, then 3900 as a 2-byte uint.
  const uint8_t want[] = {TELEM_HB_BATT_MV, 0x19, 0x0F, 0x3C};
  assertTrue(bytesEqual(frame + n - sizeof(want), sizeof(want), want,
                        sizeof(want)));
}

test(frame_heartbeat_carries_heap_min_and_interval_counters)
{
  // First heartbeat resets the interval counters (read-and-reset), then
  // discard it.
  telemetryEmitHeartbeat(123456, 9876, 3900);
  drainTelemetry();

  telemetryNoteCorrectionsLoop();
  telemetryNoteCorrectionsLoop();
  telemetryNoteCorrectionsLoop();
  telemetryNotePositionLoop();
  telemetryNotePositionLoop();
  telemetryNoteRtcmPushed(1000);
  telemetryNoteRtcmPushed(24);
  assertTrue(telemetryEmitHeartbeat(123456, 9876, 3900));

  uint8_t frame[TELEMETRY_MAX_FRAME];
  size_t n = telemetryPopFrame(frame, sizeof(frame));
  assertMore(n, (size_t)4);

  // heap_min: key 17, 9876 = 0x2694 as a 2-byte uint
  const uint8_t wantHeapMin[] = {TELEM_HB_HEAP_MIN, 0x19, 0x26, 0x94};
  assertTrue(containsBytes(frame, n, wantHeapMin, sizeof(wantHeapMin)));

  // Adjacent pairs: loops_pos (key 19) = 2, loops_corr (key 20) = 3,
  // rtcm_bytes (key 21) = 1024 = 0x0400 as a 2-byte uint
  const uint8_t wantCounters[] = {TELEM_HB_LOOPS_POS, 0x02,
                                  TELEM_HB_LOOPS_CORR, 0x03,
                                  TELEM_HB_RTCM_BYTES, 0x19, 0x04, 0x00};
  assertTrue(containsBytes(frame, n, wantCounters, sizeof(wantCounters)));

  // Read-and-reset: the next heartbeat reports zeros.
  assertTrue(telemetryEmitHeartbeat(123456, 9876, 3900));
  n = telemetryPopFrame(frame, sizeof(frame));
  assertMore(n, (size_t)4);
  const uint8_t wantZeros[] = {TELEM_HB_LOOPS_POS, 0x00,
                               TELEM_HB_LOOPS_CORR, 0x00,
                               TELEM_HB_RTCM_BYTES, 0x00};
  assertTrue(containsBytes(frame, n, wantZeros, sizeof(wantZeros)));
}

test(frame_error_msg_capped)
{
  drainTelemetry();
  char longMsg[200];
  memset(longMsg, 'x', sizeof(longMsg) - 1);
  longMsg[sizeof(longMsg) - 1] = '\0';
  assertTrue(telemetryEmitError(2, "test_code", longMsg));

  uint8_t frame[TELEMETRY_MAX_FRAME];
  size_t n = telemetryPopFrame(frame, sizeof(frame));
  assertMore(n, (size_t)3);
  assertEqual(frame[5], TELEM_TYPE_ERROR);
  // Worst-case event must stay within the single-frame budget.
  assertLessOrEqual(n, (size_t)TELEMETRY_MAX_FRAME);
}

#endif /*** TESTS_TELEMETRY_H ***/
