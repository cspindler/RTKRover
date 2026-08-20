
/*******************************************************************************
 * @file main.cpp
 * @authors Markus Hädrich
 * <br>
 * @brief This is part of a distributed software, here: head tracker and GNSS
 *        positioning using Sparkfun Real Time Kinematics
 * <br>
 * @todo  - Upgrade to Sparkfun RTK Library v3
 *
 * @version 0.44.2
 * @date 2026-08-13
 ******************************************************************************/


#include <Arduino.h>
#include <Wire.h> // BNO080 and uBlox GNSS
#include <BLEDevice.h>
#include <BLE2902.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <SparkFun_BNO080_Arduino_Library.h>
#include <utility/imumaths.h>
#include <sdkconfig.h>
#include <esp_system.h> // esp_reset_reason()
#include <RTKRoverConfig.h>
#include <CasterSecrets.h>
#include <battery.h>
#include <handle_wifi.h>
#include <telemetry/telemetry.h>
#include <telemetry/telemetry_ble.h>
#include <TestsRTKRover.h>

/*
=================================================================================
                                Buttons
=================================================================================
*/
#include "Button2.h"

// Button to press to reboot the device
Button2 rebootButton = Button2(REBOOT_BUTTON_PIN, INPUT, false, false);

void buttonHandler(Button2 &btn);

/*
=================================================================================
                                Bluetooth LE
=================================================================================
*/
float bleConnected = false; // TODO: deglobalize this

// Fleet-configured BLE name (fleet-secrets.ini via CasterSecrets.h), empty on
// placeholder builds -> fall back to the chip-id name.
static String getBleName()
{
  if (kBleName[0] != '\0')
    return String(kBleName);
  return getDeviceName(DEVICE_TYPE);
}

class MyCharacteristicCallbacks: public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pHeadtrackerCharacteristic)
  {
    std::string value = pHeadtrackerCharacteristic->getValue(); // Here I get the commands from the App (client)

    if (value.length() > 0)
    {
      DBG.println(F("*********"));
      DBG.print(F("New value: "));
      for (int i = 0; i < value.length(); i++)
          DBG.print(value[i]);

      DBG.println();
      DBG.println(F("*********"));
    }
  }

  void onConnect(BLEServer* pServer)
  {
    bleConnected = true;
    DBG.print(F("bleConnected: "));
    DBG.println(bleConnected);
  };

  void onDisconnect(BLEServer* pServer)
  {
    bleConnected = false;
    DBG.print(("bleConnected: "));
    DBG.println(bleConnected);
  }
};

// Task handles, for the debug-build stack watermark report in loop().
static TaskHandle_t hTaskCorrData = NULL;
static TaskHandle_t hTaskPosition = NULL;
static TaskHandle_t hTaskBnoBle = NULL;
static TaskHandle_t hTaskRtkBle = NULL;

// Heap diagnostics (debug builds): connect time is the critical moment —
// Bluedroid allocates the GATT connection control block on the BT task, and
// an allocation failure there escalates to a vQueueDelete(NULL) panic
// (fixed_queue_new error path, observed 2026-07-29). Track the margin.
static void logFreeHeap(const char *where)
{
  DBG.printf("heap @ %s: free %u, min ever %u\n",
             where, esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
}

class MyServerCallbacks: public BLEServerCallbacks
{
    void onConnect(BLEServer* pServer)
    {
        bleConnected = true;
        BLEDevice::stopAdvertising();
        telemetryBleOnConnect();
        logFreeHeap("ble_connect");
    };

    void onDisconnect(BLEServer* pServer)
    {
        bleConnected = false;
        BLEDevice::startAdvertising();
        telemetryBleOnDisconnect();
        logFreeHeap("ble_disconnect");
    }

    void onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param)
    {
        // The telemetry drain must know the real MTU: notifying more than
        // mtu-3 bytes is silently truncated and would desync its stream.
        telemetryBleOnMtuChanged(param->mtu.mtu);
        DBG.printf("BLE MTU changed: %u\n", param->mtu.mtu);
    }
};

BLECharacteristic *pHeadtrackerCharacteristic;
BLECharacteristic *pRealtimeKinematicsCharacteristic;

void setupBLE(void);
/*
=================================================================================
                                BNO080
=================================================================================
*/
BNO080 bno080;
void setupBNO080(void);

/*
=================================================================================
                                GNSS
=================================================================================
*/
long lastTime = 0; //Simple local timer. Limits amount if I2C traffic to Ublox module.
bool beginPositioning = false;  // Wait with positioning for first correction data from caster

// The ESP32 core has a built in base64 library but not every platform does
// We'll use an external lib if necessary.
#if defined(ARDUINO_ARCH_ESP32)
#include "base64.h" //Built-in ESP32 library
#else
#include <Base64.h> //nfriendly library from https://github.com/adamvr/arduino-base64, will work with any platform
#endif


SFE_UBLOX_GNSS myGNSS;

/**
 * @brief Setup the ZED-F9D to a rover
 *
 * @return true If succeeded
 * @return false If failed
 */
bool setupGNSS(void);

/**
 * @brief Start the
 *
 */
void beginClient(void);

/**
 * @brief Get the Position
 *
 */
void updatePosition(void);

/*
=================================================================================
                                FreeRTOS
=================================================================================
*/
typedef struct Coord
{
  int32_t lat;
  int8_t  latHp;
  int32_t lon;
  int8_t  lonHp;
} coord_t;

const uint8_t QUEUE_SIZE = 2;
xQueueHandle xQueueCoord;
static xSemaphoreHandle mutexSem;

/**
 * @brief Task to get the correction data from the caster server
 *        using WiFi
 *
 * @param pvParameters Void pointer, no parameter used here
 */
void task_rtk_get_corrrection_data(void *pvParameters);

/**
 * @brief Task to get location data
 *
 * @param pvParameters
 */

void task_rtk_get_rover_position(void *pvParameters);
/**
 * @brief Task for sending the corrected location data to the
 *        iPhone using BLE
 *
 * @param pvParameters Void pointer, no parameter used here
 */
