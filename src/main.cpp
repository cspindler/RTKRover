
/*******************************************************************************
 * @file main.cpp
 * @authors Markus Hädrich
 * <br>
 * @brief This is part of a distributed software, here: head tracker and GNSS
 *        positioning using Sparkfun Real Time Kinematics
 * <br>
 * @todo  - Upgrade to Sparkfun RTK Library v3
 *
 * @version 0.47.0
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
#include <led.h>
#include <handle_wifi.h>
#include <WiFiUdp.h> // hotspot path warmer (warmHotspotPath)
#include <telemetry/telemetry.h>
#include <telemetry/telemetry_ble.h>
#include <ble_link.h>
#ifdef TESTING
#include <TestsRTKRover.h>  // AUnit suites; debug builds only
#endif

/*
=================================================================================
                                Bluetooth LE
=================================================================================
*/
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

// Heap diagnostics (debug builds): connect time is the critical moment —
// Bluedroid allocates the GATT connection control block on the BT task, and
// an allocation failure there escalates to a vQueueDelete(NULL) panic
// (fixed_queue_new error path, observed 2026-07-29). Track the margin.
static void logFreeHeap(const char *where)
{
  DBG.printf("heap @ %s: free %u, min ever %u\n",
             where, esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
}

BLECharacteristic *pHeadtrackerCharacteristic;
BLECharacteristic *pRealtimeKinematicsCharacteristic;

/*
Notify pacing for the heading stream (PROJECT-PLAN.md par. 5.5).

Nothing leaves the device between connection events: the central anchors the
link at the negotiated connection interval, and a notify() call only enqueues
a buffer for the next one. Sampling faster than that interval therefore cannot
make the data arrive sooner, it only queues frames that ride out in the same
burst, where the app renders the newest and discards the rest. Each of those
discarded frames still costs airtime (a longer connection event is a longer
WiFi blackout through radio coex) and still holds a Bluedroid TX buffer, which
is heap.

So: keep draining the IMU every tick, keep the cached frame always fresh, and
put exactly one frame on the wire per connection event. The interval is the
iOS central's choice; ble_link learns it from the stack (bleLinkConnIntervalUnits)
and the task paces against it, which adapts to whatever iOS picks instead of
hardcoding a guess.
*/

/**
 * @brief Notify period to pace the heading stream against, in ms.
 *
 * One tick short of the granted connection interval, so every connection event
 * finds a frame refreshed since the last one without a second frame queueing
 * behind it. Falls back to a fast default until the stack reports the interval,
 * so a central that never triggers the event cannot make head tracking sluggish.
 */
static uint32_t headingNotifyPeriodMs(void)
{
  const uint16_t units = bleLinkConnIntervalUnits();
  if (units == 0) return HEADING_NOTIFY_FALLBACK_MS;

  const uint32_t interval_ms = (units * 5u) / 4u;  // 1.25 ms units
  const uint32_t tick_ms = TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS;
  const uint32_t period = (interval_ms > tick_ms) ? interval_ms - tick_ms : tick_ms;
  return constrain(period, (uint32_t)HEADING_NOTIFY_MIN_MS, (uint32_t)HEADING_NOTIFY_MAX_MS);
}

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
#include "base64.h" // ESP32 core base64, for the NTRIP basic-auth header

SFE_UBLOX_GNSS myGNSS;

// High-precision coordinate: UBX 1e-7 deg + 1e-9 high-res part, the units
// the 713D0004 line carries.
typedef struct Coord
{
  int32_t lat;
  int8_t  latHp;
  int32_t lon;
  int8_t  lonHp;
} coord_t;

/**
 * @brief Setup the ZED-F9D to a rover
 *
 * @return true If succeeded
 * @return false If failed
 */
bool setupGNSS(void);

/**
 * @brief One position-pipeline pass under mutexSem: reads the streamed NAV
 * packets, emits the 1 Hz gnss_fix event, and returns the coordinate to
 * stream when the solution is fresh and within MIN_ACCEPTABLE_ACCURACY_MM.
 *
 * @return true if *out holds a position the walk may trust
 */
static bool updatePosition(coord_t *out);

/*
=================================================================================
                                FreeRTOS
=================================================================================
*/
static xSemaphoreHandle mutexSem;

/**
 * @brief Task to get the correction data from the caster server
 *        using WiFi
 *
 * @param pvParameters Void pointer, no parameter used here
 */
void task_rtk_get_corrrection_data(void *pvParameters);

/**
 * @brief Task for the position pipeline: updatePosition() under the mutex,
 *        then the ASCII position to the iPhone on 713D0004
 *
 * @param pvParameters Void pointer, no parameter used here
 */
void task_rtk_get_rover_position(void *pvParameters);

/**
 * @brief Task for sending the BNO080 position data to the
 *        iPhone using BLE
 *
 * @param pvParameters Void pointer, no parameter used here
 */
void task_bno_orientation_via_ble(void *pvParameters);

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
  ledInit();  // blink codes are listed in README.md

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
  /*
  Sizes from the watermark measurements (debug loop() prints "stack min free"
  every 10 s = bytes of stack never touched). Kept margin is ~2 KB over
  observed peak use; the FreeRTOS stack canary turns an undersized stack into
  a loud "Stack canary watchpoint triggered" panic on the bench, not silent
  corruption. 2026-07-29: total 21 KB, down from 35 KB - the freed 14 KB of
  heap is what ended the connect-time OOM panics (vQueueDelete assert /
  lock_init_generic abort). 2026-09-11: 17 KB, the position sender task and
  its queue merged into the position task.
  */
  int stack_size_task_rtk_get_corrrection_data = 1024 * 9;       // min free 2792 (2026-09-11)
  int stack_size_task_rtk_get_rover_position = 1024 * 4;         // min free 2344 before the merge; notify() added
  int stack_size_task_bno_orientation_via_ble = 1024 * 4;        // min free 2080

  xTaskCreatePinnedToCore( &task_rtk_get_corrrection_data, "task_rtk_get_corrrection_data", stack_size_task_rtk_get_corrrection_data, NULL, TASK_RTK_GET_CORR_DATA_PRIORITY, &hTaskCorrData, RUNNING_CORE_0);
  xTaskCreatePinnedToCore( &task_rtk_get_rover_position, "task_rtk_get_rover_position", stack_size_task_rtk_get_rover_position, NULL, TASK_RTK_GET_POSITION_PRIORITY, &hTaskPosition, RUNNING_CORE_0);
  xTaskCreatePinnedToCore( &task_bno_orientation_via_ble, "task_bno_orientation_via_ble", stack_size_task_bno_orientation_via_ble, NULL, TASK_BNO080_VIA_BLE_PRIORITY, &hTaskBnoBle, RUNNING_CORE_1);

  // (Telemetry drain task is started right after setupBLE() above, so sensor
  // failures during setup are already visible in diagnostics.)

  String thisBoard = ARDUINO_BOARD;
  DBG.print(F("Setup done on "));
  DBG.println(thisBoard);
  logFreeHeap("setup_done");
} /*** end setup ***/

