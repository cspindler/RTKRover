#include "telemetry/TelemetryBuffer.h"

#include <string.h>

void TelemetryBuffer::writeWrapped(size_t pos, const uint8_t *src, size_t n)
{
  size_t first = min(n, cap - pos);
  memcpy(buf + pos, src, first);
  if (n > first) memcpy(buf, src + first, n - first);
}

void TelemetryBuffer::readWrapped(size_t pos, uint8_t *dst, size_t n) const
{
  size_t first = min(n, cap - pos);
  memcpy(dst, buf + pos, first);
  if (n > first) memcpy(dst + first, buf, n - first);
}

uint16_t TelemetryBuffer::frameLenAt(size_t pos) const
{
  uint8_t lo = buf[pos];
  uint8_t hi = buf[(pos + 1) % cap];
  return (uint16_t)(lo | (hi << 8));
}

void TelemetryBuffer::evictOldest()
{
  uint16_t len = frameLenAt(tail);
  tail = (tail + 2 + len) % cap;
  used -= 2 + len;
  dropped++;
}

bool TelemetryBuffer::push(const uint8_t *frame, size_t len)
{
  if (len == 0 || len + 2 > cap || len > 0xFFFF)
  {
    portENTER_CRITICAL(&mux);
    dropped++;
    portEXIT_CRITICAL(&mux);
    return false;
  }

  uint8_t prefix[2] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};

  portENTER_CRITICAL(&mux);
  while (cap - used < len + 2) evictOldest();
  writeWrapped(head, prefix, 2);
  writeWrapped((head + 2) % cap, frame, len);
  head = (head + 2 + len) % cap;
  used += 2 + len;
  portEXIT_CRITICAL(&mux);
  return true;
}

size_t TelemetryBuffer::pop(uint8_t *out, size_t outCap)
{
  portENTER_CRITICAL(&mux);
  if (used == 0)
  {
    portEXIT_CRITICAL(&mux);
    return 0;
  }
  uint16_t len = frameLenAt(tail);
  if (len > outCap)
  {
    evictOldest();
    portEXIT_CRITICAL(&mux);
    return 0;
  }
  readWrapped((tail + 2) % cap, out, len);
  tail = (tail + 2 + len) % cap;
  used -= 2 + len;
  portEXIT_CRITICAL(&mux);
  return len;
}