void task_send_rtk_position_via_ble(void *pvParameters);

/**
 * @brief Task for sending the BNO080 position data to the
 *        iPhone using BLE
 *
 * @param pvParameters Void pointer, no parameter used here
 */
void task_bno_orientation_via_ble(void *pvParameters);

/**
 * @brief Create the queues with the right size
 *
 */
void xQueueSetup(void);

/**
 * @brief Function that blinks one time
 *
 * @param blinkTime       Blink time in ms
 * @param doNotBlock      Type of delay between blinking
 */
void blinkOneTime(int blinkTime, bool doNotBlock);

/**
 * @brief Deletes WiFi station SSID and PW from LittleFS
 *
 */
void wipeWiFiCredentials(void);

/**
 * @brief Report why the chip last reset. Abnormal causes (brownout, panic,
 * watchdog) become error events so field reboots show up in Grafana instead of
 * only as a repeating boot blink pattern. The event waits in the telemetry ring
 * until BLE connects, so emitting this early in setup() is safe.
 */
static void reportResetReason(void)
{
  const esp_reset_reason_t reason = esp_reset_reason();
  DBG.printf("Reset reason: %d\n", (int)reason);

  const char *code = NULL;
  switch (reason)
  {
    case ESP_RST_BROWNOUT: code = "reset_brownout"; break;
    case ESP_RST_PANIC:    code = "reset_panic";    break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      code = "reset_wdt";      break;
    default: break; // poweron / sw / deepsleep are normal, no event
  }
  if (code != NULL)
  {
    char msg[48];
    snprintf(msg, sizeof(msg), "abnormal reset (reason %d), batt %u mV",
             (int)reason, (unsigned)batteryMilliVolts());
    telemetryEmitError(2, code, msg);
  }
}

void setup()
{
  // Board LED used for error codes (written in README.md)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  batteryInit();

  blinkOneTime(1000, true);
  blinkOneTime(1000, true);

  #if DEBUGGING
  Serial.begin(BAUD);
  while (!Serial) {};
  DBG.println(F("Press any key to continue..."));
  while (!Serial.available()) delay(100);
  while (Serial.available()) Serial.read();
  #endif

  reportResetReason();

  #ifdef TESTING
  // Run the AUnit tests here rather than relying on loop(): with no WiFi
  // hotspot in range setup() blocks forever below, so loop() (the usual
  // AUnit driver) may never run. All tests are synchronous; a bounded
  // number of passes resolves them all and prints the summary to serial.
  DBG.println(F("Running unit tests..."));
  for (int i = 0; i < 100; i++)
  {
    aunit::TestRunner::run();
    delay(2);
  }
  #endif

  // blink sequence before starting WiFi and BLE
  blinkOneTime(125, true);
  blinkOneTime(125, true);
  blinkOneTime(2000, true);

  setupWiFi();

  blinkOneTime(125, true);
  blinkOneTime(125, true);
  blinkOneTime(125, true);
  blinkOneTime(125, true);

  while (!WiFi.isConnected())
  {
    DBG.println(F("setup(): Not connected to WiFi station"));
    DBG.printf("WiFi state: %d", WiFi.status());
    blinkOneTime(1000, false);
    blinkOneTime(100, false);
    setupWiFi();
  }

  DBG.print(F("BLE Device name: "));
  DBG.println(getBleName());

  setupBLE();

  setupGNSS();

  DBG.print(F("Device type: ")); DBG.println(DEVICE_TYPE);
  DBG.print(F("Battery: "));
  DBG.print(getBatteryVolts());
  DBG.println(" V");

  // FreeRTOS
  mutexSem = xSemaphoreCreateMutex();
  xQueueSetup();
  /*
  Stack sizes of the tasks. You have to measure the used size in the task (set a high value for first run) and
  after that you can reduce the stack size to an fitting smaller value. This have to repeated if
  the task code is changed. There are no rules, just measure and adjust (thats why its a magic number).
  For measurement you need to uncomment the uxHighWaterMark related code in the task (setup and loop).
  After measurement comment out it again.
  */
  /*
  Sizes from the 2026-07-29 watermark measurement (debug loop() prints
  "stack min free" every 10 s = bytes of stack never touched). Kept margin is
  ~2 KB over observed peak use; the FreeRTOS stack canary turns an undersized
  stack into a loud "Stack canary watchpoint triggered" panic on the bench,
  not silent corruption. Total 21 KB, down from 35 KB - the freed 14 KB of
  heap is what ended the connect-time OOM panics (vQueueDelete assert /
  lock_init_generic abort).
  */
  int stack_size_task_rtk_get_corrrection_data = 1024 * 9;       // min free was 280 of 7168 (!) — grown, was nearly overflowing
  int stack_size_task_rtk_get_rover_position = 1024 * 4;         // min free was 5824 of 7168
  int stack_size_task_bno_orientation_via_ble = 1024 * 4;        // min free was 9160 of 11264
  int stack_size_task_send_rtk_position_via_ble = 1024 * 4;      // min free was 8224 of 10240

  xTaskCreatePinnedToCore( &task_rtk_get_corrrection_data, "task_rtk_get_corrrection_data", stack_size_task_rtk_get_corrrection_data, NULL, TASK_RTK_GET_CORR_DATA_PRIORITY, &hTaskCorrData, RUNNING_CORE_0);
  xTaskCreatePinnedToCore( &task_rtk_get_rover_position, "task_rtk_get_rover_position", stack_size_task_rtk_get_rover_position, NULL, TASK_RTK_GET_POSITION_PRIORITY, &hTaskPosition, RUNNING_CORE_0);
  xTaskCreatePinnedToCore( &task_bno_orientation_via_ble, "task_bno_orientation_via_ble", stack_size_task_bno_orientation_via_ble, NULL, TASK_BNO080_VIA_BLE_PRIORITY, &hTaskBnoBle, RUNNING_CORE_1);
  xTaskCreatePinnedToCore( &task_send_rtk_position_via_ble, "task_send_rtk_position_via_ble", stack_size_task_send_rtk_position_via_ble, NULL, TASK_RTK_POSITION_VIA_BLE_PRIORITY, &hTaskRtkBle, RUNNING_CORE_1);

  // Telemetry drain: lowest priority in the system (PROJECT-PLAN.md par. 5)
  telemetryBleStartTask();

  String thisBoard = ARDUINO_BOARD;
  DBG.print(F("Setup done on "));
  DBG.println(thisBoard);
  logFreeHeap("setup_done");
} /*** end setup ***/

