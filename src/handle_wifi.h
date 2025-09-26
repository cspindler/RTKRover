#ifndef HANDLE_WIFI_H
#define HANDLE_WIFI_H

#include <Arduino.h>
#include <WiFi.h>
#include <RTKRoverConfig.h>
#include <CasterSecrets.h>

/**
 * @brief Setup WiFi connection by scanning for the target network and connecting
 *
 * This function waits for the target WiFi network (defined by kWifiSsid) to become
 * available by scanning, then attempts to connect to it using station mode.
 *
 * @return true If WiFi connection was successfully established
 * @return false If WiFi connection failed
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
 * @brief Check if a specific WiFi network is available by scanning
 *
 * This function performs a WiFi network scan and searches for a network
 * with the specified SSID. It returns true if the network is found among
 * the available networks.
 *
 * @param ssid The SSID of the network to search for
 * @return true If the specified network was found in the scan results
 * @return false If no networks were found or the specified SSID was not detected
 */
bool checkNetworkAvailable(const String& ssid);

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
