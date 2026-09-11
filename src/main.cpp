
/*******************************************************************************
 * @file main.cpp
 * @authors Markus Hädrich
 * <br>
 * @brief This is part of a distributed software, here: head tracker and GNSS
 *        positioning using Sparkfun Real Time Kinematics
 * <br>
 * @todo  - Upgrade to Sparkfun RTK Library v3
 *
 * @version 0.46.1
 * @date 2026-09-11
 ******************************************************************************/


#include <Arduino.h>
#include <Wire.h> // BNO080 and uBlox GNSS
#include <BLEDevice.h>
#include <BLE2902.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <SparkFun_BNO080_Arduino_Library.h>
#include <sdkconfig.h>
#include <esp_system.h> // esp_reset_reason()
#include <RTKRoverConfig.h>
#include <CasterSecrets.h>
#include <battery.h>
#include <handle_wifi.h>
#include <WiFiUdp.h> // hotspot path warmer (warmHotspotPath)
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

  DBG.print(F("BLE Device name: "));
  DBG.println(getBleName());

  // BLE comes up first, and we don't wait for WiFi in setup().
  setupBLE();

  // Telemetry drain: lowest priority in the system (PROJECT-PLAN.md par. 5).
  // Started BEFORE the blocking sensor setups on purpose: setupGNSS()/
  // setupBNO080() retry forever when a sensor doesn't answer, and with the
  // drain not yet running the assembly would sit BLE-connected but mute.
  // The i2c_* error events waiting in the ring, never delivered (observed
  // 2026-08-24, rwa-hs-4: F9P not ACKing, app received no telemetry at all).
  // The task needs only the ring, BLE and battery/WiFi reads.
  telemetryBleStartTask();

  delay(RADIO_START_STAGGER_MS);

  // One bounded attempt (setupStationMode() gives up after 10 s), then carry on
  // whatever the result. WiFi has one consumer: task_rtk_get_corrrection_data, that task has a
  // reconnect ladder for a hotspot that is missing or lost.
  setupWiFi();

  blinkOneTime(125, true);
  blinkOneTime(125, true);
  blinkOneTime(125, true);
  blinkOneTime(125, true);

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

  // (Telemetry drain task is started right after setupBLE() above, so sensor
  // failures during setup are already visible in diagnostics.)

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

// Receiver-liveness timestamp: millis() of the last GGA sentence the module
// produced (callbackGPGGA fires with or without a fix, ~1/s at the configured
// MSGOUT rate). Written from callbackGPGGA and the NTRIP task's re-arm sites.
// GNSS_SILENT_AFTER_MS without one means the receiver is mute
// (bench 4.1, 2026-08-24) and drives the recovery ladder.
static volatile uint32_t lastGgaHeard_ms = 0;

// millis() of the last GGA carrying a fix (quality field >= 1), 0 = never.
// Drives the NTRIP connect gate: the VRS computes its virtual station from
// our GGA, and a fixless one is unusable to it. Same single-writer situation
// as lastGgaHeard_ms (callbackGPGGA only runs from the NTRIP task's
// checkCallbacks dispatch).
static volatile uint32_t lastFixGgaHeard_ms = 0;

// GGA fix-quality field (7th comma-separated field; 0 = no fix). Returns 0 on
// any parse shortfall: an unparseable sentence shouldn't count as a fix.
static uint8_t ggaFixQuality(const uint8_t *nmea, uint16_t length)
{
  uint8_t commas = 0;
  for (uint16_t i = 0; i < length; i++)
  {
    if (nmea[i] != ',') continue;
    if (++commas < 6) continue;
    if (i + 1 < length && nmea[i + 1] >= '0' && nmea[i + 1] <= '9')
      return nmea[i + 1] - '0';
    return 0;
  }
  return 0;
}

// Callback: callbackGPGGA will be called when new GPGGA NMEA data arrives
// See u-blox_structs.h for the full definition of NMEA_GGA_data_t
//         _____  You can use any name you like for the callback. Use the same name when you call setNMEAGPGGAcallback
//        /               _____  This _must_ be NMEA_GGA_data_t
//        |              /           _____ You can use any name you like for the struct
//        |              |          /
//        |              |          |
void callbackGPGGA(NMEA_GGA_data_t *nmeaData)
{
  // Liveness first, unconditionally: even when the copy below is skipped,
  // a GGA arriving proves the receiver is producing output.
  lastGgaHeard_ms = millis();

  // Store only sentences with a fix: what reaches ggaSentence is what gets
  // pushed to the caster, and a fixless GGA shouldn't go there.
  // No fix also leaves ggaSentenceComplete un-armed, so a mid-session fix loss
  // stops the pushes instead of repeating the last position.
  if (ggaFixQuality(nmeaData->nmea, nmeaData->length) == 0)
    return;
  lastFixGgaHeard_ms = millis();

  // Bounded take: this runs inside the NTRIP task's checkCallbacks(), and a
  // portMAX_DELAY here was the unattributed ~14 s of the 2026-08-24 stall
  // capture (position task holding mutexSem through a slow-I2C stretch).
  // Skipping one GGA is free, the module emits a fresh one every epoch.
  if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GGA_MUTEX_TIMEOUT_MS))) {
    memset(ggaSentence, 0, NMEA_GGA_MAX_LENGTH);
    strncpy(ggaSentence, (const char *)nmeaData->nmea, nmeaData->length);
    ggaSentenceComplete = true;
    xSemaphoreGive(mutexSem);
  }
}