void loop()
{
  #if DEBUGGING
  aunit::TestRunner::run();

  // Periodic memory report: free heap + per-task stack watermarks (bytes of
  // stack never used — the reclaimable margin when right-sizing the
  // stack_size_task_* values in setup()).
  static uint32_t lastMemReport = 0;
  if (millis() - lastMemReport >= 10000)
  {
    lastMemReport = millis();
    logFreeHeap("loop");
    DBG.printf("stack min free: corr %u, pos %u, bno %u, rtkble %u, telem %u, loop %u\n",
               hTaskCorrData ? uxTaskGetStackHighWaterMark(hTaskCorrData) : 0,
               hTaskPosition ? uxTaskGetStackHighWaterMark(hTaskPosition) : 0,
               hTaskBnoBle ? uxTaskGetStackHighWaterMark(hTaskBnoBle) : 0,
               hTaskRtkBle ? uxTaskGetStackHighWaterMark(hTaskRtkBle) : 0,
               telemetryBleTaskHandle() ? uxTaskGetStackHighWaterMark(telemetryBleTaskHandle()) : 0,
               uxTaskGetStackHighWaterMark(NULL));
  }
  #endif
}

/*
=================================================================================
                                GNSS
=======================================_==========================================
*/

char ggaSentence[NMEA_GGA_MAX_LENGTH] = {0};
volatile bool ggaSentenceComplete = false;

// Callback: callbackGPGGA will be called when new GPGGA NMEA data arrives
// See u-blox_structs.h for the full definition of NMEA_GGA_data_t
//         _____  You can use any name you like for the callback. Use the same name when you call setNMEAGPGGAcallback
//        /               _____  This _must_ be NMEA_GGA_data_t
//        |              /           _____ You can use any name you like for the struct
//        |              |          /
//        |              |          |
void callbackGPGGA(NMEA_GGA_data_t *nmeaData)
{
  if (xSemaphoreTake(mutexSem, portMAX_DELAY)) {
    memset(ggaSentence, 0, NMEA_GGA_MAX_LENGTH);
    strncpy(ggaSentence, (const char *)nmeaData->nmea, nmeaData->length);
    ggaSentenceComplete = true;
    xSemaphoreGive(mutexSem);
  }
}

bool setupGNSS()
{
    bool busFailEmitted = false;   // one error event per setup, not per retry
    while (!Wire1.begin(RTK_SDA_PIN, RTK_SCL_PIN))
    {
      DBG.println(F("I2C for RTK not running, check cable!"));
      if (!busFailEmitted)
      {
        busFailEmitted = true;
        telemetryEmitError(2, "i2c_bus_rtk_failed", "Wire1.begin() failing, check cable");
      }
      delay(500);
    }

    Wire1.setClock(I2C_FREQUENCY_400K);

    bool gnssFailEmitted = false;
    while (myGNSS.begin(Wire1, RTK_I2C_ADDR) == false)
    {
      DBG.println(F("u-blox GNSS not detected at default I2C address. Please check wiring. Freezing loop."));
      if (!gnssFailEmitted)
      {
        gnssFailEmitted = true;
        // Severity 3: without the ZED-F9P there is no positioning at all.
        telemetryEmitError(3, "i2c_gnss_not_detected", "ZED-F9P begin() failing, check wiring");
      }
      blinkOneTime(500, false);
    }

    bool response = true;
    response &= myGNSS.setI2COutput(COM_TYPE_UBX | COM_TYPE_NMEA); // Set the I2C port to output both NMEA and UBX messages
    response &= myGNSS.setPortInput(COM_PORT_I2C, COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3); // Be sure RTCM3 input is enabled. UBX + RTCM3 is not a valid state.
    response &= myGNSS.setDGNSSConfiguration(SFE_UBLOX_DGNSS_MODE_FIXED); // Set the differential mode - ambiguities are fixed whenever possible
    response &= myGNSS.enableNMEAMessage(UBX_NMEA_GGA, COM_PORT_I2C);  // Verify the GGA sentence is enabled
    response &= myGNSS.setHighPrecisionMode(true);
    response &= myGNSS.setMainTalkerID(SFE_UBLOX_MAIN_TALKER_ID_GP); // Set the Main Talker ID to "GP". The NMEA GGA messages will be GPGGA instead of GNGGA

    // Set output in Hz.
    response &= myGNSS.setNavigationFrequency(NAVIGATION_FREQUENCY_HZ);

    // Stream the nav messages instead of polling them. Polled getters block
    // on an I2C poll round-trip per message (measured bursts up to ~2 s in
    // updatePosition, stalling the position task and everything behind
    // mutexSem). With auto delivery the module pushes NAV-PVT (fixType,
    // carrSoln, h/vAcc, SIV, pDOP), NAV-HPPOSLLH (high-res lat/lon/height)
    // and NAV-HPPOSECEF (getPositionAccuracy) at the navigation rate, and
    // the getters become non-blocking reads of the cached packet.
    response &= myGNSS.setAutoPVT(true);
    response &= myGNSS.setAutoHPPOSLLH(true);
    response &= myGNSS.setAutoNAVHPPOSECEF(true);
    byte rate = myGNSS.getNavigationFrequency(); // Get the update rate of this module
    DBG.print(F("Current update rate: "));
    DBG.println(rate);

    response &= myGNSS.setNMEAGPGGAcallbackPtr(&callbackGPGGA); // Set up the callback for GPGGA
    response &= myGNSS.setVal8(UBLOX_CFG_MSGOUT_NMEA_ID_GGA_I2C, 10); // Tell the module to output GGA every 10 seconds

    return response;
}