void loop()
{
  // "Waiting for a phone" blink code (README.md): 0.1 s on / 0.1 s off while
  // no BLE central is connected. Otherwise idle; the delay keeps loopTask
  // from spinning at priority 1 against the telemetry drain.
  if (!bleLinkConnected()) blinkOneTime(100, true);
  else vTaskDelay(100/portTICK_PERIOD_MS);

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
    DBG.printf("stack min free: corr %u, pos %u, bno %u, telem %u, loop %u\n",
               hTaskCorrData ? uxTaskGetStackHighWaterMark(hTaskCorrData) : 0,
               hTaskPosition ? uxTaskGetStackHighWaterMark(hTaskPosition) : 0,
               hTaskBnoBle ? uxTaskGetStackHighWaterMark(hTaskBnoBle) : 0,
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
 * @return NULL when the module answered and every config write was
 *         acknowledged; otherwise the name of the step that failed. "begin"
 *         means the module is not answering at all (wiring); any other name
 *         is one config write the module did not ACK, which every caller
 *         simply retries.
 */
static const char *configureGNSS()
{
    if (myGNSS.begin(Wire1, RTK_I2C_ADDR) == false)
      return "begin";

    // Fewer, larger I2C transactions: 4x fewer start/stop cycles for the
    // same data (library default is 32; the lib itself recommends 128 on
    // ESP32, whose Wire buffer is 128 B).
    myGNSS.setI2CTransactionSize(128);

    // Stop at the first write the module does not ACK and name it: a bare
    // false hid which one (2026-09-11 captures: a first-pass failure on
    // every boot, always cleared by the retry).
#define GNSS_CFG_STEP(name, call) do { if (!(call)) return name; } while (0)
    GNSS_CFG_STEP("setI2COutput", myGNSS.setI2COutput(COM_TYPE_UBX | COM_TYPE_NMEA)); // I2C port outputs both NMEA and UBX
    GNSS_CFG_STEP("setPortInput", myGNSS.setPortInput(COM_PORT_I2C, COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3)); // RTCM3 input on. UBX + RTCM3 alone is not a valid state.
    GNSS_CFG_STEP("setDGNSSConfiguration", myGNSS.setDGNSSConfiguration(SFE_UBLOX_DGNSS_MODE_FIXED)); // ambiguities fixed whenever possible
    GNSS_CFG_STEP("enableNMEAMessage", myGNSS.enableNMEAMessage(UBX_NMEA_GGA, COM_PORT_I2C)); // GGA sentence enabled
    GNSS_CFG_STEP("setHighPrecisionMode", myGNSS.setHighPrecisionMode(true));
    GNSS_CFG_STEP("setMainTalkerID", myGNSS.setMainTalkerID(SFE_UBLOX_MAIN_TALKER_ID_GP)); // GPGGA instead of GNGGA

    // Solution output rate in Hz.
    GNSS_CFG_STEP("setNavigationFrequency", myGNSS.setNavigationFrequency(NAVIGATION_FREQUENCY_HZ));

    // Stream the nav messages instead of polling them. Polled getters block
    // on an I2C poll round-trip per message (measured bursts up to ~2 s in
    // updatePosition, stalling the position task and everything behind
    // mutexSem). With auto delivery the module pushes NAV-PVT (fixType,
    // carrSoln, h/vAcc, SIV, pDOP), NAV-HPPOSLLH (high-res lat/lon/height)
    // and NAV-HPPOSECEF (getPositionAccuracy) at the navigation rate, and
    // the getters become non-blocking reads of the cached packet.
    GNSS_CFG_STEP("setAutoPVT", myGNSS.setAutoPVT(true));
    GNSS_CFG_STEP("setAutoHPPOSLLH", myGNSS.setAutoHPPOSLLH(true));
    GNSS_CFG_STEP("setAutoNAVHPPOSECEF", myGNSS.setAutoNAVHPPOSECEF(true));
    byte rate = myGNSS.getNavigationFrequency(); // Get the update rate of this module
    DBG.print(F("Current update rate: "));
    DBG.println(rate);

    GNSS_CFG_STEP("setNMEAGPGGAcallbackPtr", myGNSS.setNMEAGPGGAcallbackPtr(&callbackGPGGA)); // fails only on RAM alloc
    // GGA every 10th nav epoch = 1/s at 10 Hz. This doubles as the
    // receiver-liveness signal (lastGgaHeard_ms), so keep it ~1 Hz.
    GNSS_CFG_STEP("setVal8(MSGOUT_GGA)", myGNSS.setVal8(UBLOX_CFG_MSGOUT_NMEA_ID_GGA_I2C, 10));
#undef GNSS_CFG_STEP

    return NULL;
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

    bool notDetectedEmitted = false;  // one event per setup, not per retry
    bool configRetryEmitted = false;
    const char *failedStep;
    while ((failedStep = configureGNSS()) != NULL)
    {
      if (strcmp(failedStep, "begin") == 0)
      {
        DBG.println(F("ZED-F9P not answering at its I2C address, check wiring. Retrying."));
        if (!notDetectedEmitted)
        {
          notDetectedEmitted = true;
          // Severity 3: without the ZED-F9P there is no positioning at all.
          telemetryEmitError(3, "i2c_gnss_not_detected", "ZED-F9P begin() failing, check wiring");
        }
      }
      else
      {
        // The module is there (begin() passed) but did not ACK one config
        // write. Retrying the whole configuration has always cleared it;
        // this is not a wiring fault and must not raise the fatal code
        // (it did until 2026-09-11: a false sev-3 alert on every boot).
        DBG.printf("ZED-F9P config step %s not acknowledged, retrying\n", failedStep);
        if (!configRetryEmitted)
        {
          configRetryEmitted = true;
          char msg[64];
          snprintf(msg, sizeof(msg), "config step %s not acked, retrying", failedStep);
          telemetryEmitError(1, "gnss_config_retry", msg);
        }
      }
      blinkOneTime(500, false);
    }

    return true;
}

/**
 * @brief One bounded-mutex checkUblox() pass, then the NMEA callbacks.
 *
 * Every myGNSS I2C access runs under mutexSem: the NTRIP task shares the
 * object and the bus with the position task, and unsynchronized access
 * desyncs the UBX parser (8.8 s getter stalls measured 2026-07-29). On
 * timeout the pass is skipped: the position task's own passes keep the
 * parser fed, and the caller must not stall behind a slow bus.
 * checkCallbacks() stays OUTSIDE the mutex: no I2C, and callbackGPGGA takes
 * the (non-recursive) mutex itself, so holding it here would self-deadlock.
 */
static void gnssCheckUbloxLocked(uint32_t timeout_ms)
{
  if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(timeout_ms)))
  {
    myGNSS.checkUblox();
    xSemaphoreGive(mutexSem);
  }
  myGNSS.checkCallbacks();
}

