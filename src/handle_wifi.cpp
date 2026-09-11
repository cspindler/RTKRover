#include <handle_wifi.h>
#include <led.h>
#include <telemetry/telemetry.h>

bool setupWiFi()
{
  // Start the driver and cap TX power before any radio activity: TX at default
  // power browns out weak batteries. No pre-scan for the hotspot.
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);

  return setupStationMode(kWifiSsid, kWifiPw);
}

bool setupStationMode(const char* ssid, const char* password)
{
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  // disconnect(true) stops the driver, which resets TX power to the
  // 19.5 dBm default. Re-apply the cap before begin() associates.
  WiFi.setTxPower(WIFI_TX_POWER);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  DBG.print(F("Connecting to "));
  DBG.print(ssid);

  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000)
  {
    vTaskDelay(200/portTICK_RATE_MS);
    DBG.print(F("."));
  }
  DBG.println();

  if (WiFi.status() != WL_CONNECTED)
  {
    DBG.println(F("WiFi connection failed"));
    return false;
  }
  else
  {
    DBG.println(F("WiFi connected"));
    /* DBG.print(F("WiFi connected to SSID: "));
    DBG.println(WiFi.SSID());
    DBG.print(F("WiFi client name: "));
    DBG.println(WiFi.getHostname());
    DBG.print(F("IP Address: "));
    DBG.println(WiFi.localIP()); */
    return true;
  }
}

bool wifiEnsureAssociated()
{
  if (WiFi.isConnected()) return false;

  // The previous full setupStationMode() per retry cycled a complete driver
  // deinit/init every ~12 s, which leaked ~48 B/cycle (-14.5 kB/h, measured
  // bench 2+4 2026-08-24: OOM after ~1 h of continuous hotspot loss) and
  // transiently dipped free heap by several kB per cycle. It remains only as
  // a rare escape hatch for a wedged driver.
  uint32_t wifiDown_ms = millis();
  uint32_t lastNudge_ms = millis();
  uint32_t nudgeDelay_ms = WIFI_RECONNECT_NUDGE_MS;
  // Status at the previous iteration, and when the driver first latched
  // WL_CONNECT_FAILED (0 = not latched). Both drive the ladder below.
  wl_status_t lastStatus = WiFi.status();
  uint32_t failedSince_ms = 0;
  bool lossEmitted = false;  // one error event per outage, not per retry
  while (!WiFi.isConnected())
  {
    const wl_status_t status = WiFi.status();
    DBG.printf("WiFi: not associated, state %d\n", status);
    // Report only once the outage has outlived the grace.
    if (!lossEmitted && millis() - wifiDown_ms >= WIFI_LOSS_REPORT_AFTER_MS)
    {
      lossEmitted = true;
      telemetryEmitError(1, "wifi_disconnected", "hotspot lost, reconnecting");
    }

    bool nudgeNow = false;
    if (status != lastStatus)
    {
      lastStatus = status;
      nudgeDelay_ms = WIFI_RECONNECT_NUDGE_MS;
      nudgeNow = true;
    }

    if (status == WL_CONNECT_FAILED)
    {
      if (failedSince_ms == 0) failedSince_ms = millis();
    }
    else
    {
      failedSince_ms = 0;
    }
    const bool failedTooLong = failedSince_ms != 0 &&
                               millis() - failedSince_ms >= WIFI_REINIT_AFTER_FAILED_MS;

    if (failedTooLong || millis() - wifiDown_ms >= WIFI_REINIT_AFTER_MS)
    {
      wifiDown_ms = millis();
      lastNudge_ms = millis();
      nudgeDelay_ms = WIFI_RECONNECT_NUDGE_MS;  // fresh driver, fresh ladder
      failedSince_ms = 0;
      DBG.printf("WiFi full driver re-init (%s)\n",
                 failedTooLong ? "connect failed, soft path is a dead end"
                               : "down for minutes");
      setupStationMode(kWifiSsid, kWifiPw);
      lastStatus = WiFi.status();
    }
    else if (nudgeNow || millis() - lastNudge_ms >= nudgeDelay_ms)
    {
      lastNudge_ms = millis();
      WiFi.reconnect();
      if (nudgeDelay_ms < WIFI_RECONNECT_NUDGE_MAX_MS)
      {
        nudgeDelay_ms = min(nudgeDelay_ms * 2, (uint32_t)WIFI_RECONNECT_NUDGE_MAX_MS);
      }
      // Same heap measure as the heartbeat (8-bit capable internal heap);
      // ESP.getFreeHeap() also counts 32-bit-only IRAM and reads ~2x higher.
      DBG.printf("WiFi soft reconnect nudge, free heap %u, next in %u ms\n",
                 esp_get_free_heap_size(), nudgeDelay_ms);
    }
    blinkOneTime(1000, false);
    blinkOneTime(100, false);
  }
  return true;
}

String getDeviceName(const String& prefix)
{
  return prefix + "-" + String(getChipId(), HEX);
}

uint32_t getChipId()
{
  uint32_t chipId = 0;
  for(int i=0; i<17; i=i+8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }
  return chipId;
}