void updatePosition()
{
  coord_t coord;

  myGNSS.checkUblox();

  int32_t lat = myGNSS.getHighResLatitude();
  int8_t latHp = myGNSS.getHighResLatitudeHp();
  int32_t lon = myGNSS.getHighResLongitude();
  int8_t lonHp = myGNSS.getHighResLongitudeHp();
  int32_t accuracy = myGNSS.getPositionAccuracy();

  coord = {.lat = lat, .latHp = latHp, .lon = lon, .lonHp = lonHp};
  // Only stream positions the walk may trust: MIN_ACCEPTABLE_ACCURACY_MM
  // was documented in the config but never enforced. When accuracy
  // degrades past it the position stream simply goes quiet, ubloxUpdatedAt
  // on the phone goes stale, and the app falls back to internal GPS after
  // its 3 s freshness window - degraded RTK and lost RTK use the same
  // fallback, no extra protocol. The 1 Hz gnss_fix telemetry below is
  // deliberately NOT gated: the dead-zone dataset needs the bad fixes too.
  if (accuracy > 0 && accuracy <= MIN_ACCEPTABLE_ACCURACY_MM)
  {
    // Never block here: this runs holding mutexSem, and with no BLE central
    // draining the queue a portMAX_DELAY send wedged the whole GNSS
    // pipeline (position task blocks holding the mutex -> NTRIP task can't
    // pushRawData -> corrections stop; measured 43 s stalls, 2026-07-29).
    // Latest position wins: on a full queue, drop the oldest and retry.
    if (xQueueSend(xQueueCoord, &coord, 0) != pdPASS)
    {
      coord_t discard;
      xQueueReceive(xQueueCoord, &discard, 0);
      xQueueSend(xQueueCoord, &coord, 0);
    }
  }

  // 1 Hz gnss_fix telemetry sample (PROJECT-PLAN.md par. 4.3, the dead-zone
  // dataset). NAV-PVT/HPPOSLLH/HPPOSECEF arrive streamed (setAuto* in
  // setupGNSS), so every getter below is a non-blocking read of the cached
  // packet — no I2C poll round-trips. Emitting is a non-blocking memcpy
  // into the telemetry ring.
  static uint32_t lastFixEmit_ms = 0;
  if (millis() - lastFixEmit_ms >= 1000)
  {
    lastFixEmit_ms = millis();
    uint32_t emitStart_ms = millis();  // measure the getter cost (debug diag)
    TelemetryGnssFix fix;
    fix.lat = lat * 1e-7 + latHp * 1e-9;   // UBX 1e-7 deg + 1e-9 high-res part
    fix.lon = lon * 1e-7 + lonHp * 1e-9;
    fix.heightM = myGNSS.getElipsoid() / 1000.0f
                + myGNSS.getElipsoidHp() / 10000.0f;  // mm + 0.1 mm parts
    fix.fixType = myGNSS.getFixType();
    fix.carrSoln = myGNSS.getCarrierSolutionType();
    fix.hAccMm = myGNSS.getHorizontalAccEst();
    fix.vAccMm = myGNSS.getVerticalAccEst();
    fix.numSv = myGNSS.getSIV();
    fix.pdop = myGNSS.getPDOP() * 0.01f;
    fix.corrAgeMs = telemetryCorrAgeMs();
    telemetryEmitGnssFix(fix);
    DBG.printf("gnss_fix: acc %d mm, getters took %u ms\n",
               accuracy, millis() - emitStart_ms);
  }
}

/*
=================================================================================
                                FreeRTOS
=================================================================================
*/

