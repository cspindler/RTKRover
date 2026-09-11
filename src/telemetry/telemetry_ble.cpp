#include "telemetry/telemetry_ble.h"

#include <BLE2902.h>
#include <BLEDevice.h>

#include <atomic>

#include "battery.h"
#include "ble_link.h"
#include "telemetry/telemetry.h"

static BLECharacteristic *pTelemetryTx = nullptr;
static BLE2902 *pTxCccd = nullptr;
static TaskHandle_t hTelemetryTask = nullptr;

static std::atomic<bool> statusDumpRequested{false};

/**
 * @brief CTRL characteristic handler (par. 5.4).
 *
 * Runs in the BT stack context: parse, store atomics, return. No emitting,
 * no logging, no blocking here.
 */
class TelemetryCtrlCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCtrl)
  {
    std::string value = pCtrl->getValue();
    if (value.empty()) return;
    switch ((uint8_t)value[0])
    {
      case TELEM_CTRL_SET_VERBOSITY:
        if (value.length() >= 2) telemetrySetVerbosity((uint8_t)value[1]);
        break;
      case TELEM_CTRL_STATUS_DUMP:
        statusDumpRequested.store(true, std::memory_order_relaxed);
        break;
      default:
        break;  // unknown commands ignored (forward compatibility, par. 5.4)
    }
  }
};

void telemetryBleSetup(BLEServer *pServer)
{
  BLEService *pService = pServer->createService(TELEMETRY_SERVICE_UUID);

  pTelemetryTx = pService->createCharacteristic(
    TELEMETRY_TX_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCccd = new BLE2902();
  pTelemetryTx->addDescriptor(pTxCccd);

  BLECharacteristic *pCtrl = pService->createCharacteristic(
    TELEMETRY_CTRL_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCtrl->setCallbacks(new TelemetryCtrlCallbacks());

  pService->start();
  // Deliberately not added to the advertisement: the 31 B payload has no room
  // for a second 128-bit UUID. The app discovers the service after connecting.
}

void telemetryBleOnDisconnect()
{
  if (pTxCccd != nullptr) pTxCccd->setNotifications(false);
}

/**
 * @brief Fill one notification payload from the ring, byte-stream style.
 *
 * chunk holds the frame currently being streamed out; a frame that does not
 * fit the remaining space continues in the next notification (par. 5.2).
 */
static uint8_t chunk[TELEMETRY_MAX_FRAME];
static size_t chunkLen = 0;
static size_t chunkOff = 0;

static size_t fillNotification(uint8_t *out, size_t cap)
{
  size_t fill = 0;
  while (fill < cap)
  {
    if (chunkOff == chunkLen)
    {
      chunkLen = telemetryPopFrame(chunk, sizeof(chunk));
      chunkOff = 0;
      if (chunkLen == 0) break;
    }
    size_t take = min(cap - fill, chunkLen - chunkOff);
    memcpy(out + fill, chunk + chunkOff, take);
    fill += take;
    chunkOff += take;
  }
  return fill;
}

static void telemetryDrainTask(void *pvParameters)
{
  (void)pvParameters;

  static uint8_t notifBuf[TELEMETRY_NOTIFY_BUF];
  uint32_t lastGeneration = 0;
  // First heartbeat ~2 s after task start (system settled, early sign of life)
  uint32_t lastHeartbeat = millis() - TELEMETRY_HEARTBEAT_MS + 2000;

  TickType_t lastWake = xTaskGetTickCount();
  while (true)
  {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(TELEMETRY_TICK_MS));

    // Heartbeat is emitted into the ring regardless of the link: after a
    // reconnect the app receives the buffered recent history.
    bool dump = statusDumpRequested.exchange(false, std::memory_order_relaxed);
    if (dump || millis() - lastHeartbeat >= TELEMETRY_HEARTBEAT_MS)
    {
      lastHeartbeat = millis();
      // ADC1 read: microseconds (see src/battery.h).
      uint32_t battMv = batteryMilliVolts();
      telemetryEmitHeartbeat(esp_get_free_heap_size(),
                             esp_get_minimum_free_heap_size(), battMv);
      DBG.printf("telemetry: heartbeat%s, seq %u, dropped %u, batt %u mV\n",
                 dump ? " (status dump)" : "", telemetrySeqNow(),
                 telemetryDroppedFrames(), battMv);
    }

    if (!bleLinkConnected()) continue;
    if (pTxCccd == nullptr || !pTxCccd->getNotifications()) continue;

    uint32_t generation = bleLinkGeneration();
    if (generation != lastGeneration)
    {
      lastGeneration = generation;
      chunkLen = chunkOff = 0;  // never resume a half-sent frame on a new link
    }

    // Notifying more than mtu-3 bytes would be silently truncated by
    // Bluedroid and desync the stream; before the MTU exchange this caps
    // payloads at 20 B, which the byte-stream framing handles fine.
    size_t cap = min((size_t)(bleLinkMtu() - 3), sizeof(notifBuf));
    for (int i = 0; i < TELEMETRY_MAX_NOTIFY_PER_TICK; i++)
    {
      size_t fill = fillNotification(notifBuf, cap);
      if (fill == 0) break;
      pTelemetryTx->setValue(notifBuf, fill);
      pTelemetryTx->notify();
    }
  }
}

void telemetryBleStartTask()
{
  // 4 KB start value; right-size later from the debug watermark report
  // (same procedure as the 2026-07-29 task stack measurement).
  xTaskCreatePinnedToCore(&telemetryDrainTask, "task_telemetry_drain",
                          1024 * 4, NULL, TASK_TELEMETRY_PRIORITY,
                          &hTelemetryTask, RUNNING_CORE_1);
}

TaskHandle_t telemetryBleTaskHandle()
{
  return hTelemetryTask;
}
