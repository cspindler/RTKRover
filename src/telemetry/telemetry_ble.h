/*******************************************************************************
 * @file telemetry_ble.h
 * @brief Telemetry GATT service (TX notify + CTRL write) and the drain task.
 *
 * PROJECT-PLAN.md par. 5: the drain task runs at the lowest priority in the
 * system and paces its output (<= TELEMETRY_MAX_NOTIFY_PER_TICK notifications
 * per TELEMETRY_TICK_MS), so even a full-ring backlog drain cannot crowd the
 * headtracker notifications. The TX characteristic is a byte stream (par. 5.2):
 * frames are packed into notifications up to the negotiated MTU-3 and may span
 * notification boundaries; the app reassembles via the length prefix.
 *
 * Wiring expected from main.cpp:
 *   - telemetryBleSetup(pServer) inside setupBLE()
 *   - telemetryBleStartTask() once tasks are created in setup()
 *   - telemetryBleOnConnect/OnDisconnect/OnMtuChanged from the
 *     BLEServerCallbacks (MTU matters: notifying more than MTU-3 bytes would
 *     be silently truncated by Bluedroid and corrupt the stream)
 ******************************************************************************/
#ifndef TELEMETRY_BLE_H
#define TELEMETRY_BLE_H

#include <Arduino.h>
#include <BLEServer.h>

void telemetryBleSetup(BLEServer *pServer);
void telemetryBleStartTask();

void telemetryBleOnConnect();
void telemetryBleOnDisconnect();
void telemetryBleOnMtuChanged(uint16_t mtu);

/// For the debug-build stack watermark report.
TaskHandle_t telemetryBleTaskHandle();

#endif /*** TELEMETRY_BLE_H ***/