void task_rtk_get_rover_position(void *pvParameters)
{
  (void)pvParameters;

  // Measure stack size
  UBaseType_t uxHighWaterMark;

  // Wait for first correction data
  while ( ! beginPositioning) { vTaskDelay(1000/portTICK_PERIOD_MS); }

  while (true)
  {
    if (xSemaphoreTake(mutexSem, portMAX_DELAY))
    {
      updatePosition();

      // Measure stack size (last was 2304)
      // uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
      // DBG.print(F("task_rtk_get_rover_position loop, uxHighWaterMark: "));
      // DBG.println(uxHighWaterMark);

      xSemaphoreGive(mutexSem);
    }

    vTaskDelay(TASK_RTK_GET_POSITION_INTERVAL_MS/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void task_rtk_get_corrrection_data(void *pvParameters)
{
  (void)pvParameters;

  //=========================================================================
  // 5 RTCM messages take approximately ~300ms to arrive at 115200bps
  long lastReceivedRTCM_ms = 0;
  // If we fail to get a complete RTCM frame after 10s, then disconnect from caster
  const int maxTimeBeforeHangup_ms = 10000;

  int timeBetweenGGAUpdate_ms = 10000; //GGA is required for Rev2 NTRIP casters. Don't transmit but once every 10 seconds
  long lastTransmittedGGA_ms = 0;

  // Measure stack size
  UBaseType_t uxHighWaterMark;

  // Read RTK credentials
  String casterHost = kCasterHost;
  String casterPort = kCasterPort;
  String casterUser = kCasterUser;
  String casterPass = kCasterPass;
  String mountPoint = kMountPoint;

  // Check RTK credentials
  bool credentialsExists = true;
  credentialsExists &= !casterHost.isEmpty();
  credentialsExists &= !casterPort.isEmpty();
  credentialsExists &= !casterUser.isEmpty();
  credentialsExists &= !mountPoint.isEmpty();

  while (!credentialsExists)
  {
    DBG.println(F("RTK credentials incomplete!\nFreezing RTK task."));
    blinkOneTime(2000, true);
  }

  WiFiClient ntripClient;
  long rtcmCount = 0;

  // ntrip_status bookkeeping (PROJECT-PLAN.md par. 4.3): events on state
  // transitions only, never per retry iteration - outages must not flood
  // the ring. successfulConnects - 1 = "reconnects" in the event.
  uint32_t successfulConnects = 0;
  bool wasConnected = false;
  bool outageErrorEmitted = false;   // one error event per outage, not per retry
  bool reconnectingEmitted = false;  // one reconnecting event per outage
  bool wifiLossEmitted = false;

  while (true) // Task loop begins
  {
    // Mirror the link state for the telemetry heartbeat
    bool nowConnected = ntripClient.connected();
    telemetrySetNtripConnected(nowConnected);
    if (wasConnected && !nowConnected)
    {
      telemetryEmitNtripStatus(TELEM_NTRIP_DISCONNECTED,
                               successfulConnects > 0 ? successfulConnects - 1 : 0,
                               telemetryRtcmBytesTotal());
    }
    wasConnected = nowConnected;

    /*
    This ist most of the content beginServing() func from the
    Sparkfun u-blox GNSS Arduino Library/ZED-F9P/Example15-NTRIPClient
    Because I did not wanted to change the code too much if you want to compare
    with the Example14: "continue" calls are used in place of "return".
    (A task must not return.)
    */

    if (ntripClient.connected() == false)
    {
      // First check WiFi connection
      while (!WiFi.isConnected())
      {
        DBG.println(F("task_rtk_get_corr_data loop: Not connected to WiFi station"));
        DBG.printf("WiFi state: %d", WiFi.status());
        DBG.println();
        if (!wifiLossEmitted)
        {
          wifiLossEmitted = true;
          telemetryEmitError(1, "wifi_disconnected", "hotspot lost, reconnecting");
        }
        setupStationMode(kWifiSsid, kWifiPw);
        blinkOneTime(1000, false);
        blinkOneTime(100, false);
      }
      wifiLossEmitted = false;

      if (successfulConnects > 0 && !reconnectingEmitted)
      {
        // Once per outage, and only after a previous connection: the
        // initial connect is not a "reconnecting" transition.
        reconnectingEmitted = true;
        telemetryEmitNtripStatus(TELEM_NTRIP_RECONNECTING,
                                 successfulConnects - 1,
                                 telemetryRtcmBytesTotal());
      }

      DBG.print(F("Opening socket to "));
      DBG.println(casterHost.c_str());

      // Attempt connection
      if (ntripClient.connect( casterHost.c_str(), (uint16_t)casterPort.toInt() ) == false)
      {
        DBG.println(F("Connection to caster failed, retry in 5s"));
        if (!outageErrorEmitted)
        {
          outageErrorEmitted = true;
          telemetryEmitError(1, "ntrip_connect_failed", "TCP connect to caster failed");
        }
        vTaskDelay(5000/portTICK_PERIOD_MS);
        continue; // skip to next iteration and retry
      }
      else
      {
        DBG.print(F("Connected to "));
        DBG.print(casterHost.c_str());
        DBG.print(F(": "));
        DBG.println((uint16_t)casterPort.toInt());

        DBG.print(F("Requesting NTRIP Data from mount point "));
        DBG.println(mountPoint.c_str());

        const int SERVER_BUFFER_SIZE = 512;
        char serverRequest[SERVER_BUFFER_SIZE];

        int requestLen = snprintf(serverRequest, SERVER_BUFFER_SIZE, "GET /%s HTTP/1.0\r\nUser-Agent: NTRIP SparkFun u-blox Client v1.0\r\n",
                mountPoint.c_str());

        char credentials[512];
        if (strlen(casterUser.c_str()) == 0)
        {
          strncpy(credentials, "Accept: */*\r\nConnection: close\r\n", sizeof(credentials));
        }
        else
        {
          //Pass base64 encoded user:pw
          // length(), not sizeof: sizeof(String) is the object size (~16 B),
          // not the stored text, and %s must get c_str(), never the object.
          char userCredentials[casterUser.length() + 1 + casterPass.length() + 1]; //The ':' takes up a spot
          snprintf(userCredentials, sizeof(userCredentials), "%s:%s", casterUser.c_str(), casterPass.c_str());

          DBG.print(F("Sending credentials: "));
          DBG.println(userCredentials);

          #if defined(ARDUINO_ARCH_ESP32)
          // Encode with ESP32 built-in library
          base64 b;
          String strEncodedCredentials = b.encode(userCredentials);
          char encodedCredentials[strEncodedCredentials.length() + 1];
          strEncodedCredentials.toCharArray(encodedCredentials, sizeof(encodedCredentials)); //Convert String to char array
          snprintf(credentials, sizeof(credentials), "Authorization: Basic %s\r\n", encodedCredentials);
          #else
          // Encode with nfriendly library
          int encodedLen = base64_enc_len(strlen(userCredentials));
          char encodedCredentials[encodedLen]; //Create array large enough to house encoded data
          base64_encode(encodedCredentials, userCredentials, strlen(userCredentials)); //Note: Input array is consumed
          #endif
        }

        // Append with the REMAINING space as the bound. The previous
        // strncat(dst, src, SERVER_BUFFER_SIZE) bounded by the full
        // destination size (-Wstringop-overflow) and could smash this
        // task's stack. snprintf also reports truncation, which strncat
        // cannot - and a truncated request must not be sent: it would
        // carry broken headers and fail at the caster anyway.
        bool requestFits = requestLen > 0 && requestLen < SERVER_BUFFER_SIZE;
        if (requestFits)
        {
          int appended = snprintf(serverRequest + requestLen,
                                  SERVER_BUFFER_SIZE - requestLen,
                                  "%s\r\n", credentials);
          requestFits = appended >= 0 && appended < SERVER_BUFFER_SIZE - requestLen;
        }
        if (!requestFits)
        {
          DBG.println(F("NTRIP server request exceeds buffer, not sent. Check mount point / credential lengths."));
          telemetryEmitError(2, "ntrip_request_overflow",
                             "caster request exceeds buffer; check mount point / credential lengths");
          ntripClient.stop();
          vTaskDelay(5000/portTICK_PERIOD_MS);
          continue; // retry loop; config is wrong, but never overflow
        }
        DBG.printf("serverRequest len: %d ", strlen(serverRequest));
        DBG.print(F("serverRequest size: "));
        DBG.print(strlen(serverRequest));
        DBG.print(F(" of "));
        DBG.print(sizeof(serverRequest));
        DBG.println(F(" bytes available"));

        DBG.println(F("Sending server request:"));
        DBG.println(serverRequest);
        ntripClient.write(serverRequest, strlen(serverRequest));

        // Wait for response
        unsigned long timeout = millis();
        while (ntripClient.available() == 0)
        {
          if (millis() - timeout > CONNECTION_TIMEOUT_MS)
          {
            ntripClient.stop(); // Too many requests with wrong settings will lead to bann, stop here
            DBG.println(F("Caster timed out!"));
            vTaskDelay(5000/portTICK_PERIOD_MS);
            continue; // skip to next iteration and retry
          }
          vTaskDelay(1000/portTICK_PERIOD_MS);
        }

        // Check reply
        bool connectionSuccess = false;
        char response[512];
        int responseSpot = 0;

        while (ntripClient.available())
        {
          if (responseSpot == sizeof(response) - 1) break;

          response[responseSpot++] = ntripClient.read();
          if (strstr(response, "200") > 0) // Look for 'ICY 200 OK'
            connectionSuccess = true;
          if (strstr(response, "401") > 0) // Look for '401 Unauthorized'
          {
            DBG.println(F("Your credentials look bad!\nCheck you caster username, password and ban status (got email from rtk2go?)"));
            connectionSuccess = false;
          }
        }
        response[responseSpot] = '\0';

        DBG.print(F("Caster responded with: "));
        DBG.println(response);

        if (connectionSuccess == false)
        {
          DBG.print(F("Failed to connect to "));
          DBG.print(casterHost.c_str());
          DBG.print(F(": "));
          DBG.println(response);
          if (!outageErrorEmitted)
          {
            outageErrorEmitted = true;
            // Caster spoke but refused (401, wrong mount point, ban):
            // config-class problem, so severity 2 - a Grafana alert, not
            // noise. The caster response goes in msg (no secrets in it).
            telemetryEmitError(2, "ntrip_bad_response", response);
          }
          vTaskDelay(5000/portTICK_PERIOD_MS);
          continue; // skip to next iteration and retry
        }
        else
        {
          DBG.print(F("Connected to "));
          DBG.println(casterHost.c_str());
          lastReceivedRTCM_ms = millis(); // Reset timeout

          successfulConnects++;
          outageErrorEmitted = false;
          reconnectingEmitted = false;
          telemetryEmitNtripStatus(TELEM_NTRIP_CONNECTED,
                                   successfulConnects - 1,
                                   telemetryRtcmBytesTotal());

          // checkUblox under mutexSem like every other myGNSS I2C access:
          // this runs on core 0 while the position task polls the same
          // object/bus from its own loop - unsynchronized access desyncs
          // the UBX parser and stalled the position getters for seconds
          // (measured 8.8 s, 2026-07-29). checkCallbacks must stay OUTSIDE
          // the mutex: it touches no I2C, and it invokes callbackGPGGA,
          // which takes mutexSem itself (non-recursive - taking it here
          // would self-deadlock this task and starve positioning).
          if (xSemaphoreTake(mutexSem, portMAX_DELAY))
          {
            myGNSS.checkUblox();
            xSemaphoreGive(mutexSem);
          }
          myGNSS.checkCallbacks();
        }
      } // End attempt to connect
    } // End connected == false

    if (ntripClient.connected() == true)
    {
      uint8_t rtcmData[512 * 4]; // Most incoming data is around 500 bytes but may be larger
      rtcmCount = 0;

      //Print any available RTCM data
      while (ntripClient.available())
      {
        //DBG.write(ntripClient.read()); // Pipe to serial port is fine but beware, it's a lot of binary data
        rtcmData[rtcmCount++] = ntripClient.read();
        if (rtcmCount == sizeof(rtcmData)) break;
      }

      if (rtcmCount > 0)
      {
        //Push RTCM to GNSS module over I2C
        if (xSemaphoreTake(mutexSem, portMAX_DELAY))
        {
          myGNSS.pushRawData(rtcmData, rtcmCount, false);
          beginPositioning = true;
          telemetryNoteRtcmPushed(rtcmCount);  // feeds corr_age_ms + bytes_rx
          xSemaphoreGive(mutexSem);
          DBG.print(F("RTCM pushed to ZED: "));
          DBG.println(rtcmCount);
          uint32_t currentTime = millis();
          DBG.print(F("Last data before ms: "));
          DBG.println(currentTime - lastReceivedRTCM_ms);
          lastReceivedRTCM_ms = currentTime;
        }

      }
    }   // End (ntripClient.connected() == true)

    // Dispatch pending NMEA callbacks every iteration, or callbackGPGGA never
    // refreshes ggaSentenceComplete after the connect-time call above and the
    // VRS caster drops us for not sending GGA. Must stay OUTSIDE mutexSem:
    // no I2C here, and callbackGPGGA takes the (non-recursive) mutex itself.
    myGNSS.checkCallbacks();

    //Provide the caster with our current position as needed
    if (ntripClient.connected() == true && (millis() - lastTransmittedGGA_ms) > timeBetweenGGAUpdate_ms)
    {
      char localGgaSentence[NMEA_GGA_MAX_LENGTH] = {0};
      bool shouldSendGga = false;

      if (xSemaphoreTake(mutexSem, portMAX_DELAY))
      {
        if (ggaSentenceComplete == true)
        {
          strncpy(localGgaSentence, ggaSentence, NMEA_GGA_MAX_LENGTH - 1);
          localGgaSentence[NMEA_GGA_MAX_LENGTH - 1] = '\0';
          shouldSendGga = true;

          // start over
          ggaSentenceComplete = false;
          lastTransmittedGGA_ms = millis();
        }
        xSemaphoreGive(mutexSem);
      }

      if (shouldSendGga)
      {
        DBG.print(F("Pushing GGA to server: "));
        DBG.println(localGgaSentence);

        //Push our current GGA sentence to caster
        ntripClient.print(localGgaSentence);
        ntripClient.print("\r\n");
      }
    }

    // Close socket if we don't have new data for 10s
    if (millis() - lastReceivedRTCM_ms > maxTimeBeforeHangup_ms)
    {
      DBG.println(F("RTCM timeout. Disconnecting..."));
      if (ntripClient.connected() == true)
      {
        // Socket up but no corrections for 10 s - this is the signature of a
        // correction-delivery problem (vs. GNSS degradation, PROJECT-PLAN par. 2)
        telemetryEmitError(1, "ntrip_rtcm_timeout", "no RTCM for 10 s, dropping caster connection");
        ntripClient.stop();
      }
    }

    // Measure stack size (last was 19320)
    // uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
    // DBG.print(F("task_rtk_get_corrrection_data loop, uxHighWaterMark: "));
    // DBG.println(uxHighWaterMark);
    // } /*** End if (xSemaphoreTake(mutexSem, portMAX_DELAY)) ***/

    // Wait a bit before the next request will be started
    vTaskDelay(TASK_WIFI_RTK_DATA_INTERVAL_MS/portTICK_PERIOD_MS);
  }

  // Delete self task
  vTaskDelete(NULL);

} /*** end task_rtk_get_corrrection_data ***/

/*
=================================================================================
                                BLE
=================================================================================
*/
void setupBLE(void)
{
  String deviceName = getBleName();
  BLEDevice::init(deviceName.c_str());
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  // Create characteristics
  pHeadtrackerCharacteristic = pService->createCharacteristic(
    HEADTRACKER_CHARACTERISTIC_UUID,
    // BLECharacteristic::PROPERTY_READ   |
    // BLECharacteristic::PROPERTY_WRITE  |
    // BLECharacteristic::PROPERTY_INDICATE |
    BLECharacteristic::PROPERTY_NOTIFY  // We only use notify characteristic (fastest -> no response)
  );

  pRealtimeKinematicsCharacteristic = pService->createCharacteristic(
    REALTIME_KINEMATICS_CHARACTERISTIC_UUID,
    //  BLECharacteristic::PROPERTY_READ   |
    //  BLECharacteristic::PROPERTY_WRITE  |
    //  BLECharacteristic::PROPERTY_INDICATE |
    BLECharacteristic::PROPERTY_NOTIFY  // We only use notify characteristic (fastest -> no response)
  );

  pHeadtrackerCharacteristic->addDescriptor(new BLE2902());
  pHeadtrackerCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  pHeadtrackerCharacteristic->setValue(deviceName.c_str());

  pRealtimeKinematicsCharacteristic->addDescriptor(new BLE2902());
  // pRealtimeKinematicsCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  // pRealtimeKinematicsCharacteristic->setValue(deviceName.c_str());

  pService->start();

  // Telemetry GATT service (PROJECT-PLAN.md par. 5.1); not advertised — the
  // 31 B adv payload has no room for a second 128-bit UUID.
  telemetryBleSetup(pServer);

  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x12);  // 0x06 x 1.25 ms = 7.5 ms, functions that help with iPhone connections issue
  pAdvertising->setMaxPreferred(0x24);  // 30 ms
  //pAdvertising->start();
  BLEDevice::startAdvertising();
  DBG.println(F("Characteristic defined! Now you can read it in your phone!"));
}

void setupBNO080()
{
  Wire.begin();
  bool beginFailEmitted = false;  // one error event per setup, not per retry
  while (!bno080.begin())
  {
    // Wait
    DBG.println(F("BNO080 not ready, waiting for I2C..."));
    if (!beginFailEmitted)
    {
      beginFailEmitted = true;
      telemetryEmitError(2, "i2c_bno080_not_detected", "BNO080 begin() failing, check wiring");
    }
    delay(500);
  }

  // Activate IMU functionalities
  bno080.enableARVRStabilizedRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
  // bno080.enableARVRStabilizedGameRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
  // bno080.enableRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
  // bno080.enableGameRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);

  bno080.enableAccelerometer(BNO080_LIN_ACCEL_UPDATE_RATE_MS);
  bno080.enableLinearAccelerometer(BNO080_LIN_ACCEL_UPDATE_RATE_MS);
  // bno080.enableStepCounter(20);   // Thomas: Funktioniert sehr schlecht..

  // Markus: --> timeBetweenReports should not be 20 ms ;)  try this: 31.25 Hz
  // bno080.enableStepCounter(32);
}

void xQueueSetup()
{
  xQueueCoord  = xQueueCreate( QUEUE_SIZE, sizeof( coord_t ) );
}

void task_send_rtk_position_via_ble(void *pvParameters)
{
  (void)pvParameters;

  String latLonStr((char *)0);
  // Latitude: 9, delimiter: 1, latitudeHp: 2, longitude: 9, delimiter: 1, longitudeHp: 2,
  latLonStr.reserve(27);

  coord_t coord;
  int32_t lat, lon;
  int8_t latHp, lonHp;

  while (!bleConnected) blinkOneTime(100, true);

  UBaseType_t uxHighWaterMark;
  // uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
  // DBG.print(F("task_send_rtk_position_via_ble setup, uxHighWaterMark: "));
  // DBG.println(uxHighWaterMark);

  while (true)
  {
    if (bleConnected)
    {
      if (xQueueReceive( xQueueCoord, &coord, ( TickType_t ) 10 ) == pdPASS)
      {
        DBG.print(F("Received coord.lat = "));
        DBG.print(coord.lat);
        DBG.print(F(", coord.latHp = "));
        DBG.print(coord.latHp);
        DBG.print(F(" coord.lon = "));
        DBG.print(coord.lon);
        DBG.print(F(", coord.lonHp = "));
        DBG.println(coord.lonHp);
        lat = coord.lat;
        latHp = coord.latHp;
        lon = coord.lon;
        lonHp = coord.lonHp;

        // Send coords
        latLonStr = String(lat);
        latLonStr += DATA_STR_DELIMITER;
        latLonStr += String(latHp);
        latLonStr += DATA_STR_DELIMITER;
        latLonStr += String(lon);
        latLonStr += DATA_STR_DELIMITER;
        latLonStr += String(lonHp);
        // DBG.print(F("latLonStr.length(): "));DBG.println(latLonStr.length());
        pRealtimeKinematicsCharacteristic->setValue(latLonStr.c_str());
        pRealtimeKinematicsCharacteristic->notify();
      }

      /*  Measure stack size (last was 9356) */
      // uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
      // DBG.print(F("task_send_rtk_position_via_ble loop, uxHighWaterMark: "));
      // DBG.println(uxHighWaterMark);

    } /*** if (bleConnected) ends ***/
    else
    {
      blinkOneTime(100, true);
    }


    vTaskDelay(TASK_RTK_BLE_INTERVAL_MS/portTICK_PERIOD_MS);
    // taskYIELD();
  } // while (true) ends

  // Delete self task
  vTaskDelete(NULL);
}

void task_bno_orientation_via_ble(void *pvParameters)
{
  (void)pvParameters;

  while (!bleConnected)
  {
    DBG.println(F("BNO tasks setup: Open RWA to connect BLE"));
    vTaskDelay(1000/portTICK_PERIOD_MS);
  }

  setupBNO080();

  // imu_status telemetry (PROJECT-PLAN.md par. 4.3): every 60 s report the
  // measured notify rate, calibration accuracy and reset count. hasReset()
  // and getQuatAccuracy() read cached state - no extra I2C on the hot path.
  uint32_t imuSampleCount = 0;
  uint32_t imuResets = 0;
  uint32_t lastImuStatus_ms = millis();

  float quatI, quatJ, quatK, quatReal, yawDegreeF, pitchDegreeF, linAccelZF;// rollDegreeF;
  int pitchDegree, yawDegree;// rollDegree;
  String dataStr((char *)0);
  // String size: (yaw: 3, delimiter: 1, pitch: 3, delimiter: 1, linAccelZF: 4) = 12 + LIN_ACCEL_Z_DECIMAL_DIGITS
  dataStr.reserve(12 + LIN_ACCEL_Z_DECIMAL_DIGITS);

  // Measure stack size
  UBaseType_t uxHighWaterMark;
  // uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
  // DBG.print(F("task_bno_orientation_via_ble setup, uxHighWaterMark: "));
  // DBG.println(uxHighWaterMark);

  while (true)
  {
    if (!bleConnected)
    {
      DBG.println(F("BNO tasks loop: Please connect BLE"));
      vTaskDelay(1000/portTICK_PERIOD_MS);
    }
    else
    {
      if (bno080.hasReset()) imuResets++;  // reading unflags it
      if (millis() - lastImuStatus_ms >= 60000)
      {
        float reportRateHz = imuSampleCount * 1000.0f / (millis() - lastImuStatus_ms);
        telemetryEmitImuStatus(bno080.getQuatAccuracy(), reportRateHz, imuResets);
        lastImuStatus_ms = millis();
        imuSampleCount = 0;
      }

      // TODO: Separate reading values from sending values
      if (bno080.dataAvailable())
      {
        imuSampleCount++;
        quatI = bno080.getQuatI();
        quatJ = bno080.getQuatJ();
        quatK = bno080.getQuatK();
        quatReal = bno080.getQuatReal();

        imu::Quaternion quat = imu::Quaternion(quatReal, quatI, quatJ, quatK);
        quat.normalize();
        imu::Vector<3> q_to_euler = quat.toEuler();
        yawDegreeF = q_to_euler.x();
        yawDegreeF = yawDegreeF * -180.0 / M_PI;   // conversion to Degree

        if ( yawDegreeF < 0 ) yawDegreeF += 359.0; // convert negative to positive angles

        yawDegree = (int)(round(yawDegreeF));

        pitchDegreeF = q_to_euler.z();
        pitchDegreeF = pitchDegreeF * -180.0 / M_PI;
        pitchDegree = (int)(round(pitchDegreeF));

        // rollDegreeF = q_to_euler.y();
        // rollDegreeF = rollDegreeF * -180.0 / M_PI;
        // rollDegree = (int)(round(rollDegreeF));

        // Seems to be much slower than bno080.getAccelZ()
        linAccelZF = bno080.getLinAccelZ();

        dataStr = String(yawDegree) + DATA_STR_DELIMITER + String(pitchDegree) \
                + DATA_STR_DELIMITER + String(linAccelZF, LIN_ACCEL_Z_DECIMAL_DIGITS);
        pHeadtrackerCharacteristic->setValue(dataStr.c_str());
        pHeadtrackerCharacteristic->notify();
        // DBG.println(linAccelZF);
        }
        else
        {
          DBG.println(F("Ready for BNO080 dataAvailable"));
          vTaskDelay(1000/portTICK_PERIOD_MS);
        }
        // Measure stack size
        // uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
        // DBG.print(F("task_bno_orientation_via_ble loop, uxHighWaterMark: "));
        // DBG.println(uxHighWaterMark);

      }
      vTaskDelay(TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS/portTICK_PERIOD_MS);
      // taskYIELD(); // 11.25 ms is the BLE connection interval, makes no sense to try to send faster
    if (!bno080.dataAvailable())
    {
      DBG.println(F("No BNO080 dataAvailable"));
    }
    }
  // Delete self task
  vTaskDelete(NULL);

} /*** end task_bno_orientation_via_ble ***/

/*
=================================================================================
                                Button(s)
=================================================================================
*/
void buttonHandler(Button2 &btn)
{
  if (btn == rebootButton)
  {
    digitalWrite(LED_BUILTIN, HIGH);
    DBG.println(F("rebooting..."));
    ESP.restart();
  }
}

void blinkOneTime(int blinkTime, bool doNotBlock)
{
  digitalWrite(LED_BUILTIN, HIGH);
  doNotBlock ? vTaskDelay(blinkTime) : delay(blinkTime);
  digitalWrite(LED_BUILTIN, LOW);
  doNotBlock ? vTaskDelay(blinkTime) : delay(blinkTime);
}
