/*******************************************************************************
 * @file CborWriter.h
 * @brief Minimal CBOR encoder (RFC 8949 subset) for telemetry frames.
 *
 * Supports exactly what the PROJECT-PLAN.md par. 5.3 key table needs: unsigned/
 * negative integers, bool, float32/float64, UTF-8 text and definite-length
 * maps. Writes into a caller-provided buffer; on overflow it stops writing,
 * latches ok() == false and never touches memory past the capacity.
 ******************************************************************************/
#ifndef TELEMETRY_CBOR_WRITER_H
#define TELEMETRY_CBOR_WRITER_H

#include <stddef.h>
#include <stdint.h>

class CborWriter {
 public:
  CborWriter(uint8_t *buffer, size_t capacity)
      : buf(buffer), cap(capacity), len(0), overflow(false) {}

  void mapHeader(uint8_t pairs);   // definite-length map
  void uintVal(uint64_t v);
  void intVal(int64_t v);
  void boolVal(bool v);
  void floatVal(float v);          // encoded as float32
  void doubleVal(double v);        // encoded as float64
  void textVal(const char *s, size_t maxLen);  // stops at NUL or maxLen

  void key(uint8_t k) { uintVal(k); }

  size_t length() const { return len; }
  bool ok() const { return !overflow; }

 private:
  void put(uint8_t b);
  void putBytes(const uint8_t *p, size_t n);
  void typeValue(uint8_t major, uint64_t v);

  uint8_t *buf;
  size_t cap;
  size_t len;
  bool overflow;
};

#endif /*** TELEMETRY_CBOR_WRITER_H ***/
