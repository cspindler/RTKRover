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
 * Link state (connected, MTU, connection generation) comes from ble_link.
 * Wiring expected from main.cpp:
 *   - telemetryBleSetup(pServer) inside setupBLE()
 *   - telemetryBleStartTask() early in setup()
 * and from ble_link.cpp:
 *   - telemetryBleOnDisconnect() from the server's onDisconnect
 ******************************************************************************/
#ifndef TELEMETRY_BLE_H
#define TELEMETRY_BLE_H

#include <Arduino.h>
#include <BLEServer.h>

void telemetryBleSetup(BLEServer *pServer);
void telemetryBleStartTask();

/// The Arduino BLE lib keeps CCCD values across connections; this forgets the
/// TX subscription so a new central must subscribe itself before it is
/// counted as listening.
void telemetryBleOnDisconnect();

/// For the debug-build stack watermark report.
TaskHandle_t telemetryBleTaskHandle();

#endif /*** TELEMETRY_BLE_H ***/
