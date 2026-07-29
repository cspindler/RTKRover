# CLAUDE.md - rtk-rover

ESP32 firmware for a head-mounted RTK GNSS + head-tracking unit, part of an audio
augmented reality installation. Read `PROJECT-PLAN.md` (repo root) first, it defines
the system architecture and the telemetry event schema (§4–5) that this firmware
must implement. The schema is a cross-repo contract; do not change field names or
semantics here without updating PROJECT-PLAN.md and the `rwa-client` decoder.

## Hardware

- Adafruit Feather ESP32 Huzzah
- u-blox ZED-F9P (SparkFun GPS-RTK-SMA, Qwiic/I²C): RTK GNSS, UBX protocol
- Bosh BNO080 (SparkFun breakout, I²C): head-tracking IMU
- Both sensors use a dedicated I²C bus

## Runtime architecture

(TODO)

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
2. `src/CasterSecrets.h` present (gitignored; copy from `CasterSecrets_example.h`)
   and a reachable WiFi/NTRIP caster.
3. Mobile device with RWA Client running, provding the WiFi hotpot:
   - Add WiFi name to `src/CasterSecrets.h` (or the other way around: Set WiFi name in caster secrets and set your mobile-device / hotspot name to the same).
   - Add `rtkrover-<esp32-slug>` as headtracker (BT) name in RWA Player, `<esp32-slug>` being the last 6 characters of the ESP32 serial number (i.e. `2d3810`)

## Conventions & constraints

- RAM is tight: BLE + WiFi coexist. Prefer static allocation; check free heap
  impact of any new buffer. The telemetry ring buffer is capped at ~8 KB.
- Flash headroom is the binding budget, not just RAM (RAM is at 18 %). Check the
  `Flash:` line of every `pio run` and flag growth toward the slot ceiling.

