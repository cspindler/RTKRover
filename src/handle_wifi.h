#ifndef HANDLE_WIFI_H
#define HANDLE_WIFI_H

#include <Arduino.h>
#include <WiFi.h>
#include <RTKRoverConfig.h>
#include <CasterSecrets.h>

/**
 * @brief Setup WiFi connection to the target network (kWifiSsid)
 *
 * Starts the driver in station mode with the TX power cap applied, then
 * attempts one connection via setupStationMode(). No scan: begin() probes
 * only the target SSID. If the hotspot is not up yet the attempt times out;
 * the caller is expected to retry.
 *
 * @return true If WiFi connection was successfully established
 * @return false If the connection attempt timed out or failed
 */
bool setupWiFi();

/**
 * @brief Configure and connect WiFi in station mode
 *
 * This function disconnects from any existing WiFi connections, sets the device
 * to station mode, enables auto-reconnect, and attempts to connect to the specified
 * network with a 10-second timeout.
 *
 * @param ssid The WiFi network SSID to connect to
 * @param password The WiFi network password
 * @return true If connection was successful
 * @return false If connection failed within the timeout period
 */
bool setupStationMode(const char* ssid, const char* password);

/**
 * @brief Block until WiFi is associated, running the reconnect ladder.
 *
 * Returns immediately when already associated. Otherwise waits SOFTLY:
 * auto-reconnect is on, so the driver keeps retrying by itself; every
 * WIFI_RECONNECT_NUDGE_MS (doubling to WIFI_RECONNECT_NUDGE_MAX_MS, reset on
 * any status change) it kicks it with WiFi.reconnect(), and only after
 * WIFI_REINIT_AFTER_MS without association (or WIFI_REINIT_AFTER_FAILED_MS in
 * WL_CONNECT_FAILED) does it fall back to a full setupStationMode(). Emits
 * one `wifi_disconnected` error per outage once it outlives
 * WIFI_LOSS_REPORT_AFTER_MS. Blinks the 1.0 s / 0.1 s code while waiting.
 *
 * @return true if it had to wait (the caller's clocks are stale)
 */
bool wifiEnsureAssociated();

/**
 * @brief Generate a unique device name using a prefix and chip ID
 *
 * This function creates a unique device name by combining a given prefix
 * with the ESP32's unique chip ID in hexadecimal format. This is useful
 * for creating unique BLE device names or hostnames.
 *
 * @param prefix The prefix string to prepend to the chip ID (e.g., "rtkrover")
 * @return String A unique device name in the format "prefix-CHIPID"
 *
 * @example getDeviceName("rtkrover") might return "rtkrover-B2C3D4"
 */
String getDeviceName(const String& prefix);

/**
 * @brief Extract and return the ESP32's unique chip ID
 *
 * This function reads the ESP32's MAC address from the eFuse and extracts
 * a 32-bit unique chip identifier. The chip ID is derived from specific
 * bytes of the 48-bit MAC address stored in the device's eFuse memory.
 *
 * @return uint32_t The unique 32-bit chip identifier
 *
 * @note This ID remains constant for each ESP32 chip and can be used for
 *       device identification purposes. The function processes bytes from
 *       the MAC address in 8-bit chunks to construct the final ID.
 */
uint32_t getChipId();

#endif /*** HANDLE_WIFI_H ***/