/**
 * @brief Receiver watchdog: one tick of the recovery ladder (bench 4.1,
 * 2026-08-24: module went fully mute for 14+ min, no self-recovery).
 *
 * Silent = no GGA for GNSS_SILENT_AFTER_MS. While silent, keep the parser
 * fed from here too (the position task polls as well, but this pass is cheap
 * while mute and guarantees the revived stream gets parsed for
 * lastGgaHeard_ms to recover even if that task is wedged on a degraded bus),
 * and every GNSS_RECOVERY_GAP_MS climb one rung: reconfigure -> GNSS software
 * reset -> hard reset (cold start). A real GGA resets the ladder.
 *
 * @return true if a recovery attempt ran (up to 2 s of reset wait included):
 *         deliberate maintenance the caller must not count as a stall.
 */
static bool gnssRecoveryTick()
{
  static uint8_t stage = 0;  // 0 = reconfigure, 1 = sw reset, 2+ = hard reset
  static uint32_t count = 0;
  static uint32_t lastAttempt_ms = 0;

  if (millis() - lastGgaHeard_ms <= GNSS_SILENT_AFTER_MS)
  {
    stage = 0;  // receiver talking: episode over (if any)
    return false;
  }

  gnssCheckUbloxLocked(GNSS_MUTEX_TIMEOUT_MS);

  if (millis() - lastAttempt_ms < GNSS_RECOVERY_GAP_MS) return false;
  lastAttempt_ms = millis();
  count++;
  uint32_t silentFor_s = (millis() - lastGgaHeard_ms) / 1000;
  const char *action = "skipped (mutex busy)";
  bool attempted = false;
  const char *failedStep = NULL;  // configureGNSS() result, valid if attempted
  if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GNSS_RECOVERY_MUTEX_MS)))
  {
    attempted = true;
    switch (stage)
    {
      case 0:
        action = "reconfigure";
        break;
      case 1:
        action = "sw reset";
        myGNSS.softwareResetGNSSOnly();
        vTaskDelay(2000/portTICK_PERIOD_MS);  // module restart time
        break;
      default:
        action = "hard reset";
        myGNSS.hardReset();  // cold start: last resort, loses ephemeris
        vTaskDelay(2000/portTICK_PERIOD_MS);
        break;
    }
    failedStep = configureGNSS();
    xSemaphoreGive(mutexSem);
    if (stage < 2) stage++;
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "receiver silent %u s, recovery #%u: %s%s%s",
           (unsigned)silentFor_s, (unsigned)count, action,
           !attempted ? "" : (failedStep == NULL ? " ok" : " failed at "),
           (attempted && failedStep != NULL) ? failedStep : "");
  telemetryEmitError(2, "gnss_degraded", msg);
  DBG.printf("gnss_degraded: %s\n", msg);
  return true;
}

