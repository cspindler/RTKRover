
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
#include <telemetry/telemetry.h>
#include <telemetry/telemetry_ble.h>
#include <ble_link.h>
#include <corrections.h>
#ifdef TESTING
#include <TestsRTKRover.h>  // AUnit suites; debug builds only
#endif

/*
=================================================================================
                                Bluetooth LE
=================================================================================
*/
// Last 24 bits of the eFuse MAC: the chip-id half of the fallback BLE name.
static uint32_t getChipId()
{
  uint32_t chipId = 0;
  for (int i = 0; i < 17; i = i + 8)
  {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }
  return chipId;
}

// Fleet-configured BLE name (fleet-secrets.ini via CasterSecrets.h), empty on
// placeholder builds -> fall back to "<DEVICE_TYPE>-<chip-id>".
static String getBleName()
{
  if (kBleName[0] != '\0')
    return String(kBleName);
  return String(DEVICE_TYPE) + "-" + String(getChipId(), HEX);
}

// Task handles, for the debug-build stack watermark report in loop().
static TaskHandle_t hTaskCorrections = NULL;
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
discarded frames still costs airtime and still holds a Bluedroid TX buffer,
which is heap.

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
 * @brief Task for the receiver's correction side: pushes the RTCM chunks the
 *        phone wrote (corrections.cpp FIFO) into the receiver, dispatches the
 *        NMEA callbacks (which notify the GGA to the phone) and runs the
 *        receiver watchdog (recovery ladder).
 *
 * @param pvParameters Void pointer, no parameter used here
 */
void task_gnss_corrections(void *pvParameters);

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
  // Run the AUnit tests here rather than relying on loop(): setupGNSS()
  // below retries forever without a receiver, so loop() (the usual AUnit
  // driver) may never run. All tests are synchronous; a bounded number of
  // passes resolves them all and prints the summary to serial.
  DBG.println(F("Running unit tests..."));
  for (int i = 0; i < 100; i++)
  {
    aunit::TestRunner::run();
    delay(2);
  }
  #endif

  // blink sequence before starting BLE
  blinkOneTime(125, true);
  blinkOneTime(125, true);
  blinkOneTime(2000, true);

  DBG.print(F("BLE Device name: "));
  DBG.println(getBleName());

  // BLE first: it is the only radio, and the phone connects within a second.
  setupBLE();

  // Telemetry drain: lowest priority in the system (PROJECT-PLAN.md par. 5).
  // Started BEFORE the blocking sensor setups on purpose: setupGNSS()/
  // setupBNO080() retry forever when a sensor doesn't answer, and with the
  // drain not yet running the assembly would sit BLE-connected but mute.
  // The i2c_* error events waiting in the ring, never delivered (observed
  // 2026-08-24, rwa-hs-4: F9P not ACKing, app received no telemetry at all).
  // The task needs only the ring, BLE and the battery read.
  telemetryBleStartTask();

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
  its queue merged into the position task. 0.48.0: 14 KB, the NTRIP task and
  its socket buffers gone (ADR-001).
  */
  int stack_size_task_gnss_corrections = 1024 * 6;               // NTRIP task: min free 2792 of 9 KB with 3 KB of socket buffers; re-measured 0.48.0
  int stack_size_task_rtk_get_rover_position = 1024 * 4;         // min free 2344 before the merge; notify() added
  int stack_size_task_bno_orientation_via_ble = 1024 * 4;        // min free 2080

  xTaskCreatePinnedToCore( &task_gnss_corrections, "task_gnss_corrections", stack_size_task_gnss_corrections, NULL, TASK_GNSS_CORRECTIONS_PRIORITY, &hTaskCorrections, RUNNING_CORE_0);
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
               hTaskCorrections ? uxTaskGetStackHighWaterMark(hTaskCorrections) : 0,
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

// Receiver-liveness timestamp: millis() of the last GGA sentence the module
// produced (callbackGPGGA fires with or without a fix, ~1/s at the configured
// MSGOUT rate). Written from callbackGPGGA and armed at corrections-task
// start. GNSS_SILENT_AFTER_MS without one means the receiver is mute
// (bench 4.1, 2026-08-24) and drives the recovery ladder.
static volatile uint32_t lastGgaHeard_ms = 0;

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

// Called from myGNSS.checkCallbacks() on the corrections task for every
// complete GGA sentence (NMEA_GGA_data_t: see u-blox_structs.h).
void callbackGPGGA(NMEA_GGA_data_t *nmeaData)
{
  // Liveness first, unconditionally: a GGA arriving proves the receiver is
  // producing output, fix or not.
  lastGgaHeard_ms = millis();

  // GGA uplink (713D0007, PROJECT-PLAN.md par. 5.6): only sentences with a
  // fix go to the phone, which forwards the latest one to the caster. A
  // fixless GGA is unusable to the VRS (it computes its virtual station from
  // it), and the app seeds the caster from CoreLocation until the first one
  // arrives (ADR-001 par. 2). No mutex here: notify() only posts to the BT
  // task.
  if (ggaFixQuality(nmeaData->nmea, nmeaData->length) == 0) return;
  if (correctionsNotifyGga(nmeaData->nmea, nmeaData->length))
  {
    size_t n = nmeaData->length;
    while (n > 0 && (nmeaData->nmea[n - 1] == '\r' || nmeaData->nmea[n - 1] == '\n')) n--;
    DBG.print(F("GGA to phone: "));
    DBG.write(nmeaData->nmea, n);
    DBG.println();
  }
}

