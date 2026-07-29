/*******************************************************************************
 * @file TelemetryBuffer.h
 * @brief Fixed-capacity FIFO of telemetry frames, drop-oldest on overflow.
 *
 * The head-tracking protection lives here (PROJECT-PLAN.md par. 5): producers
 * (GNSS / NTRIP / IMU tasks) must never block on telemetry. push() and pop()
 * only ever hold a portMUX critical section for a bounded memcpy of at most
 * TELEMETRY_MAX_FRAME bytes (< 1 us at 240 MHz) — no FreeRTOS locks a task
 * could sleep on, no allocation, no BLE calls. Safe from any task on either
 * core; single consumer (the telemetry drain task).
 *
 * Storage is caller-provided (static allocation, per CLAUDE.md constraint).
 * Internal layout: [u16 len LE][payload] per frame, wrapping byte ring.
 ******************************************************************************/
#ifndef TELEMETRY_BUFFER_H
#define TELEMETRY_BUFFER_H

#include <Arduino.h>

class TelemetryBuffer {
 public:
  TelemetryBuffer(uint8_t *storage, size_t capacity)
      : buf(storage), cap(capacity), head(0), tail(0), used(0), dropped(0) {}

  /// Append a frame, evicting oldest frames as needed. Returns false (and
  /// counts a drop) only if the frame can never fit. Never blocks.
  bool push(const uint8_t *frame, size_t len);

  /// Remove and copy out the oldest frame; returns its length, 0 if empty.
  /// A frame larger than outCap is discarded (counted) to avoid wedging.
  size_t pop(uint8_t *out, size_t outCap);

  size_t bytesUsed() const { return used; }
  uint32_t droppedCount() const { return dropped; }

 private:
  // Callers hold the critical section.
  void writeWrapped(size_t pos, const uint8_t *src, size_t n);
  void readWrapped(size_t pos, uint8_t *dst, size_t n) const;
  uint16_t frameLenAt(size_t pos) const;
  void evictOldest();

  uint8_t *buf;
  size_t cap;
  size_t head;  // next write position
  size_t tail;  // oldest frame position
  size_t used;  // bytes occupied (len prefixes included)
  uint32_t dropped;
  mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};

#endif /*** TELEMETRY_BUFFER_H ***/