/**
 * @brief One begin() attempt + the full rover configuration. Shared by boot
 * (setupGNSS, which retries around it) and the runtime recovery ladder in
 * the NTRIP task (which calls it holding mutexSem after a reset).
 * Never loops/blocks on an unresponsive module.
 *
 * @return true if the module responded and every config write was accepted
 */
static bool configureGNSS()
{
    if (myGNSS.begin(Wire1, RTK_I2C_ADDR) == false)
      return false;

    // Fewer, larger I2C transactions: 4x fewer start/stop cycles for the
    // same data (library default is 32; the lib itself recommends 128 on
    // ESP32, whose Wire buffer is 128 B).
    myGNSS.setI2CTransactionSize(128);

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
    // GGA every 10th nav epoch = 1/s at 10 Hz. This doubles as the
    // receiver-liveness signal (lastGgaHeard_ms), so keep it ~1 Hz.
    response &= myGNSS.setVal8(UBLOX_CFG_MSGOUT_NMEA_ID_GGA_I2C, 10);

    return response;
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
        telemetryEmitError(3, "i2c_bus_rtk_failed", "Wire1.begin() failing, check cable");
      }
      delay(500);
    }

    Wire1.setClock(I2C_FREQUENCY_400K);

    bool gnssFailEmitted = false;
    while (!configureGNSS())
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

    return true;
}

void updatePosition()
{
  coord_t coord = {0, 0, 0, 0};  // written only under llhFresh below

  // gnss_pipe_stall instrumentation (2026-08-24 capture): the long mutexSem
  // holds were not the explicit checkUblox but the getters' hidden re-entries:
  // every stale getter re-runs a full checkUblox pass internally (~1 s each
  // on a degraded bus, up to ~13 per emit second). This function is therefore
  // structured to pay at most three I2C passes per call: one freshness check
  // per streamed packet (getHPPOSLLH / getNAVHPPOSECEF / getPVT, each doing
  // one checkUbloxInternal pass), after which every getter below is a pure
  // cached read. The whole mutex-held body is measured, not just checkUblox.
  uint32_t holdStart_ms = millis();
  bool llhFresh = myGNSS.getHPPOSLLH();       // pass 1 (drains pending I2C)
  uint32_t llhMs = millis() - holdStart_ms;
  bool ecefFresh = myGNSS.getNAVHPPOSECEF();  // pass 2 (usually finds nothing new)
  bool pvtFresh = myGNSS.getPVT();            // pass 3

  int32_t accuracy = 0;
  if (llhFresh)
  {
    int32_t lat = myGNSS.getHighResLatitude();
    int8_t latHp = myGNSS.getHighResLatitudeHp();
    int32_t lon = myGNSS.getHighResLongitude();
    int8_t lonHp = myGNSS.getHighResLongitudeHp();
    accuracy = ecefFresh ? myGNSS.getPositionAccuracy() : 0;

    coord = {.lat = lat, .latHp = latHp, .lon = lon, .lonHp = lonHp};
  }
  // Only stream positions the walk may trust: MIN_ACCEPTABLE_ACCURACY_MM
  // was documented in the config but never enforced. When accuracy
  // degrades past it (or the receiver stops producing solutions and there
  // is nothing fresh to send) the position stream simply goes quiet,
  // ubloxUpdatedAt on the phone goes stale, and the app falls back to
  // internal GPS after its freshness window. Degraded RTK and lost
  // RTK use the same fallback.
  if (llhFresh && accuracy > 0 && accuracy <= MIN_ACCEPTABLE_ACCURACY_MM)
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
  // dataset). Emitted only when a fresh solution actually arrived: every
  // getter below is then a cached read (its packet's freshness was checked
  // above), so no hidden I2C re-entry happens under the mutex. When the
  // receiver stops producing solutions the stream gaps instead of repeating
  // stale fixes. The gap itself is diagnostic (loops_pos + gnss_pipe_stall
  // tell the rest). Emitting is a non-blocking memcpy into the ring.
  static uint32_t lastFixEmit_ms = 0;
  if (llhFresh && pvtFresh && millis() - lastFixEmit_ms >= 1000)
  {
    lastFixEmit_ms = millis();
    TelemetryGnssFix fix;
    fix.lat = coord.lat * 1e-7 + coord.latHp * 1e-9;  // UBX 1e-7 deg + 1e-9 high-res part
    fix.lon = coord.lon * 1e-7 + coord.lonHp * 1e-9;
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
    DBG.printf("gnss_fix: acc %d mm\n", accuracy);
  }

  // Whole-hold stall probe: the 2026-08-24 crawl was invisible to a probe
  // that timed only checkUblox. Anything over threshold for the full body
  // (all I2C passes + cached reads) becomes the sev-1 event.
  uint32_t holdMs = millis() - holdStart_ms;
  if (holdMs > GNSS_PIPE_STALL_MS)
  {
    static uint32_t lastStallEmit_ms = 0;
    if (millis() - lastStallEmit_ms >= GNSS_PIPE_STALL_GAP_MS)
    {
      lastStallEmit_ms = millis();
      char msg[80];
      snprintf(msg, sizeof(msg), "updatePosition held mutex %u ms (llh pass %u ms)",
               (unsigned)holdMs, (unsigned)llhMs);
      telemetryEmitError(1, "gnss_pipe_stall", msg);
    }
  }
}