/**
 * @brief One begin() attempt + the full rover configuration. Shared by boot
 * (setupGNSS, which retries around it) and the runtime recovery ladder in
 * the corrections task (which calls it holding mutexSem after a reset).
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
 * Every myGNSS I2C access runs under mutexSem: the corrections task shares
 * the object and the bus with the position task, and unsynchronized access
 * desyncs the UBX parser (8.8 s getter stalls measured 2026-07-29). On
 * timeout the pass is skipped: the position task's own passes keep the
 * parser fed, and the caller must not stall behind a slow bus.
 * checkCallbacks() stays OUTSIDE the mutex: no I2C in the callbacks, and
 * nothing they do may wait on the position task.
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

void task_gnss_corrections(void *pvParameters)
{
  (void)pvParameters;

  lastGgaHeard_ms = millis();  // arm the receiver-liveness clock at task start

  while (true)
  {
    telemetryNoteCorrectionsLoop();  // heartbeat liveness counter (key 20)

    // gnss_pipe_stall: an iteration over GNSS_PIPE_STALL_MS is reported at
    // the bottom of the loop. A recovery attempt restarts the clock so its
    // deliberate reset wait doesn't count as a stall.
    uint32_t iterStart_ms = millis();

    // Dispatch pending NMEA callbacks so callbackGPGGA runs every iteration:
    // it is the liveness signal (lastGgaHeard_ms) the recovery ladder depends
    // on, and the GGA uplink. Must stay OUTSIDE mutexSem (gnssCheckUbloxLocked
    // says why).
    myGNSS.checkCallbacks();

    if (gnssRecoveryTick()) iterStart_ms = millis();

    // --- RTCM downlink: FIFO -> receiver ------------------------------------
    // One bounded mutex take per iteration, then every queued chunk goes to
    // the receiver with the raw push (RTCM3 is self-delimiting: no framing,
    // no reassembly). On a timeout nothing is popped: the chunks wait, and
    // the FIFO's drop-oldest at CORRECTIONS_RTCM_FIFO_SIZE (~3 VRS epochs)
    // means a slow-I2C stretch on the position task costs the oldest
    // corrections, never the newest.
    if (correctionsRtcmQueued())
    {
      if (xSemaphoreTake(mutexSem, pdMS_TO_TICKS(GNSS_MUTEX_TIMEOUT_MS)))
      {
        uint8_t rtcm[CORRECTIONS_RTCM_CHUNK_MAX];
        size_t n;
        while ((n = correctionsPopRtcm(rtcm, sizeof(rtcm))) > 0)
        {
          myGNSS.pushRawData(rtcm, n, false);
          telemetryNoteRtcmPushed(n);  // feeds corr_age_ms + heartbeat rtcm_bytes
          DBG.printf("RTCM pushed to ZED: %u\n", (unsigned)n);
        }
        xSemaphoreGive(mutexSem);
      }
      else
      {
        DBG.println(F("RTCM push deferred (mutex busy)"));
      }
    }

    // Chunks the FIFO evicted unpushed: the receiver side is not draining
    // (wedged bus, mutex held for seconds). A correction-delivery problem on
    // the assembly, so its own alert dimension; rate-limited like the stall.
    static uint32_t rtcmDroppedReported = 0;
    const uint32_t rtcmDropped = correctionsRtcmDropped();
    if (rtcmDropped != rtcmDroppedReported)
    {
      static uint32_t lastDropEmit_ms = 0;
      if (millis() - lastDropEmit_ms >= GNSS_PIPE_STALL_GAP_MS)
      {
        lastDropEmit_ms = millis();
        char msg[64];
        snprintf(msg, sizeof(msg), "%u RTCM chunks evicted unpushed",
                 (unsigned)(rtcmDropped - rtcmDroppedReported));
        telemetryEmitError(1, "rtcm_fifo_overflow", msg);
        rtcmDroppedReported = rtcmDropped;
      }
    }

    // gnss_pipe_stall: a whole iteration over threshold is reported. The
    // 2026-08-21/24 causes (20 Hz nav rate, polled getters, unbounded mutex
    // takes) are fixed; the event stays as a "something is slow" alarm.
    uint32_t iterMs = millis() - iterStart_ms;
    if (iterMs > GNSS_PIPE_STALL_MS)
    {
      static uint32_t lastStallEmit_ms = 0;
      if (millis() - lastStallEmit_ms >= GNSS_PIPE_STALL_GAP_MS)
      {
        lastStallEmit_ms = millis();
        char msg[32];
        snprintf(msg, sizeof(msg), "corrections iter %u ms", (unsigned)iterMs);
        telemetryEmitError(1, "gnss_pipe_stall", msg);
      }
    }

    vTaskDelay(TASK_GNSS_CORRECTIONS_INTERVAL_MS/portTICK_PERIOD_MS);
  }

  vTaskDelete(NULL);
} /*** end task_gnss_corrections ***/

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

  // The correction loop (RTCM down, GGA up), ADR-001.
  correctionsBleSetup(pService);

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
