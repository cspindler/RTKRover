/*******************************************************************************
 * @file ble_link.h
 * @brief The BLE link: Bluedroid bring-up, advertising, and the one copy of
 *        the connection state every notifier paces against.
 *
 * Everything that touches the raw Bluedroid API (custom GAP/GATTS handlers,
 * BLEServerCallbacks, esp_ble_* types) lives in ble_link.cpp. A port to
 * another stack (NimBLE, see CLAUDE.md "memory escalation ladder") rewrites
 * that one file; the readers below keep their contract.
 *
 * Writers run on the Bluedroid BTC task and only store atomics; readers are
 * the heading, position and telemetry-drain tasks.
 ******************************************************************************/
#ifndef BLE_LINK_H
#define BLE_LINK_H

#include <Arduino.h>
#include <BLEServer.h>

/// Install the link handlers, start Bluedroid under deviceName and return the
/// server the caller adds its services to. Must be the first BLE call.
BLEServer *bleLinkBegin(const char *deviceName);

/// Advertise serviceUuid (with scan response and the connection-interval
/// preference) and start. Advertising is stopped on connect and restarted on
/// disconnect by the link itself.
void bleLinkStartAdvertising(const char *serviceUuid);

/// A central is connected.
bool bleLinkConnected();

/// Bumped on every connect. A notifier that streams partial frames resets its
/// stream state when it changes, so a new central never receives the tail of
/// a frame half-sent to the previous one.
uint32_t bleLinkGeneration();

/// Effective ATT MTU: 23 until the central exchanges (iOS asks for ~185 right
/// after connecting). A notification payload larger than MTU-3 is silently
/// truncated by Bluedroid.
uint16_t bleLinkMtu();

/// Connection interval the central granted, in 1.25 ms units; 0 until the
/// stack reports it (and again after a disconnect).
uint16_t bleLinkConnIntervalUnits();

/// True while the GATT server's TX queue is full (ESP_GATTS_CONGEST_EVT). A
/// flag older than BLE_TX_CONGESTION_MAX_MS counts as a missed "cleared"
/// event and is dropped: a notifier must never freeze waiting for it.
bool bleLinkTxCongested();

#endif /*** BLE_LINK_H ***/