static bool updatePosition(coord_t *out)
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
  // Only stream positions the walk may trust (MIN_ACCEPTABLE_ACCURACY_MM).
  // When accuracy degrades past it (or the receiver stops producing
  // solutions and there is nothing fresh to send) the position stream simply
  // goes quiet, ubloxUpdatedAt on the phone goes stale, and the app falls
  // back to internal GPS after its freshness window. Degraded RTK and lost
  // RTK use the same fallback. The caller notifies after releasing the
  // mutex: nothing here may block (a blocking queue send from under the
  // mutex wedged the whole GNSS pipeline for 43 s, 2026-07-29).
  const bool trusted = llhFresh && accuracy > 0 && accuracy <= MIN_ACCEPTABLE_ACCURACY_MM;
  if (trusted) *out = coord;

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
      char msg[48];
      snprintf(msg, sizeof(msg), "updatePosition held mutex %u ms", (unsigned)holdMs);
      telemetryEmitError(1, "gnss_pipe_stall", msg);
    }
  }
  return trusted;
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

  coord_t coord;
  // "<lat> <latHp> <lon> <lonHp>", decimal, space-separated (the 713D0004 wire
  // format). Worst case "-1234567890 -99 -1234567890 -99" = 27 chars + NUL.
  // Stack buffer, not String: 10 Hz on a heap with a measured 1.8 kB minimum.
  char latLonStr[32];

  while (true)
  {
    telemetryNotePositionLoop();  // heartbeat liveness counter (key 19)

    bool havePosition = false;
    if (xSemaphoreTake(mutexSem, portMAX_DELAY))
    {
      havePosition = updatePosition(&coord);
      xSemaphoreGive(mutexSem);
    }

    // Notify from here, mutex released: notify() only posts to the BT task,
    // so core 0 is fine, and the producer/consumer hand-off through a queue
    // and a second 100 ms task (up to one period of added latency) is gone.
    if (havePosition && bleLinkConnected())
    {
      int n = snprintf(latLonStr, sizeof(latLonStr),
                       "%ld" DATA_STR_DELIMITER "%d" DATA_STR_DELIMITER
                       "%ld" DATA_STR_DELIMITER "%d",
                       (long)coord.lat, (int)coord.latHp,
                       (long)coord.lon, (int)coord.lonHp);
      pRealtimeKinematicsCharacteristic->setValue((uint8_t *)latLonStr, (size_t)n);
      pRealtimeKinematicsCharacteristic->notify();
      DBG.printf("pos: %s\n", latLonStr);
    }

    vTaskDelay(TASK_RTK_GET_POSITION_INTERVAL_MS/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

/**
 * @brief Build the caster request: GET line, user agent, Basic auth (or a
 * plain Accept/Connection block when there is no user). Returns false when
 * the request does not fit: a truncated one would carry broken headers and
 * fail at the caster anyway, and the old strncat bounded by the full
 * destination size could smash this task's stack.
 */
static bool ntripBuildRequest(char *out, size_t cap)
{
  int len = snprintf(out, cap, "GET /%s HTTP/1.0\r\nUser-Agent: NTRIP SparkFun u-blox Client v1.0\r\n",
                     kMountPoint);
  if (len < 0 || (size_t)len >= cap) return false;

  int appended;
  if (kCasterUser[0] == '\0')
  {
    appended = snprintf(out + len, cap - len, "Accept: */*\r\nConnection: close\r\n\r\n");
  }
  else
  {
    char userCredentials[sizeof(kCasterUser) + sizeof(kCasterPass)];  // "user:pass" + NUL
    snprintf(userCredentials, sizeof(userCredentials), "%s:%s", kCasterUser, kCasterPass);
    base64 b;
    String encoded = b.encode(userCredentials);
    appended = snprintf(out + len, cap - len, "Authorization: Basic %s\r\n\r\n", encoded.c_str());
  }
  return appended >= 0 && (size_t)appended < cap - len;
}

void task_rtk_get_corrrection_data(void *pvParameters)
{
  (void)pvParameters;

  // Caster credentials: compile-time constants from CasterSecrets.h, empty
  // on a placeholder build (no fleet-secrets entry): nothing to connect to,
  // park the task; heading, position and telemetry keep running.
  const uint16_t casterPort = (uint16_t)strtoul(kCasterPort, NULL, 10);
  if (kCasterHost[0] == '\0' || casterPort == 0 || kCasterUser[0] == '\0' || kMountPoint[0] == '\0')
  {
    DBG.println(F("RTK credentials incomplete! Suspending RTK task."));
    vTaskSuspend(NULL);
  }

  WiFiClient ntripClient;

  // No-RTCM hangup window. Starts at the post-connect grace (the VRS needs
  // our GGA before it streams; 2026-08-24: a fixed 10 s window expired inside
  // the post-connect mutex wait and killed every session in the iteration
  // that opened it), tightens to NTRIP_RTCM_TIMEOUT_MS once data flows.
  uint32_t lastReceivedRTCM_ms = 0;
  uint32_t rtcmTimeout_ms = NTRIP_CONNECT_GRACE_MS;
  bool gotDataThisSession = false;

  // Reconnect backoff (caster etiquette, refnet throttles reconnect floods):
  // attemptDelay_ms is waited before the next connect attempt; armBackoff()
  // arms it from reconnectDelay_ms at every failed or dataless attempt, which
  // then doubles up to the cap. Received RTCM resets both.
  uint32_t reconnectDelay_ms = NTRIP_BACKOFF_START_MS;
  uint32_t attemptDelay_ms = 0;
  auto armBackoff = [&]()
  {
    attemptDelay_ms = reconnectDelay_ms;
    reconnectDelay_ms = min(reconnectDelay_ms * 2, (uint32_t)NTRIP_BACKOFF_MAX_MS);
  };

  // GGA is required for Rev2 NTRIP casters (the VRS computes its virtual
  // station from it); push one every 10 s.
  const uint32_t timeBetweenGGAUpdate_ms = 10000;
  uint32_t lastTransmittedGGA_ms = 0;

  // ntrip_status bookkeeping (PROJECT-PLAN.md par. 4.3): events on state
  // transitions only, never per retry iteration - outages must not flood
  // the ring. successfulConnects - 1 = "reconnects" in the event.
  uint32_t successfulConnects = 0;
  bool wasConnected = false;
  bool reconnectingEmitted = false;  // one reconnecting event per outage
  bool outageErrorEmitted = false;   // one error event per outage, not per retry
  auto emitOutageError = [&](uint8_t severity, const char *code, const char *msg)
  {
    if (outageErrorEmitted) return;
    outageErrorEmitted = true;
    telemetryEmitError(severity, code, msg);
  };

  lastGgaHeard_ms = millis();  // arm the receiver-liveness clock at task start

  while (true)
  {
    telemetryNoteNtripLoop();  // heartbeat liveness counter (key 18)

    // gnss_pipe_stall: an iteration over GNSS_PIPE_STALL_MS is reported at
    // the bottom of the loop. Deliberate waits (recovery, WiFi outage,
    // backoff) restart the clock so they don't count as a stall.
    uint32_t iterStart_ms = millis();

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

    if (gnssRecoveryTick()) iterStart_ms = millis();

    if (!ntripClient.connected())
    {
      if (wifiEnsureAssociated())
      {
        // A WiFi outage is reported by wifi_disconnected, not
        // gnss_pipe_stall: restart the iteration clock so outage time
        // doesn't count as a stall. And while blocked, checkCallbacks never
        // ran, so the liveness clock is stale regardless of the receiver's
        // health. Re-arm it: the receiver gets GNSS_SILENT_AFTER_MS to
        // prove itself before gate/ladder act.
        iterStart_ms = millis();
        lastGgaHeard_ms = millis();
      }

      // WiFi associated, caster disconnected: keep the hotspot's upstream
      // path awake - nothing else is generating traffic in this state, and
      // the gates below can hold us here for minutes. (Self rate-limited.)
      warmHotspotPath(kCasterHost);

      // Receiver-liveness gate: a mute F9P produces no GGA, and the VRS
      // streams nothing without one. Connecting would only cycle dataless
      // sessions against the caster (~93 s cycle observed, bench 4.1). The
      // recovery ladder owns this state; skip caster attempts until the
      // receiver talks again.
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
        // (up to NTRIP_BACKOFF_MAX_MS in one go) - these gaps are exactly
        // where the hotspot dozes off.
        uint32_t waited_ms = 0;
        while (waited_ms < attemptDelay_ms)
        {
          uint32_t slice_ms = min(attemptDelay_ms - waited_ms, (uint32_t)1000);
          vTaskDelay(slice_ms/portTICK_PERIOD_MS);
          waited_ms += slice_ms;
          warmHotspotPath(kCasterHost);
        }
        attemptDelay_ms = 0;
        iterStart_ms = millis();  // deliberate pacing, not a pipeline stall
      }

      // --- Open a session: TCP connect, request, response -----------------
      DBG.printf("Opening socket to %s:%u\n", kCasterHost, casterPort);
      if (!ntripClient.connect(kCasterHost, casterPort))
      {
        DBG.println(F("Connection to caster failed"));
        emitOutageError(1, "ntrip_connect_failed", "TCP connect to caster failed");
        armBackoff();
        continue;
      }

      char serverRequest[512];
      if (!ntripBuildRequest(serverRequest, sizeof(serverRequest)))
      {
        DBG.println(F("NTRIP server request exceeds buffer, not sent. Check mount point / credential lengths."));
        telemetryEmitError(2, "ntrip_request_overflow",
                           "caster request exceeds buffer; check mount point / credential lengths");
        ntripClient.stop();
        telemetrySetNtripConnected(false);
        armBackoff();
        continue;  // config is wrong, but never overflow
      }
      // Request line only: the header block carries the Basic-auth
      // credentials, which must not land in serial captures.
      DBG.printf("Requesting mount point %s: ", kMountPoint);
      DBG.write((const uint8_t *)serverRequest, strcspn(serverRequest, "\r\n"));
      DBG.println();
      ntripClient.write(serverRequest, strlen(serverRequest));

      // Wait for the response, bounded: too many requests with wrong
      // settings lead to a ban, so stop instead of re-sending.
      uint32_t waitStart_ms = millis();
      bool casterTimedOut = false;
      while (ntripClient.available() == 0)
      {
        if (millis() - waitStart_ms > CONNECTION_TIMEOUT_MS)
        {
          ntripClient.stop();
          DBG.println(F("Caster timed out!"));
          casterTimedOut = true;
          break;
        }
        vTaskDelay(1000/portTICK_PERIOD_MS);
      }
      if (casterTimedOut)
      {
        telemetrySetNtripConnected(false);  // stop() happened in the wait loop
        emitOutageError(1, "ntrip_connect_failed", "caster response timeout");
        armBackoff();
        continue;
      }

      char response[512];
      size_t responseLen = 0;
      while (ntripClient.available() && responseLen < sizeof(response) - 1)
      {
        response[responseLen++] = ntripClient.read();
      }
      response[responseLen] = '\0';
      DBG.print(F("Caster responded with: "));
      DBG.println(response);

      // 'ICY 200 OK' / 'HTTP/1.1 200 OK' opens the stream. A source table
      // means the mount point is unknown to the caster; 401 means bad
      // credentials or a ban.
      if (strstr(response, "200") == NULL)
      {
        DBG.printf("Failed to connect to %s\n", kCasterHost);
        // Caster spoke but refused (401, wrong mount point, ban):
        // config-class problem, so severity 2 - a Grafana alert, not
        // noise. The caster response goes in msg (no secrets in it).
        emitOutageError(2, "ntrip_bad_response", response);
        armBackoff();
        continue;
      }

      DBG.printf("Connected to %s\n", kCasterHost);
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

      // One parser pass so a GGA is ready for that push; bounded, because
      // this task must reach its read/GGA sections while the grace window
      // is still open.
      gnssCheckUbloxLocked(GNSS_MUTEX_TIMEOUT_MS);
    }

    // --- Stream: drain the socket into the receiver ------------------------
    if (ntripClient.connected())
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
        size_t rtcmCount = 0;
        while (ntripClient.available() && rtcmCount < sizeof(rtcmData))
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

        // Push RTCM to the receiver over I2C. Bounded take: when the
        // position task is in a slow-I2C stretch, dropping one redundant
        // correction slice beats stalling the link (the 12-26 s portMAX_DELAY
        // waits here are what killed every session on 2026-08-24).
        if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GNSS_MUTEX_TIMEOUT_MS)))
        {
          myGNSS.pushRawData(rtcmData, rtcmCount, false);
          telemetryNoteRtcmPushed(rtcmCount);  // feeds corr_age_ms + bytes_rx
          xSemaphoreGive(mutexSem);
          DBG.printf("RTCM pushed to ZED: %u\n", (unsigned)rtcmCount);
        }
        else
        {
          DBG.printf("RTCM slice dropped (mutex busy): %u\n", (unsigned)rtcmCount);
        }
      }
    }

    // --- GGA push: our position to the caster every 10 s -------------------
    if (ntripClient.connected() && millis() - lastTransmittedGGA_ms > timeBetweenGGAUpdate_ms)
    {
      char localGgaSentence[NMEA_GGA_MAX_LENGTH] = {0};
      bool shouldSendGga = false;

      // Bounded take; on timeout the gate is left expired, so the next
      // iteration (~1 s) retries instead of waiting the full 10 s period.
      if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GGA_MUTEX_TIMEOUT_MS)))
      {
        if (ggaSentenceComplete)
        {
          strncpy(localGgaSentence, ggaSentence, NMEA_GGA_MAX_LENGTH - 1);
          localGgaSentence[NMEA_GGA_MAX_LENGTH - 1] = '\0';
          shouldSendGga = true;
          ggaSentenceComplete = false;  // start over
          lastTransmittedGGA_ms = millis();
        }
        xSemaphoreGive(mutexSem);
      }

      if (shouldSendGga)
      {
        DBG.print(F("Pushing GGA to server: "));
        DBG.println(localGgaSentence);
        ntripClient.print(localGgaSentence);
        ntripClient.print("\r\n");
      }
    }

    // --- Hangup: the no-RTCM window expired (30 s grace after a connect,
    // 10 s once data has flowed) ---------------------------------------------
    if (ntripClient.connected() && millis() - lastReceivedRTCM_ms > rtcmTimeout_ms)
    {
      DBG.println(F("RTCM timeout. Disconnecting..."));
      // Socket up but no corrections. This is the signature of a
      // correction-delivery problem (vs. GNSS degradation, PROJECT-PLAN par. 2)
      char msg[64];
      snprintf(msg, sizeof(msg), "no RTCM for %u s, dropping caster connection",
               (unsigned)(rtcmTimeout_ms / 1000));
      telemetryEmitError(1, "ntrip_rtcm_timeout", msg);
      ntripClient.stop();
      telemetrySetNtripConnected(false);
      // A session that never delivered a byte counts as a failed attempt:
      // back off before hammering the caster again.
      if (!gotDataThisSession) armBackoff();
    }

    // gnss_pipe_stall: a whole iteration over threshold is reported. The
    // 2026-08-21/24 causes (20 Hz nav rate, polled getters, unbounded mutex
    // takes) are fixed; the event stays as a "something is slow" alarm.
    // Iterations that `continue` above skip this on purpose: their delays
    // are retry pacing.
    uint32_t iterMs = millis() - iterStart_ms;
    if (iterMs > GNSS_PIPE_STALL_MS)
    {
      static uint32_t lastStallEmit_ms = 0;
      if (millis() - lastStallEmit_ms >= GNSS_PIPE_STALL_GAP_MS)
      {
        lastStallEmit_ms = millis();
        char msg[32];
        snprintf(msg, sizeof(msg), "ntrip iter %u ms", (unsigned)iterMs);
        telemetryEmitError(1, "gnss_pipe_stall", msg);
      }
    }

    vTaskDelay(TASK_WIFI_RTK_DATA_INTERVAL_MS/portTICK_PERIOD_MS);
  }

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
  BLEServer *pServer = bleLinkBegin(deviceName.c_str());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  // Notify-only characteristics (fastest: no response)
  pHeadtrackerCharacteristic = pService->createCharacteristic(
    HEADTRACKER_BIN_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pHeadtrackerCharacteristic->addDescriptor(new BLE2902());

  pRealtimeKinematicsCharacteristic = pService->createCharacteristic(
    REALTIME_KINEMATICS_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pRealtimeKinematicsCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  // Telemetry GATT service (PROJECT-PLAN.md par. 5.1); not advertised — the
  // 31 B adv payload has no room for a second 128-bit UUID.
  telemetryBleSetup(pServer);

  bleLinkStartAdvertising(SERVICE_UUID);
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

  while (!bleLinkConnected())
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
  // Frames actually put on the wire per second.
  // ticks should stay at the sensor rate while notifies track the connection interval.
  uint32_t bnoNotifies = 0;
#endif

  heading_frame_t headingFrame = {};
  // The cached frame is refreshed every tick; frameFresh says whether it holds
  // a sample not yet transmitted.
  bool frameFresh = false;
  uint32_t lastHeadingNotify_ms = 0;

  while (true)
  {
    if (!bleLinkConnected())
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
        DBG.printf("bno stats: ticks %u, reports %u, notifies %u (period %u ms, conn %u units), "
                   "misses %u, max drain %u, last drain %u us\n",
                   bnoTicks, bnoReports, bnoNotifies, headingNotifyPeriodMs(),
                   bleLinkConnIntervalUnits(), bnoMisses, bnoMaxDrain, bnoLastRead_us);
        bnoTicks = bnoReports = bnoMisses = bnoMaxDrain = bnoNotifies = 0;
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
      // Refresh the cached frame every tick, whether or not it gets sent: the
      // slot always holds the newest sample, so whatever the pacing below
      // decides to transmit is as fresh as the sensor allows. t_dev_ms is
      // stamped here, at sample time, not at notify time. the apps derive
      // rotation speed from its deltas.
      if (drained > 0)
      {
        imuSampleCount++;
        // Raw quaternion on the wire; the apps do the (identical, spec'd)
        // quat->azimuth/elevation math on their hardware FPUs.
        headingFrame.t_dev_ms = millis();
        headingFrame.qi = headingScaledInt16(bno080.getQuatI(), 16384.0f);
        headingFrame.qj = headingScaledInt16(bno080.getQuatJ(), 16384.0f);
        headingFrame.qk = headingScaledInt16(bno080.getQuatK(), 16384.0f);
        headingFrame.qw = headingScaledInt16(bno080.getQuatReal(), 16384.0f);
        headingFrame.linAccelZ_cms2 = headingScaledInt16(bno080.getLinAccelZ(), 100.0f);
        frameFresh = true;
      }

      // Transmit at most one frame per connection event. A congested TX queue
      // means the previous frames have not gone out yet, so adding another
      // only deepens the backlog the app will discard: skip and let the next
      // tick send a fresher one (ble_link drops a stale congestion flag by
      // itself, so a missed "cleared" event cannot freeze head tracking).
      const uint32_t nowNotify_ms = millis();
      if (frameFresh && !bleLinkTxCongested() &&
          (nowNotify_ms - lastHeadingNotify_ms) >= headingNotifyPeriodMs())
      {
        lastHeadingNotify_ms = nowNotify_ms;
        frameFresh = false;
        // seq counts frames put on the wire, so the apps' drop detection keeps
        // meaning "this many notifications went missing" and not "this many
        // samples were coalesced".
        headingFrame.seq++;
        pHeadtrackerCharacteristic->setValue((uint8_t *)&headingFrame, sizeof(headingFrame));
        pHeadtrackerCharacteristic->notify();
#if DEBUGGING
        bnoNotifies++;
#endif
      }
      }
      vTaskDelay(TASK_BNO_ORIENTATION_VIA_BLE_INTERVAL_MS/portTICK_PERIOD_MS);
    }
  // Delete self task
  vTaskDelete(NULL);

} /*** end task_bno_orientation_via_ble ***/
