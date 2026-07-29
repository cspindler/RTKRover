#include "telemetry/CborWriter.h"

#include <string.h>

// CBOR major types (RFC 8949 par. 3.1), shifted into the high 3 bits.
static const uint8_t MAJOR_UINT = 0 << 5;
static const uint8_t MAJOR_NEGINT = 1 << 5;
static const uint8_t MAJOR_TEXT = 3 << 5;
static const uint8_t MAJOR_MAP = 5 << 5;
static const uint8_t SIMPLE_FALSE = 0xF4;
static const uint8_t SIMPLE_TRUE = 0xF5;
static const uint8_t FLOAT32 = 0xFA;
static const uint8_t FLOAT64 = 0xFB;

void CborWriter::put(uint8_t b)
{
  if (overflow || len >= cap)
  {
    overflow = true;
    return;
  }
  buf[len++] = b;
}

void CborWriter::putBytes(const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++) put(p[i]);
}

// Argument encoding shared by all major types: the shortest of the
// immediate/1/2/4/8-byte forms, big-endian (RFC 8949 par. 3).
void CborWriter::typeValue(uint8_t major, uint64_t v)
{
  if (v < 24)
  {
    put(major | (uint8_t)v);
  }
  else if (v <= 0xFF)
  {
    put(major | 24);
    put((uint8_t)v);
  }
  else if (v <= 0xFFFF)
  {
    put(major | 25);
    put((uint8_t)(v >> 8));
    put((uint8_t)v);
  }
  else if (v <= 0xFFFFFFFF)
  {
    put(major | 26);
    put((uint8_t)(v >> 24));
    put((uint8_t)(v >> 16));
    put((uint8_t)(v >> 8));
    put((uint8_t)v);
  }
  else
  {
    put(major | 27);
    for (int shift = 56; shift >= 0; shift -= 8) put((uint8_t)(v >> shift));
  }
}

void CborWriter::mapHeader(uint8_t pairs)
{
  typeValue(MAJOR_MAP, pairs);
}

void CborWriter::uintVal(uint64_t v)
{
  typeValue(MAJOR_UINT, v);
}

void CborWriter::intVal(int64_t v)
{
  if (v >= 0)
    typeValue(MAJOR_UINT, (uint64_t)v);
  else
    typeValue(MAJOR_NEGINT, (uint64_t)(-1 - v));
}

void CborWriter::boolVal(bool v)
{
  put(v ? SIMPLE_TRUE : SIMPLE_FALSE);
}

void CborWriter::floatVal(float v)
{
  uint32_t bits;
  memcpy(&bits, &v, sizeof(bits));
  put(FLOAT32);
  put((uint8_t)(bits >> 24));
  put((uint8_t)(bits >> 16));
  put((uint8_t)(bits >> 8));
  put((uint8_t)bits);
}

void CborWriter::doubleVal(double v)
{
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  put(FLOAT64);
  for (int shift = 56; shift >= 0; shift -= 8) put((uint8_t)(bits >> shift));
}

void CborWriter::textVal(const char *s, size_t maxLen)
{
  size_t n = strnlen(s, maxLen);
  typeValue(MAJOR_TEXT, n);
  putBytes((const uint8_t *)s, n);
}