// Hotspot path warmer (see HOTSPOT_WARM_INTERVAL_MS in RTKRoverConfig.h for
// the doze doom loop this breaks). Sends one minimal DNS A query for the
// caster host to the hotspot's DNS server every interval. Deliberately NOT
// WiFi.hostByName(): that goes through lwIP's DNS cache, which answers a
// repeated name locally — no packet on the wire — until the record's TTL
// expires, so it cannot hold a cadence. A hand-built query always transmits,
// and the reply coming back keeps the NAT entry fresh too. Fire-and-forget:
// the exchange is the point, the answer is never parsed (the phone's DNS
// proxy resolving it upstream is the pre-warming side effect). Rate-limits
// itself, so call sites may invoke it every iteration. No-op unless WiFi is
// associated; callers guarantee the caster is disconnected.
static void warmHotspotPath(const char *host)
{
  static uint32_t lastWarm_ms = 0;
  static WiFiUDP warmUdp;
  static bool warmUdpReady = false;
  static uint16_t warmQueryId = 0;

  if (!WiFi.isConnected()) return;
  if (millis() - lastWarm_ms < HOTSPOT_WARM_INTERVAL_MS) return;
  lastWarm_ms = millis();

  if (!warmUdpReady) warmUdpReady = warmUdp.begin(0) != 0;  // ephemeral port
  if (!warmUdpReady) return;

  // Discard the previous warm's reply (never parsed, see above).
  while (warmUdp.parsePacket() > 0) warmUdp.flush();

  IPAddress dnsServer = WiFi.dnsIP();
  if (dnsServer == IPAddress()) return;

  // DNS header: id, RD flag, one question.
  uint8_t query[12 + 260];
  size_t len = 0;
  warmQueryId++;
  query[len++] = warmQueryId >> 8;
  query[len++] = warmQueryId & 0xFF;
  query[len++] = 0x01;  // flags: recursion desired
  query[len++] = 0x00;
  query[len++] = 0x00;  // QDCOUNT = 1
  query[len++] = 0x01;
  memset(query + len, 0, 6);  // AN/NS/ARCOUNT = 0
  len += 6;
  // QNAME: dotted host as length-prefixed labels
  for (const char *p = host; *p != '\0'; )
  {
    const char *dot = strchr(p, '.');
    size_t label = dot ? (size_t)(dot - p) : strlen(p);
    if (label == 0 || label > 63 || len + label + 1 + 5 > sizeof(query)) return;
    query[len++] = (uint8_t)label;
    memcpy(query + len, p, label);
    len += label;
    p += label + (dot ? 1 : 0);
  }
  query[len++] = 0x00;  // root label
  query[len++] = 0x00;  // QTYPE = A
  query[len++] = 0x01;
  query[len++] = 0x00;  // QCLASS = IN
  query[len++] = 0x01;

  warmUdp.beginPacket(dnsServer, 53);
  warmUdp.write(query, len);
  bool sent = warmUdp.endPacket() != 0;
  DBG.printf("hotspot path warmer: DNS query for %s -> %s\n",
             host, sent ? "sent" : "send failed");
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


  while (true)
  {
    telemetryNotePositionLoop();  // heartbeat liveness counter (key 19)

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
  // No-RTCM hangup window. Starts at the post-connect grace (the VRS needs
  // our GGA before it streams; 2026-08-24: a fixed 10 s window expired inside
  // the post-connect mutex wait and killed every session in the iteration
  // that opened it), tightens to NTRIP_RTCM_TIMEOUT_MS once data flows.
  uint32_t rtcmTimeout_ms = NTRIP_CONNECT_GRACE_MS;

  // Reconnect backoff (caster etiquette, refnet throttles reconnect floods):
  // attemptDelay_ms is waited before the next connect attempt; it is armed
  // from reconnectDelay_ms at every failed or dataless attempt, which then
  // doubles up to the cap. Received RTCM resets both.
  uint32_t reconnectDelay_ms = NTRIP_BACKOFF_START_MS;
  uint32_t attemptDelay_ms = 0;
  bool gotDataThisSession = false;

  // GNSS receiver recovery ladder (bench 4.1, 2026-08-24: module went fully
  // mute for 14+ min, no self-recovery). Stage escalates per attempt:
  // 0 = reconfigure, 1 = GNSS software reset, 2+ = hard reset (cold start).
  // A real GGA (lastGgaHeard_ms) resets the stage.
  uint8_t gnssRecoveryStage = 0;
  uint32_t gnssRecoveryCount = 0;
  uint32_t lastGnssRecovery_ms = 0;
  lastGgaHeard_ms = millis();  // arm the liveness clock at task start

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
    telemetryNoteNtripLoop();  // heartbeat liveness counter (key 18)

    // gnss_pipe_stall instrumentation: phase timers for this iteration.
    // Whatever exceeds GNSS_PIPE_STALL_MS in one pass is emitted as a sev-1
    // error at the bottom of the loop (2026-08-21 slowdown diagnosis).
    uint32_t iterStart_ms = millis();
    uint32_t mutexWaitMs = 0, ubxMs = 0, pushMs = 0, ggaMs = 0;

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

    // Dispatch pending NMEA callbacks at the loop top so callbackGPGGA runs
    // in EVERY iteration, including receiver-silent ones that skip the
    // caster below: it feeds the GGA push, and it is the liveness signal
    // (lastGgaHeard_ms) the gate and recovery ladder depend on. Must stay
    // OUTSIDE mutexSem: no I2C here, and callbackGPGGA takes the
    // (non-recursive) mutex itself.
    myGNSS.checkCallbacks();

    // --- GNSS receiver watchdog / recovery ladder --------------------------
    if (millis() - lastGgaHeard_ms <= GNSS_SILENT_AFTER_MS)
    {
      gnssRecoveryStage = 0;  // receiver talking: episode over (if any)
    }
    else
    {
      // Receiver silent. Keep the parser fed from here too: the position
      // task polls as well (unconditionally since the boot-deadlock fix),
      // but this pass is cheap while mute (no bytes) and guarantees the
      // revived stream gets parsed for lastGgaHeard_ms to recover even if
      // that task is wedged on a degraded bus.
      uint32_t phase_ms = millis();
      if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GNSS_MUTEX_TIMEOUT_MS)))
      {
        mutexWaitMs += millis() - phase_ms;
        phase_ms = millis();
        myGNSS.checkUblox();
        ubxMs += millis() - phase_ms;
        xSemaphoreGive(mutexSem);
      }
      else
      {
        mutexWaitMs += millis() - phase_ms;
      }
      myGNSS.checkCallbacks();

      if (millis() - lastGnssRecovery_ms >= GNSS_RECOVERY_GAP_MS)
      {
        lastGnssRecovery_ms = millis();
        gnssRecoveryCount++;
        uint32_t silentFor_s = (millis() - lastGgaHeard_ms) / 1000;
        const char *action = "skipped (mutex busy)";
        bool attempted = false;
        bool configured = false;
        phase_ms = millis();
        if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GNSS_RECOVERY_MUTEX_MS)))
        {
          mutexWaitMs += millis() - phase_ms;
          attempted = true;
          switch (gnssRecoveryStage)
          {
            case 0:
              action = "reconfigure";
              configured = configureGNSS();
              break;
            case 1:
              action = "sw reset";
              myGNSS.softwareResetGNSSOnly();
              vTaskDelay(2000/portTICK_PERIOD_MS);  // module restart time
              configured = configureGNSS();
              break;
            default:
              action = "hard reset";
              myGNSS.hardReset();  // cold start: last resort, loses ephemeris
              vTaskDelay(2000/portTICK_PERIOD_MS);
              configured = configureGNSS();
              break;
          }
          xSemaphoreGive(mutexSem);
          if (gnssRecoveryStage < 2) gnssRecoveryStage++;
        }
        else
        {
          mutexWaitMs += millis() - phase_ms;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "receiver silent %u s, recovery #%u: %s%s",
                 (unsigned)silentFor_s, (unsigned)gnssRecoveryCount, action,
                 !attempted ? "" : (configured ? " ok" : " (module not answering)"));
        telemetryEmitError(2, "gnss_degraded", msg);
        DBG.printf("gnss_degraded: %s\n", msg);
        // Deliberate maintenance (includes the 2 s reset wait), not a stall.
        iterStart_ms = millis();
      }
    }

    /*
    This ist most of the content beginServing() func from the
    Sparkfun u-blox GNSS Arduino Library/ZED-F9P/Example15-NTRIPClient
    Because I did not wanted to change the code too much if you want to compare
    with the Example14: "continue" calls are used in place of "return".
    (A task must not return.)
    */

    if (ntripClient.connected() == false)
    {
      // First check WiFi connection. Wait SOFTLY: auto-reconnect is on, so
      // the driver keeps retrying by itself; every WIFI_RECONNECT_NUDGE_MS
      // we kick it with WiFi.reconnect() (plain disconnect+connect, no
      // teardown). The previous full setupStationMode() per retry cycled a
      // complete driver deinit/init every ~12 s, which leaked ~48 B/cycle
      // (-14.5 kB/h, measured bench 2+4 2026-08-24: OOM after ~1 h of
      // continuous hotspot loss) and transiently dipped free heap by
      // several kB per cycle. It remains only as a rare escape hatch for a
      // wedged driver, after WIFI_REINIT_AFTER_MS without association.
      uint32_t wifiDown_ms = millis();
      uint32_t lastNudge_ms = millis();
      uint32_t nudgeDelay_ms = WIFI_RECONNECT_NUDGE_MS;
      bool wifiWaited = false;
      while (!WiFi.isConnected())
      {
        wifiWaited = true;
        DBG.println(F("task_rtk_get_corr_data loop: Not connected to WiFi station"));
        DBG.printf("WiFi state: %d", WiFi.status());
        DBG.println();
        // Report only once the outage has outlived the grace.
        if (!wifiLossEmitted && millis() - wifiDown_ms >= WIFI_LOSS_REPORT_AFTER_MS)
        {
          wifiLossEmitted = true;
          telemetryEmitError(1, "wifi_disconnected", "hotspot lost, reconnecting");
        }
        if (millis() - wifiDown_ms >= WIFI_REINIT_AFTER_MS)
        {
          wifiDown_ms = millis();
          lastNudge_ms = millis();
          nudgeDelay_ms = WIFI_RECONNECT_NUDGE_MS;  // fresh driver, fresh ladder
          DBG.println(F("WiFi down for minutes, full driver re-init"));
          setupStationMode(kWifiSsid, kWifiPw);
        }
        else if (millis() - lastNudge_ms >= nudgeDelay_ms)
        {
          lastNudge_ms = millis();
          DBG.printf("WiFi soft reconnect nudge (next in %u ms)\n", nudgeDelay_ms);
          WiFi.reconnect();
          if (nudgeDelay_ms < WIFI_RECONNECT_NUDGE_MAX_MS)
          {
            nudgeDelay_ms = min(nudgeDelay_ms * 2, (uint32_t)WIFI_RECONNECT_NUDGE_MAX_MS);
          }
        }
        blinkOneTime(1000, false);
        blinkOneTime(100, false);
      }
      wifiLossEmitted = false;
      if (wifiWaited)
      {
        // A WiFi outage is reported by wifi_disconnected, not
        // gnss_pipe_stall: restart the iteration clock so outage time
        // doesn't count as a stall. And while blocked above,
        // checkCallbacks never ran, so the liveness clock is stale
        // regardless of the receiver's health. Re-arm it: the receiver
        // gets GNSS_SILENT_AFTER_MS to prove itself before gate/ladder act.
        iterStart_ms = millis();
        lastGgaHeard_ms = millis();
      }

      // WiFi associated, caster disconnected: keep the hotspot's upstream
      // path awake — nothing else is generating traffic in this state, and
      // the gates below can hold us here for minutes. (Self rate-limited.)
      warmHotspotPath(casterHost.c_str());

      // Receiver-liveness gate: a mute F9P produces no GGA, and the VRS
      // streams nothing without one. Connecting would only cycle dataless
      // sessions against the caster (~93 s cycle observed, bench 4.1). The
      // recovery ladder above owns this state; skip caster attempts until
      // the receiver talks again. (checkCallbacks/ladder already ran this
      // iteration, so pacing out via the loop is safe.)
      if (millis() - lastGgaHeard_ms > GNSS_SILENT_AFTER_MS)
      {
        DBG.println(F("NTRIP connect skipped: receiver silent"));
        vTaskDelay(TASK_WIFI_RTK_DATA_INTERVAL_MS/portTICK_PERIOD_MS);
        continue;
      }

      // Fix gate: don't open a session before the receiver has a usable
      // position for the VRS. The receiver's health is visible regardless -
      // the position task emits gnss_fix (fix_type 0) throughout acquisition.
      // 0 = no fix-quality GGA seen since boot.
      if (lastFixGgaHeard_ms == 0 ||
          millis() - lastFixGgaHeard_ms > NTRIP_GGA_FIX_MAX_AGE_MS)
      {
        DBG.println(F("NTRIP connect skipped: no GNSS fix"));
        vTaskDelay(TASK_WIFI_RTK_DATA_INTERVAL_MS/portTICK_PERIOD_MS);
        continue;
      }

      if (successfulConnects > 0 && !reconnectingEmitted)
      {
        // Once per outage, and only after a previous connection: the
        // initial connect is not a "reconnecting" transition.
        reconnectingEmitted = true;
        telemetryEmitNtripStatus(TELEM_NTRIP_RECONNECTING,
                                 successfulConnects - 1,
                                 telemetryRtcmBytesTotal());
      }

      // Backoff: armed by the previous failed or dataless attempt. Waiting
      // here (single site) keeps every retry path (TCP fail, caster timeout,
      // bad response, dataless hangup) on the same schedule.
      if (attemptDelay_ms > 0)
      {
        DBG.printf("NTRIP backoff: waiting %u ms before reconnect\n", attemptDelay_ms);
        // Sliced sleep: the warmer must keep its cadence through this wait
        // (up to NTRIP_BACKOFF_MAX_MS in one go) — these gaps are exactly
        // where the hotspot dozes off.
        uint32_t waited_ms = 0;
        while (waited_ms < attemptDelay_ms)
        {
          uint32_t slice_ms = min(attemptDelay_ms - waited_ms, (uint32_t)1000);
          vTaskDelay(slice_ms/portTICK_PERIOD_MS);
          waited_ms += slice_ms;
          warmHotspotPath(casterHost.c_str());
        }
        attemptDelay_ms = 0;
        iterStart_ms = millis();  // deliberate pacing, not a pipeline stall
      }

      DBG.print(F("Opening socket to "));
      DBG.println(casterHost.c_str());

      // Attempt connection
      if (ntripClient.connect( casterHost.c_str(), (uint16_t)casterPort.toInt() ) == false)
      {
        DBG.println(F("Connection to caster failed"));
        if (!outageErrorEmitted)
        {
          outageErrorEmitted = true;
          telemetryEmitError(1, "ntrip_connect_failed", "TCP connect to caster failed");
        }
        attemptDelay_ms = reconnectDelay_ms;
        reconnectDelay_ms = min(reconnectDelay_ms * 2, (uint32_t)NTRIP_BACKOFF_MAX_MS);
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
          telemetrySetNtripConnected(false);
          attemptDelay_ms = reconnectDelay_ms;
          reconnectDelay_ms = min(reconnectDelay_ms * 2, (uint32_t)NTRIP_BACKOFF_MAX_MS);
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

        // Wait for response.
        // The timeout must leave this wait loop before retrying.
        unsigned long timeout = millis();
        bool casterTimedOut = false;
        while (ntripClient.available() == 0)
        {
          if (millis() - timeout > CONNECTION_TIMEOUT_MS)
          {
            ntripClient.stop(); // Too many requests with wrong settings will lead to bann, stop here
            DBG.println(F("Caster timed out!"));
            casterTimedOut = true;
            break;
          }
          vTaskDelay(1000/portTICK_PERIOD_MS);
        }
        if (casterTimedOut)
        {
          telemetrySetNtripConnected(false);  // stop() happened in the wait loop
          if (!outageErrorEmitted)
          {
            outageErrorEmitted = true;
            telemetryEmitError(1, "ntrip_connect_failed", "caster response timeout");
          }
          attemptDelay_ms = reconnectDelay_ms;
          reconnectDelay_ms = min(reconnectDelay_ms * 2, (uint32_t)NTRIP_BACKOFF_MAX_MS);
          continue; // skip to next iteration and retry
        }

        // Check reply
        bool connectionSuccess = false;
        char response[512];
        int responseSpot = 0;

        while (ntripClient.available())
        {
          if (responseSpot == sizeof(response) - 1) break;

          response[responseSpot++] = ntripClient.read();
        }
        response[responseSpot] = '\0';

        if (strstr(response, "SOURCETABLE") != NULL ||
            strstr(response, "sourcetable") != NULL)
        {
          DBG.println(F("Caster returned its source table - mount point unknown to the caster"));
        }
        else if (strstr(response, "401") != NULL) // '401 Unauthorized'
        {
          DBG.println(F("Your credentials look bad!\nCheck you caster username, password and ban status (got email from rtk2go?)"));
        }
        else if (strstr(response, "200") != NULL) // 'ICY 200 OK' / 'HTTP/1.1 200 OK'
        {
          connectionSuccess = true;
        }

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
          attemptDelay_ms = reconnectDelay_ms;
          reconnectDelay_ms = min(reconnectDelay_ms * 2, (uint32_t)NTRIP_BACKOFF_MAX_MS);
          continue; // skip to next iteration and retry
        }
        else
        {
          DBG.print(F("Connected to "));
          DBG.println(casterHost.c_str());
          lastReceivedRTCM_ms = millis();
          // Fresh session: full grace window until the first RTCM (the VRS
          // streams only after our GGA), and nothing received yet.
          rtcmTimeout_ms = NTRIP_CONNECT_GRACE_MS;
          gotDataThisSession = false;
          telemetrySetNtripConnected(true);

          successfulConnects++;
          outageErrorEmitted = false;
          reconnectingEmitted = false;
          telemetryEmitNtripStatus(TELEM_NTRIP_CONNECTED,
                                   successfulConnects - 1,
                                   telemetryRtcmBytesTotal());

          // Expire the GGA gate so the push block later in this iteration
          // sends a GGA as soon as one is complete: the VRS computes the
          // virtual station from it and streams nothing until it arrives.
          lastTransmittedGGA_ms = millis() - timeBetweenGGAUpdate_ms - 1;

          // checkUblox under mutexSem like every other myGNSS I2C access:
          // this runs on core 0 while the position task polls the same
          // object/bus from its own loop - unsynchronized access desyncs
          // the UBX parser and stalled the position getters for seconds
          // (measured 8.8 s, 2026-07-29). Bounded take: on timeout skip the
          // pass: the position task's own passes keep the parser fed, and
          // this task must reach its read/GGA sections while the grace
          // window is still open. checkCallbacks must stay outside the
          // mutex: it touches no I2C, and it invokes callbackGPGGA, which
          // takes mutexSem itself (non-recursive - taking it here would
          // self-deadlock this task and starve positioning).
          uint32_t phase_ms = millis();
          if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GNSS_MUTEX_TIMEOUT_MS)))
          {
            mutexWaitMs += millis() - phase_ms;
            phase_ms = millis();
            myGNSS.checkUblox();
            ubxMs += millis() - phase_ms;
            xSemaphoreGive(mutexSem);
          }
          else
          {
            mutexWaitMs += millis() - phase_ms;
          }
          myGNSS.checkCallbacks();
        }
      } // End attempt to connect
    } // End connected == false

    if (ntripClient.connected() == true)
    {
      uint8_t rtcmData[512 * 4]; // Most incoming data is around 500 bytes but may be larger

      // Drain the socket completely, in buffer-sized slices. A single-buffer
      // read left the rest queued in lwIP when an iteration ran slow, up to
      // the 5.7 kB TCP window pinned in pbufs (the observed heap dips), and a
      // zero-window stall toward the caster. The byte cap is a backstop
      // against a flooding caster, not an expected limit.
      uint32_t drainedTotal = 0;
      while (ntripClient.available() && drainedTotal < NTRIP_DRAIN_MAX_BYTES)
      {
        rtcmCount = 0;
        while (ntripClient.available() && rtcmCount < (long)sizeof(rtcmData))
        {
          rtcmData[rtcmCount++] = ntripClient.read();
        }
        drainedTotal += rtcmCount;

        // The link is alive: note that independently of whether the push
        // below wins the mutex. First data also ends the post-connect
        // grace and resets the reconnect backoff.
        lastReceivedRTCM_ms = millis();
        if (!gotDataThisSession)
        {
          gotDataThisSession = true;
          rtcmTimeout_ms = NTRIP_RTCM_TIMEOUT_MS;
          reconnectDelay_ms = NTRIP_BACKOFF_START_MS;
        }

        //Push RTCM to GNSS module over I2C. Bounded take: when the position
        //task is in a slow-I2C stretch, dropping one redundant correction
        //slice beats stalling the link (the 12-26 s portMAX_DELAY waits here
        //are what killed every session on 2026-08-24).
        uint32_t phase_ms = millis();
        if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GNSS_MUTEX_TIMEOUT_MS)))
        {
          mutexWaitMs += millis() - phase_ms;
          phase_ms = millis();
          myGNSS.pushRawData(rtcmData, rtcmCount, false);
          pushMs += millis() - phase_ms;
          telemetryNoteRtcmPushed(rtcmCount);  // feeds corr_age_ms + bytes_rx
          xSemaphoreGive(mutexSem);
          DBG.print(F("RTCM pushed to ZED: "));
          DBG.println(rtcmCount);
        }
        else
        {
          mutexWaitMs += millis() - phase_ms;
          DBG.print(F("RTCM slice dropped (mutex busy): "));
          DBG.println(rtcmCount);
        }
      }
    }   // End (ntripClient.connected() == true)

    // (NMEA callbacks are dispatched at the loop top, before the
    // receiver-liveness gate — see there.)

    //Provide the caster with our current position as needed
    if (ntripClient.connected() == true && (millis() - lastTransmittedGGA_ms) > timeBetweenGGAUpdate_ms)
    {
      char localGgaSentence[NMEA_GGA_MAX_LENGTH] = {0};
      bool shouldSendGga = false;

      // Bounded take; on timeout the gate is left expired, so the next
      // iteration (~1 s) retries instead of waiting the full 10 s period.
      uint32_t phase_ms = millis();
      if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GGA_MUTEX_TIMEOUT_MS)))
      {
        mutexWaitMs += millis() - phase_ms;
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
      else
      {
        mutexWaitMs += millis() - phase_ms;
      }

      if (shouldSendGga)
      {
        DBG.print(F("Pushing GGA to server: "));
        DBG.println(localGgaSentence);

        //Push our current GGA sentence to caster
        phase_ms = millis();
        ntripClient.print(localGgaSentence);
        ntripClient.print("\r\n");
        ggaMs += millis() - phase_ms;
      }
    }

    // Close socket if the no-RTCM window expired (30 s grace right after a
    // connect, 10 s once data has flowed)
    if (millis() - lastReceivedRTCM_ms > rtcmTimeout_ms)
    {
      DBG.println(F("RTCM timeout. Disconnecting..."));
      if (ntripClient.connected() == true)
      {
        // Socket up but no corrections. This is the signature of a
        // correction-delivery problem (vs. GNSS degradation, PROJECT-PLAN par. 2)
        char msg[64];
        snprintf(msg, sizeof(msg), "no RTCM for %u s, dropping caster connection",
                 (unsigned)(rtcmTimeout_ms / 1000));
        telemetryEmitError(1, "ntrip_rtcm_timeout", msg);
        ntripClient.stop();
        telemetrySetNtripConnected(false);
        if (!gotDataThisSession)
        {
          // A session that never delivered a byte counts as a failed
          // attempt: back off before hammering the caster again.
          attemptDelay_ms = reconnectDelay_ms;
          reconnectDelay_ms = min(reconnectDelay_ms * 2, (uint32_t)NTRIP_BACKOFF_MAX_MS);
        }
      }
    }

    // Measure stack size (last was 19320)
    // uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
    // DBG.print(F("task_rtk_get_corrrection_data loop, uxHighWaterMark: "));
    // DBG.println(uxHighWaterMark);
    // } /*** End if (xSemaphoreTake(mutexSem, portMAX_DELAY)) ***/

    // gnss_pipe_stall: a whole iteration over threshold gets reported with
    // its phase breakdown (any remainder beyond the four phases is connect /
    // response-wait time). Iterations that `continue` above skip this on
    // purpose: their delays are deliberate retry pacing.
    uint32_t iterMs = millis() - iterStart_ms;
    if (iterMs > GNSS_PIPE_STALL_MS)
    {
      static uint32_t lastStallEmit_ms = 0;
      if (millis() - lastStallEmit_ms >= GNSS_PIPE_STALL_GAP_MS)
      {
        lastStallEmit_ms = millis();
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "ntrip iter %u ms (mutex %u, ubx %u, push %u, gga %u)",
                 (unsigned)iterMs, (unsigned)mutexWaitMs, (unsigned)ubxMs,
                 (unsigned)pushMs, (unsigned)ggaMs);
        telemetryEmitError(1, "gnss_pipe_stall", msg);
      }
    }

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
    HEADTRACKER_BIN_CHARACTERISTIC_UUID,
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
  // Advertised connection-interval preference, units of 1.25 ms.
  // Only a hint - iOS chooses the actual interval.
  //
  // Don't request a faster interval via esp_ble_gap_update_conn_params:
  // a granted 15-30 ms request starved the WiFi side through radio coex
  // and killed the NTRIP stream completely. The head-tracking
  // cost of the default interval is small: notifies queue in the controller
  // and flush together each connection event, so the newest frame still
  // arrives every event.
  pAdvertising->setMinPreferred(0x12);  // 22.5 ms
  pAdvertising->setMaxPreferred(0x24);  // 45 ms
  //pAdvertising->start();
  BLEDevice::startAdvertising();
  DBG.println(F("Characteristic defined! Now you can read it in your phone!"));
}

