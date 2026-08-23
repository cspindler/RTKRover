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

## Runtime architecture

TODO, see [DOCUMENTATION.md]([./DOCUMENTATION.md]) for an attempt of documenting this.

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
- TODO: document partitioning / OTA

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
  WiFi association alone takes ~11 s to first log line.
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
2. `tools/fleet-secrets.ini` present (gitignored; copy from
   `tools/fleet-secrets_example.ini`) and a reachable WiFi/NTRIP caster.
   `src/CasterSecrets.h` is generated from it at build time by
   `tools/gen_caster_secrets.py` (extra_script, like the fw-version header).
   Never edit the header by hand. Per-assembly facts are keyed by the assembly label
   from `tools/known-boards.txt`; each unit has its own caster username
   (`musikbasel01`, `musikbasel02`, ...). Board selection: `RTK_BOARD` (label or
   serial; `flash.sh` exports its board argument), else the single attached
   unit. With neither, the previous header is kept (or an empty placeholder is
   written on a fresh clone) so hardware-less builds still work; a selected
   board with incomplete secrets fails the build on purpose.
3. The unit's phone with RWA Player running, providing the WiFi hotspot:
   - Hotspot name = unit label (e.g. `rwa-hs-2`, which is also the assembly's
     BLE name), so the SSID needs no config of its own. A `wifi_ssid` override
     per assembly section exists for hotspots not yet renamed.
   - In RWA Player, Settings -> Unit ID = the unit label; the app connects to
     the assembly advertising that name. Only an assembly without a
     fleet-secrets entry advertises the fallback `rtkrover-<chip-id>` (last 6
     hex digits of the ESP32 MAC, e.g. `rtkrover-2d3810`). Enter that under
     Settings -> Headset assembly to connect to it.

## Conventions & constraints

- RAM is tight: BLE + WiFi coexist. Prefer static allocation; check free heap
  impact of any new buffer. The telemetry ring buffer is capped at 4 KB
  (8 KB caused connect-time OOM panics, measured 2026-07-29). Steady-state
  free heap is ~18 KB with ~13 KB min — verify with the debug build's 10 s
  heap/stack-watermark report before adding buffers, and keep task stacks
  sized from measured watermarks (see setup() comment in main.cpp).
- Never log with blocking printf from time-critical tasks; route through the
  telemetry ring buffer (or ESP_LOG for local-USB debugging only).
- Error events use stable short `code` strings (e.g. `i2c_timeout_bno080`) — these
  become Grafana alert dimensions; don't rename casually.
- Versioning: `fw_version` = semver + short git hash, embedded at build time and
  reported in every heartbeat event.
- Partition table: two-OTA-slot scheme (ota_0/ota_1 + otadata) is the target for
  deployed builds, but the tree currently ships `no_ota.csv`. **Blocking conflict:**
  the app is already ~1.63 MB (77.7 % of the 2 MB single slot), so the stock
  `default.csv` (1.25 MB per OTA slot) will not link. Moving to OTA needs
  `min_spiffs.csv` (~1.9 MB/slot) or a custom table — decide before item 5 in the
  work queue.
- Flash headroom is the binding budget, not just RAM (RAM is at 18 %). Check the
  `Flash:` line of every `pio run` and flag growth toward the slot ceiling.
- **Memory escalation ladder** (decided 2026-07-29; context: Arduino framework
  ships ESP-IDF 4.4.7 precompiled, so IDF config like Bluedroid pools and WiFi
  buffer counts is NOT tunable here — no `menuconfig`). If heartbeat telemetry
  shows sustained free-heap minimums under ~6–8 KB, escalate in this order;
  do not jump straight to an IDF migration:
  1. Port BLE to NimBLE-Arduino (stays in Arduino; frees ~30–50 KB heap AND
     ~100 KB flash, which also helps the OTA slot conflict above). Expected
     first lever.
  2. Rebuild as "Arduino as an IDF component" (code unchanged, unlocks
     `sdkconfig`/menuconfig for IDF memory knobs).
  3. Full IDF rewrite — effectively never justified; option 2 provides the
     same knobs without one.

## Current work queue

1. ~~Telemetry GATT service (TX notify + CTRL write characteristics)~~ done 2026-07-29
2. ~~Ring buffer + telemetry task; CBOR encoding with short integer keys~~ done 2026-07-29
3. ~~Event emitters: gnss_fix (1 Hz), heartbeat (15 s), ntrip_status, imu_status, error~~ done 2026-07-29
4. ~~CTRL commands: set verbosity, status dump~~ done 2026-07-29 (device side; app sends nothing yet)
5. OTA partition table (version embedding is done; the partition decision —
   `min_spiffs.csv` vs custom table — is still open, see the constraint above)
6. Live end-to-end check with the updated RWA Player build (device events in
   the Diagnostics tab), then watch real events land in Grafana (§9 step 4)
