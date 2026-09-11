# CLAUDE.md - rtk-rover

ESP32 firmware for a head-mounted RTK GNSS + head-tracking unit, part of an audio
augmented reality installation. Read `PROJECT-PLAN.md` (repo root) first, it defines
the system architecture and the telemetry event schema (§4–5) that this firmware
must implement. The schema is a cross-repo contract; do not change field names or
semantics here without updating PROJECT-PLAN.md and the `rwa-player` decoder.

## Hardware

- Adafruit Feather ESP32 Huzzah
- u-blox ZED-F9P (SparkFun GPS-RTK-SMA, Qwiic/I²C): RTK GNSS, UBX protocol
- Bosh BNO080 (SparkFun breakout, I²C): head-tracking IMU
- Both sensors use a dedicated I²C bus
- Radio: BLE only (ADR-001, 0.48.0). The phone is the NTRIP client and proxies
  RTCM down / GGA up over BLE (PROJECT-PLAN.md §5.6); the firmware has no WiFi.

## Runtime architecture

See [DOCUMENTATION.md](./DOCUMENTATION.md): task table (core, priority, period, stack),
synchronisation primitives, boot order, callback contexts.

## Build & flash

Toolchain is **PlatformIO + Arduino framework** (not ESP-IDF). All configuration lives in `platformio.ini` and `src/RTKRoverConfig.h`.

- Envs: `featheresp32` (production, the default) and `featheresp32_debug`
  (adds `-DDEBUGGING=1`, so `DBG` serial logging is live). Shared settings live in
  the `[env]` section; the two envs differ only by that flag.
- Build both, no hardware needed: `pio run -e featheresp32 -e featheresp32_debug`
- Builds are incremental (~13 s warm, ~2 min cold after a `pio system prune` or a
  dependency bump). Debug costs ~30 KB flash / ~0.5 KB RAM over production.
- Flash: `tools/flash.sh` (~37 s; the 1.6 MB image alone takes ~29 s to write)
- Observe: `tools/watch.sh 30`
- Both: `tools/flash.sh && tools/watch.sh 30`. deliberately two commands, so a
  failed upload and a failed capture surface as distinct exit codes.
- Which board: `tools/find-board.sh` resolves the attached unit against
  `tools/known-boards.txt` (CP2104 serial → label) and prints its device path.
  `flash.sh`/`watch.sh` call it and pass the port explicitly. Add new units to
  that file; take the serial from the `SER=` field of `pio device list`.
  Select a specific one with `tools/flash.sh <env> <serial-or-label>` or
  `RTK_BOARD=rover-01`. Zero matches, an unknown name, and two attached units are
  three distinct errors — none of them silently uses the wrong board.
- Serial baud: 115200 (`monitor_speed`). `platformio.ini` intentionally has no
  `upload_port`/`monitor_port`/`test_port` keys — a hardcoded port embeds one
  unit's serial number and goes stale on a board swap, which is exactly how it
  broke before. Discovery replaces it.
- Partition table: `min_spiffs.csv` (two 1.92 MB OTA slots, 128 KB SPIFFS
  unused, 64 KB coredump). Changing the table needs a USB flash of every unit;
  `pio run -t upload` rewrites it.

### Observing the device (autonomous iterate → build → flash → observe loop)

**Never call `pio device monitor` from a script or tool call.** It is not just
that it fails to exit — pyserial's miniterm calls `termios.tcgetattr()` on stdin
and dies with `termios.error: (102, 'Operation not supported on socket')` the
moment stdin is not a TTY. It only works when a human is at a terminal.

Use `tools/watch.sh` instead. It wraps `tools/serial-capture.py`, a direct
pyserial reader that needs no terminal: it resets the board on open, timestamps
each line, and exits when the window closes.

The three tools are deliberately separate — board resolution, upload, and capture
each fail in their own way, and bundling them hid which one broke:

| script | description |
|---|---|
| `tools/find-board.sh` | resolve attached unit → device path |
| `tools/flash.sh` | build + upload (thin; `pio` already exits properly) |
| `tools/watch.sh` | bounded serial capture (the part that needs wrapping) |

- The capture window counts serial time only; the ~37 s upload is not billed
  against it. Don't set the window shorter than the output you're waiting for —
  the phone connects ~1 s after advertising, but the first `gnss_fix` with a
  fix, and with it the first GGA notify, needs the receiver to acquire (tens of
  seconds from cold).
- `watch.sh` uses `~/.platformio/penv/bin/python`, the only interpreter here with
  pyserial. The system `python3` does not have it.
- **Debug builds block in `setup()`** on `Press any key to continue...`
  ([src/main.cpp:255](src/main.cpp:255)) and will never reach `loop()` unattended.
  `serial-capture.py` sends newlines at 0.5/1.5/3.0 s to get past it. If that
  prompt moves or a second one is added, the capture will hang at the new prompt
  until the window expires — update the nudge, not the window.
- Serial output is gated by `DEBUGGING` (the `DBG` macro in
  `src/RTKRoverConfig.h`). It defaults to `0`; the `featheresp32_debug` env turns
  it on. Don't edit the header to toggle it — build the debug env instead, so the
  switch never shows up in a diff.
- Crash triage: grep the log for `Guru Meditation`, `abort()`, `Brownout`. Do
  *not* rely on the ROM banner (`rst:0x...`, `boot:0x..`) — this board keeps it
  silenced, so it never appears even on a real reset. App-level panic output does.
- Only one process may hold the port. Never leave a background capture running
  across an upload; kill it first or uploads fail with "port busy".
