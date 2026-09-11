#include "ble_link.h"

#include <BLEDevice.h>
#include <atomic>

#include <RTKRoverConfig.h>
#include "telemetry/telemetry_ble.h"

static BLEServer *pServer = nullptr;

static std::atomic<bool> connected{false};
static std::atomic<uint32_t> generation{0};
static std::atomic<uint16_t> mtu{23};
// Negotiated connection interval in 1.25 ms units; 0 until reported.
static std::atomic<uint16_t> connIntervalUnits{0};
// Set while the GATT server's TX queue is full, with the time it latched.
static std::atomic<bool> txCongested{false};
static std::atomic<uint32_t> txCongestedSince_ms{0};

// Heap diagnostics (debug builds): connect time is the critical moment.
// Bluedroid allocates the GATT connection control block on the BT task, and
// an allocation failure there escalates to a vQueueDelete(NULL) panic
// (fixed_queue_new error path, observed 2026-07-29). Track the margin.
static void logHeap(const char *where)
{
  DBG.printf("heap @ %s: free %u, min ever %u\n",
             where, esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
}

static void logInterval(const char *what, uint16_t units, uint16_t latency)
{
  DBG.printf("BLE %s: interval %u units (%u.%02u ms), latency %u\n",
             what, units, (units * 5u) / 4u, ((units * 5u) % 4u) * 25u, latency);
}

// Ask the central for the fastest interval Apple allows a non-HID peripheral
// (ADR-001 par. 5). The advertised preference is a hint iOS accepts or
// ignores at connect; this is the request it actually answers, with the
// grant arriving in ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT (gapHandler), which
// the heading pacing then follows. Until 0.47 this request was forbidden:
// 15-30 ms starved WiFi through radio coex and killed the NTRIP stream
// (0 RTCM in 300 s, A/B/A on rwa-hs-1, 2026-08-27). WiFi is gone.
static void requestConnInterval(const esp_bd_addr_t remote_bda)
{
  esp_ble_conn_update_params_t req = {};
  memcpy(req.bda, remote_bda, sizeof(esp_bd_addr_t));
  req.min_int = BLE_CONN_INTERVAL_MIN_UNITS;
  req.max_int = BLE_CONN_INTERVAL_MAX_UNITS;
  req.latency = 0;
  req.timeout = BLE_CONN_SUPERVISION_TIMEOUT;
  esp_err_t err = esp_ble_gap_update_conn_params(&req);
  DBG.printf("BLE conn params requested: %u-%u units -> %s\n",
             req.min_int, req.max_int, err == ESP_OK ? "queued" : "failed");
}

class LinkServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *)
  {
    // Fresh link: MTU back to the default until the central exchanges, and a
    // new generation before readers may see "connected".
    mtu.store(23, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_relaxed);
    connected.store(true, std::memory_order_release);
    BLEDevice::stopAdvertising();
    logHeap("ble_connect");
  }

  void onDisconnect(BLEServer *)
  {
    connected.store(false, std::memory_order_release);
    // Don't carry this link's pacing state into the next central: a new one
    // negotiates its own interval, and a congestion flag latched as the link
    // dropped would otherwise stall the first frames after reconnect.
    connIntervalUnits.store(0, std::memory_order_relaxed);
    txCongested.store(false, std::memory_order_relaxed);
    telemetryBleOnDisconnect();  // the TX CCCD must not survive the link
    BLEDevice::startAdvertising();
    logHeap("ble_disconnect");
  }

  void onMtuChanged(BLEServer *, esp_ble_gatts_cb_param_t *param)
  {
    mtu.store(param->mtu.mtu, std::memory_order_relaxed);
    DBG.printf("BLE MTU changed: %u\n", param->mtu.mtu);
  }
};

// Learn the connection interval the central actually granted (a later
// renegotiation; the initial one arrives with ESP_GATTS_CONNECT_EVT below).
static void gapHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
  if (event == ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT)
  {
    // status != 0 means the central declined; the interval reported is then
    // the one still in force, which is what the pacing needs either way.
    connIntervalUnits.store(param->update_conn_params.conn_int, std::memory_order_relaxed);
    logInterval(param->update_conn_params.status == ESP_BT_STATUS_SUCCESS
                  ? "conn params granted" : "conn params declined",
                param->update_conn_params.conn_int,
                param->update_conn_params.latency);
  }
}

// Backpressure from the GATT server's TX queue. Without it a full queue shows
// up only as a storm of `esp_ble_gatts_send_notify: rc=-1` errors and silently
// dropped frames.
static void gattsHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                         esp_ble_gatts_cb_param_t *param)
{
  (void)gatts_if;
  if (event == ESP_GATTS_CONGEST_EVT)
  {
    if (param->congest.congested) txCongestedSince_ms.store(millis(), std::memory_order_relaxed);
    txCongested.store(param->congest.congested, std::memory_order_relaxed);
  }
  else if (event == ESP_GATTS_CONNECT_EVT)
  {
    // The interval in force at connection setup. This is the source that
    // actually fires with iOS: testing showed ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT
    // not arriving (the central accepted the advertised preference and ran no
    // update procedure), leaving the pacing on its fallback.
    connIntervalUnits.store(param->connect.conn_params.interval, std::memory_order_relaxed);
    logInterval("connect", param->connect.conn_params.interval,
                param->connect.conn_params.latency);
    requestConnInterval(param->connect.remote_bda);
  }
}

BLEServer *bleLinkBegin(const char *deviceName)
{
  // Custom handlers must be installed before init(): the library forwards
  // every GAP/GATTS event to them after its own dispatch.
  BLEDevice::setCustomGapHandler(gapHandler);
  BLEDevice::setCustomGattsHandler(gattsHandler);
  BLEDevice::init(deviceName);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new LinkServerCallbacks());
  return pServer;
}

void bleLinkStartAdvertising(const char *serviceUuid)
{
  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->addServiceUUID(serviceUuid);
  pAdvertising->setScanResponse(true);
  // Advertised connection-interval preference, units of 1.25 ms. Only a hint
  // iOS may take at connect; requestConnInterval() asks for the same range
  // once the link is up.
  pAdvertising->setMinPreferred(BLE_CONN_INTERVAL_MIN_UNITS);
  pAdvertising->setMaxPreferred(BLE_CONN_INTERVAL_MAX_UNITS);
  BLEDevice::startAdvertising();
}

bool bleLinkConnected()
{
  return connected.load(std::memory_order_acquire);
}

uint32_t bleLinkGeneration()
{
  return generation.load(std::memory_order_relaxed);
}

uint16_t bleLinkMtu()
{
  return mtu.load(std::memory_order_relaxed);
}

uint16_t bleLinkConnIntervalUnits()
{
  return connIntervalUnits.load(std::memory_order_relaxed);
}

bool bleLinkTxCongested()
{
  if (!txCongested.load(std::memory_order_relaxed)) return false;
  if (millis() - txCongestedSince_ms.load(std::memory_order_relaxed) >= BLE_TX_CONGESTION_MAX_MS)
  {
    txCongested.store(false, std::memory_order_relaxed);  // assume a missed clear
    return false;
  }
  return true;
}
