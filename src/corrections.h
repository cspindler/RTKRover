/*******************************************************************************
 * @file corrections.h
 * @brief The correction loop over BLE (ADR-001, PROJECT-PLAN.md par. 5.6):
 *        RTCM from the phone into a chunk FIFO for the receiver, the
 *        receiver's GGA to the phone for the caster.
 *
 * The phone is the NTRIP client. It writes the caster's RTCM byte stream to
 * RTCM_CHARACTERISTIC_UUID in MTU-sized chunks (write without response); the
 * write callback runs on the Bluedroid task and only copies the chunk into a
 * static drop-oldest FIFO. The corrections task pops the chunks and pushes
 * them to the ZED-F9P under the GNSS mutex, so I2C and mutex waits never
 * land on the BT task that also carries the heading notifies. The same task
 * notifies each fix-quality GGA the receiver produces on
 * GGA_CHARACTERISTIC_UUID; the app forwards the latest one to the caster.
 ******************************************************************************/
#ifndef CORRECTIONS_H
#define CORRECTIONS_H

#include <Arduino.h>
#include <BLEService.h>

/// Create the correction characteristics on the tracker service. Call before
/// the service is started.
void correctionsBleSetup(BLEService *pService);

/// True when at least one RTCM chunk waits in the FIFO (cheap; lets the
/// consumer skip the mutex take on an idle link).
bool correctionsRtcmQueued();

/// Oldest queued RTCM chunk into out; returns its length, 0 if none. A chunk
/// larger than outCap is discarded (counted), never wedges the queue.
size_t correctionsPopRtcm(uint8_t *out, size_t outCap);

/// Chunks evicted or rejected since boot (FIFO full because the consumer did
/// not drain, or a write larger than the FIFO). Feeds rtcm_fifo_overflow.
uint32_t correctionsRtcmDropped();

/// Notify one GGA sentence (its line terminator is stripped) on the GGA
/// characteristic. Returns false, and sends nothing, while no central is
/// connected or the sentence would not fit one notification (MTU-3): the
/// receiver brings a fresh one next epoch, nothing is split or queued.
bool correctionsNotifyGga(const uint8_t *sentence, size_t len);

#endif /*** CORRECTIONS_H ***/