- Auto-reset into the bootloader uses DTR/RTS and normally works. If upload fails
  with "Failed to connect", the board needs the physical BOOT button held —
  that requires the operator, so ask rather than retrying in a loop.

### What is needed from the operator

Hardware-in-the-loop iteration needs, once per machine/session:

1. The board physically plugged in over USB (nothing else is a substitute, the
   port cannot be reached remotely).
2. The board listed in `tools/known-boards.txt` (CP2104 serial → assembly
   label). The label is the BLE name; `tools/gen_assembly_config.py` writes it
   into `src/AssemblyConfig.h` at build time (extra_script, like the
   fw-version header). Never edit the header by hand. Board selection:
   `RTK_BOARD` (label or serial; `flash.sh` exports its board argument), else
   the single attached unit. With neither, the previous header is kept (or an
   empty placeholder is written on a fresh clone) so hardware-less builds still
   work. `tools/fleet-secrets.ini` (gitignored; template
   `tools/fleet-secrets_example.ini`) is optional since ADR-001: the build reads
   only a `ble_name` override from it. The caster credentials it records (each
   unit has its own caster username, `musikbasel01`, `musikbasel02`, ...) are
   for provisioning the phones, not the firmware.
3. The unit's phone with RWA Player running, within BLE range:
   - Settings -> Unit ID = the unit label; the app connects to the assembly
     advertising that name. A board not in `known-boards.txt` advertises the
     fallback `rtkrover-<chip-id>` (last 6 hex digits of the ESP32 MAC, e.g.
     `rtkrover-2d3810`); enter that under Settings -> Headset assembly to
     connect to it.
   - Corrections flow only while the app's NTRIP client (caster credentials in
     its Settings) holds a session: the assembly has no path to the caster of
     its own. Without the app the receiver runs plain GNSS, no RTK.

## Conventions & constraints

- RAM: BLE is the only radio since 0.48.0 (ADR-001), which took steady free
  heap from ~11 KB to the ~60 KB range (CHANGELOG 0.48.0 bench notes). Still
  prefer static allocation and check the free-heap impact of any new buffer
  with the debug build's 10 s heap/stack-watermark report, and keep task stacks
  sized from measured watermarks (see setup() comment in main.cpp). The
  telemetry ring's 4 KB and the RTCM FIFO's 4 KB are sized for their purpose,
  no longer by the heap.
- Never log with blocking printf from time-critical tasks; route through the
  telemetry ring buffer (or ESP_LOG for local-USB debugging only).
- Error events use stable short `code` strings (e.g. `i2c_timeout_bno080`) — these
  become Grafana alert dimensions; don't rename casually.
- Versioning: `fw_version` = semver + short git hash, embedded at build time and
  reported in every heartbeat event.
- Partition table: `min_spiffs.csv`, decided 2026-09-11 (two OTA slots of
  1.92 MB, `ota_0`/`ota_1` + `otadata`). Chosen when the image was ~1.63 MB and
  the stock `default.csv` slots (1.25 MB) could not hold it; since 0.48.0 (WiFi
  removed) the image is ~1.21 MB, 61 % of the slot. Nothing in the tree uses
  SPIFFS or LittleFS, so its 128 KB region costs nothing. The tree shipped
  `no_ota.csv` until then; changing tables costs one USB flash per unit, so
  it stays.
- Check the `Flash:` line of every `pio run` and flag growth toward the slot
  ceiling; the ~750 KB of headroom since 0.48.0 is what OTA (ADR-002) works in.
- **Memory escalation ladder** (decided 2026-07-29, superseded by ADR-001 on
  2026-09-11). It existed because BLE + WiFi + lwIP left ~6–8 KB minimum free
  heap with no IDF knobs to turn (the Arduino framework ships ESP-IDF 4.4.7
  precompiled, no `menuconfig`). Removing WiFi freed ~50 KB, so none of its
  triggers apply. Kept for reference should a second radio ever return:
  1. Port BLE to NimBLE-Arduino (stays in Arduino; ~30–50 KB heap, ~100 KB
     flash; `src/ble_link.cpp` is the one file it rewrites).
  2. Rebuild as "Arduino as an IDF component" (unlocks `sdkconfig`).
  3. Full IDF rewrite — effectively never justified.

## Current work queue

1. ~~Telemetry GATT service (TX notify + CTRL write characteristics)~~ done 2026-07-29
2. ~~Ring buffer + telemetry task; CBOR encoding with short integer keys~~ done 2026-07-29
3. ~~Event emitters: gnss_fix (1 Hz), heartbeat (15 s), ntrip_status, imu_status, error~~ done 2026-07-29
4. ~~CTRL commands: set verbosity, status dump~~ done 2026-07-29 (device side; app sends nothing yet)
5. ~~OTA partition table~~ done 2026-09-11 (`min_spiffs.csv`; version embedding
   was already done). OTA *delivery* (backend → app → BLE chunked transfer,
   PROJECT-PLAN §8.1) is not started.
6. Live end-to-end check with the updated RWA Player build (device events in
   the Diagnostics tab), then watch real events land in Grafana (§9 step 4)
7. ADR-001 firmware side done 2026-09-11 (branch `ble-only-transport`, 0.48.0):
   WiFi and the NTRIP client removed, RTCM downlink `713D0006`, GGA uplink
   `713D0007`, 15–30 ms connection-interval request. Open, in order: the
   rwa-player NTRIP client and decoder update for the retired heartbeat keys
   (PROJECT-PLAN.md §6 item 6), the ADR's §7 bench A/B for acceptance, the
   Grafana panels keyed on the retired fields.
