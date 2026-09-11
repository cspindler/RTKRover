#include "corrections.h"

#include <BLE2902.h>
#include <BLEDevice.h>

#include <RTKRoverConfig.h>
#include "ble_link.h"
#include "telemetry/TelemetryBuffer.h"

static BLECharacteristic *pGga = nullptr;

// RTCM chunk FIFO: static storage (CLAUDE.md), drop-oldest. The telemetry
// ring's class fits exactly: producer on the Bluedroid task that must never
// block, one consumer, a bounded portMUX critical section per push/pop, and
// eviction at chunk granularity so a stall costs whole old chunks rather than
// tearing a message in the middle of a newer one.
static uint8_t rtcmStorage[CORRECTIONS_RTCM_FIFO_SIZE];
static TelemetryBuffer rtcmFifo(rtcmStorage, sizeof(rtcmStorage));

/**
 * @brief RTCM write handler: Bluedroid BTC task context. Copy and return;
 * no I2C, no mutex, no logging (DOCUMENTATION.md, callback contexts).
 */
class RtcmWriteCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pRtcm)
  {
    size_t len = pRtcm->getLength();
    if (len == 0) return;
    rtcmFifo.push(pRtcm->getData(), len);
  }
};

void correctionsBleSetup(BLEService *pService)
{
  // Write without response is the intended path (no round trip per chunk);
  // a central that insists on a response gets one, same handler.
  BLECharacteristic *pRtcm = pService->createCharacteristic(
    RTCM_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_WRITE);
  pRtcm->setCallbacks(new RtcmWriteCallbacks());

  pGga = pService->createCharacteristic(GGA_CHARACTERISTIC_UUID,
                                        BLECharacteristic::PROPERTY_NOTIFY);
  pGga->addDescriptor(new BLE2902());
}

bool correctionsNotifyGga(const uint8_t *sentence, size_t len)
{
  // The app appends CRLF when it writes to the caster; keep the wire value
  // a bare sentence, like the 713D0004 line.
  while (len > 0 && (sentence[len - 1] == '\r' || sentence[len - 1] == '\n')) len--;
  if (len == 0 || pGga == nullptr || !bleLinkConnected()) return false;
  // Bluedroid silently truncates a notification longer than MTU-3. A GGA is
  // ~80 B, so this only bites before the MTU exchange (MTU 23).
  if (len > (size_t)(bleLinkMtu() - 3)) return false;
  pGga->setValue((uint8_t *)sentence, len);
  pGga->notify();
  return true;
}

bool correctionsRtcmQueued()
{
  return rtcmFifo.bytesUsed() > 0;
}

size_t correctionsPopRtcm(uint8_t *out, size_t outCap)
{
  return rtcmFifo.pop(out, outCap);
}

uint32_t correctionsRtcmDropped()
{
  return rtcmFifo.droppedCount();
}
