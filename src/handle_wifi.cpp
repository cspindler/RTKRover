#include <handle_wifi.h>

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