void setupBNO080()
{
  Wire.begin();
  // 400 kHz, same as GNSS bus (Wire1)
  Wire.setClock(I2C_FREQUENCY_400K);
  bool beginFailEmitted = false;  // one error event per setup, not per retry
  while (!bno080.begin())
  {
    // Wait
    DBG.println(F("BNO080 not ready, waiting for I2C..."));
    if (!beginFailEmitted)
    {
      beginFailEmitted = true;
      telemetryEmitError(3, "i2c_bno080_not_detected", "BNO080 begin() failing, check wiring");
    }
    delay(500);
  }

  // Activate IMU functionalities
  bno080.enableARVRStabilizedRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
  // bno080.enableARVRStabilizedGameRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
  // bno080.enableRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
  // bno080.enableGameRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);

  // Only the linear accelerometer is consumed (getLinAccelZ for step
  // detection); the raw accelerometer report would be a third 100 Hz report
  // stream competing for the same I2C drain budget.
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

// Binary heading frame on HEADTRACKER_BIN_CHARACTERISTIC_UUID
// (cross-repo contract with rwa-player and rwa-creator):
// little-endian, quaternion components of the ARVR-stabilized
// rotation vector in Q14 (unit-length, so +/-1.0 -> +/-16384), linear
// acceleration z in cm/s^2. seq restarts every boot, like dev_seq.
typedef struct __attribute__((packed))
{
  uint16_t seq;
  uint32_t t_dev_ms;
  int16_t qi;
  int16_t qj;
  int16_t qk;
  int16_t qw;
  int16_t linAccelZ_cms2;
} heading_frame_t;
static_assert(sizeof(heading_frame_t) == 16, "heading frame is a 16-byte wire contract");

static inline int16_t headingScaledInt16(float v, float scale)
{
  float scaled = v * scale;
  if (scaled > 32767.0f) scaled = 32767.0f;
  if (scaled < -32768.0f) scaled = -32768.0f;
  return (int16_t)lroundf(scaled);
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

#if DEBUGGING
  // Latency instrumentation: per-second poll/consume stats. A sensor-side
  // report backlog shows up as consumed << produced (~200/s with rotation
  // vector + linear accel at 100 Hz each); read cost tracks the I2C bus speed.
  uint32_t bnoTicks = 0, bnoReports = 0, bnoMisses = 0, bnoMaxDrain = 0;
  uint32_t bnoLastRead_us = 0, bnoLastStats_ms = millis();
#endif

  heading_frame_t headingFrame = {};

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

#if DEBUGGING
      bnoTicks++;
      if (millis() - bnoLastStats_ms >= 1000)
      {
        DBG.printf("bno stats: ticks %u, reports %u, misses %u, max drain %u, last drain %u us\n",
                   bnoTicks, bnoReports, bnoMisses, bnoMaxDrain, bnoLastRead_us);
        bnoTicks = bnoReports = bnoMisses = bnoMaxDrain = 0;
        bnoLastStats_ms = millis();
      }
      uint32_t bnoReadStart_us = micros();
#endif

      // Drain the sensor-side queue and use only the newest values: the
      // BNO080 produces reports faster than one per tick, and a backlog in
      // its FIFO is delivered oldest-first, i.e. as stale orientation. Each
      // dataAvailable() consumes one SHTP report into the library's cached
      // values; after the drain those hold the freshest quaternion/accel.
      uint8_t drained = 0;
      while (drained < BNO080_DRAIN_MAX_REPORTS && bno080.dataAvailable())
      {
        drained++;
      }
#if DEBUGGING
      bnoLastRead_us = micros() - bnoReadStart_us;
      bnoReports += drained;
      if (drained > bnoMaxDrain) bnoMaxDrain = drained;
      if (drained == 0) bnoMisses++;
#endif

      // A tick with no report just waits the normal tick delay: any extra
      // wait here is a head-tracking freeze (a miss used to stall 1 s).
      if (drained > 0)
      {
        imuSampleCount++;
        // Raw quaternion on the wire; the apps do the (identical, spec'd)
        // quat->azimuth/elevation math on their hardware FPUs.
        headingFrame.seq++;
        headingFrame.t_dev_ms = millis();
        headingFrame.qi = headingScaledInt16(bno080.getQuatI(), 16384.0f);
        headingFrame.qj = headingScaledInt16(bno080.getQuatJ(), 16384.0f);
        headingFrame.qk = headingScaledInt16(bno080.getQuatK(), 16384.0f);
        headingFrame.qw = headingScaledInt16(bno080.getQuatReal(), 16384.0f);
        headingFrame.linAccelZ_cms2 = headingScaledInt16(bno080.getLinAccelZ(), 100.0f);
        pHeadtrackerCharacteristic->setValue((uint8_t *)&headingFrame, sizeof(headingFrame));
        pHeadtrackerCharacteristic->notify();
      }
      }
      vTaskDelay(TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS/portTICK_PERIOD_MS);
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
