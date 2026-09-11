# RTKRover

## Headtracker + Real Time Kinematics (RTK rover)

<img align="right" src="./screenshots/rtkrover.jpg" width="360"/>

Hardware used:

* Adafruit Feather ESP32 Huzzah
* SparkFun GPS-RTK-SMA Breakout - ZED-F9P (Qwiic)
* SparkFun BNO080 Breakout
* Ardusimple [Compact Helical Tripleband GNSS Antenna
](https://www.ardusimple.com/product/compact-helical-gnss-tripleband-l-band-antenna-ip67/)
* LiPo battery
* Resistor 10 k
* Switch

Infrastructure:

* an iPhone running RWA Player within BLE range: it is the NTRIP client and
  proxies the corrections over cellular (ADR-001; the assembly has no radio
  but BLE)
* free line of sight between antenna (horizontal placed) an sky

Naming convention (the full glossary is PROJECT-PLAN.md §1.1):

* **Headset assembly** (*assembly*) = the headtracker, what you wear:
  headphones + microphone + ESP32 board + LiPo cell + BNO080 IMU breakout
  + ZED-F9P breakout + antenna. This firmware makes it the **RTK headtracker**;
  the sibling firmware RWAHT (`../rwa-headtracker`) makes a plain headtracker.
  Its **assembly label** is its BLE name and its sticker, e.g. `rwa-hs-2`.
* **Board** = the bare ESP32 Feather; only a flashing-time concept
  (CP2104 serial ↔ assembly label in `tools/known-boards.txt`).
* **Unit** = what you carry with you during the soundwalk: assembly + phone
  + accessories. The **unit label** (`rwa-hs-N`) is the phone's name and the
  `device_id` in telemetry; by convention it equals the assembly label.
* **Rover** is the RTK role of the GNSS receiver (corrected against the
  refnet reference station), not a name for the hardware. It survives in
  the firmware name *rtk-rover* only.

### Circuit diagram

![plot](./fritzing/RTKRover_bb.jpg)

### Configuration

BNO080:

In [`main.cpp`](./src/main.cpp) line 763 (or near) you can choose the way of sensor fusion in the BNO080.

```cpp
// Activate IMU functionalities
// bno080.enableRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
// bno080.enableGameRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
bno080.enableARVRStabilizedRotationVector(BNO080_ROT_VECT_UPDATE_RATE_MS);
```

more information in the [datasheet](https://www.ceva-dsp.com/wp-content/uploads/2019/10/BNO080_085-Datasheet.pdf)

Assembly name:

The firmware embeds one per-assembly fact, the BLE name. It is the assembly
label from `tools/known-boards.txt` (CP2104 serial → label, committed);
`tools/gen_assembly_config.py` generates `src/AssemblyConfig.h` with it at
build time for the selected assembly. Select with the `RTK_BOARD` env var
(label or serial), or just have exactly one known board attached. A board
not in that file advertises `rtkrover-<chip-id>`. Do not edit the generated
header by hand. `tools/fleet-secrets.ini` (gitignored, optional; template in
`tools/fleet-secrets_example.ini`) can override the name with `ble_name` and
otherwise records the caster credentials for provisioning the phones: since
ADR-001 the caster account is typed into RWA Player, not compiled in.

[Support RTK2GO](http://new.rtk2go.com/donations-and-support/)

### PlatformIO

[`platformio.ini`](./platformio.ini) carries no serial-port keys on purpose:
`tools/flash.sh` and `tools/watch.sh` discover the attached board through
`tools/known-boards.txt`. Build, flash and observe are described in
[CLAUDE.md](./CLAUDE.md) (Build & flash); the runtime architecture in
[DOCUMENTATION.md](./DOCUMENTATION.md).

### ESP32 board (red) LED codes

![blink-codes](./assets/blink-codes.svg)

> **Stale:** the diagram still shows the WiFi-era startup (`setupWiFi()`
> → "wait for WiFi connection" → `setupBLE()`); there is no WiFi since
> ADR-001. The list below is current; the diagram needs re-exporting from its
> draw.io source.

#### Startup

* 1.0s 2x: started setup (blocking)
* 0.125s 2x 1.0s 1x (watch for this to spot reboots) (blocking)
* `setupBLE` no blinking. unit is now discoverable
* `setupGNSS`
  * while myGNSS.begin
    * 0.5s: setupGNSS() failed (I2C setup) (blocking)
* FreeRTOS mutex and tasks setup

#### Runtime

Arduino `loop()`

* while no BLE central is connected
  * 0.1s on / 0.1s off (non-blocking); stops as soon as the phone connects

### ESP32 board (yellow) LED codes

* flashing: running on USB power
* steady: Charging
* off: fully charged
