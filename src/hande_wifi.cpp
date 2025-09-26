#include <handle_wifi.h>

bool setupWiFi()
{
  while (!checkNetworkAvailable(kWifiSsid) )
  {
    DBG.print(F("Waiting for HotSpot "));
    DBG.print(kWifiSsid);
    DBG.println(F(" to appear..."));
    vTaskDelay(1000/portTICK_RATE_MS);
  }
  return setupStationMode(kWifiSsid, kWifiPw);
}

bool setupStationMode(const char* ssid, const char* password)
{
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
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

bool checkNetworkAvailable(const String& ssid)
{
  u_int8_t n = WiFi.scanNetworks();
  if (n == 0)
  {
    /* DBG.println(F("no networks found")); */
    return false;
  }
  else
  {
    /* DBG.print(n);
    DBG.println(F(" networks found")); */
    for (u_int8_t i = 0; i < n; ++i)
    {
      if (WiFi.SSID(i) == ssid)
      {
        /* DBG.println(F("Target network found:"));
        DBG.print(WiFi.SSID(i));
        DBG.print(F(" ("));
        DBG.print(WiFi.RSSI(i));
        DBG.println(F("dB)")); */
        return true;
      }
    }
  }
  return false;
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
