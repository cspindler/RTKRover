# RTKRover

## Headtracker + Real Time Kinematics (RTK rover)

<img align="right" src="./screenshots/rtkrover.jpg" width="360"/>

Hardware used:

* Adafruit Feather ESP32 Huzzah
* SparkFun GPS-RTK-SMA Breakout - ZED-F9P (Qwiic)
* SparkFun BNO080 Breakout
* ublox [ANN-MB1](https://www.u-blox.com/en/product/ann-mb-series?legacy=Current) antenna (the small one the picture is not used here at the moment)
* LiPo battery
* Push button(s)
* Resistor 10 k
* Switch

Infrastructure:

* WiFi (e. g. a personal hotspot)
* free line of sight between antenna (horizontal placed) an sky

### Dependencies

* [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
* [RTKRoverManager](https://github.com/jangleboom/RTKRoverManager)

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

ZED-F9P:

Caster and WiFi credentials live in `tools/fleet-secrets.ini` (gitignored; copy
from `tools/fleet-secrets_example.ini`), keyed by the board labels from
`tools/known-boards.txt`. At build time `tools/gen_caster_secrets.py` generates
`src/CasterSecrets.h` from them for the selected unit — select it with the
`RTK_BOARD` env var (label or CP2104 serial), or just have exactly one known
unit attached. Do not edit the generated header by hand.

```ini
[caster]
host = rtk2go.com
port = 2101
mount = YOUR_MOUNT_POINT
pass =

[rwa-hs-1]
caster_user = YOUR_CASTER_USER_01
wifi_pw = HOTSPOT_PASSWORD_OF_UNIT_1
```

The WiFi SSID equals the board label (name the phone hotspot after the unit);
`wifi_ssid` in a board section overrides that. The BLE device name is derived
from the chip ID at runtime and is not configured anywhere.

The mklittlefs file in the root dir you have to [get](https://github.com/earlephilhower/mklittlefs/releases) depending on your OS.
If you have the Arduino IDE installed, you can borrow it from there too. On macOS you can find it here: `~/Library/Arduino15/packages/esp32/tools/mklittlefs/3.0.0-gnu12-dc7f933/mklittlefs`.  Help for setup the file system you can find [here](https://randomnerdtutorials.com/esp8266-nodemcu-vs-code-platformio-littlefs/). This project was created on macOS (silicon).

[Support RTK2GO](http://new.rtk2go.com/donations-and-support/)

### PlatformIO

Update the serial-port paths in [`platformio.ini`](./platformio.ini) to the values read from PlatformIO "Devices" command (`platformio device list`).

### ESP32 board (red) LED codes

![blink-codes](./assets/blink-codes.svg)

#### Startup

* 1.0s 2x: started setup (blocking)
* 0.125s 2x 1.0s 1x (watch for this to spot reboots) (blocking)
* `setupWiFi`
* 0.125s 4x (blocking, after first WiFi connection attempt)
* while wait for WiFi Connection
  * 1.0s, 0.1s (blocking)
* `setupBLE` no blinking
* `setupGNSS`
  * while myGNSS.begin
    * 0.5s: setupGNSS() failed (I2C setup) (blocking)
* FreeRTOS queues and tasks setup

#### Runtime

> legacy, check if still applicable
>
> * 1.0 s RTK: setupGNSS() failed (I2C communication)
> * 2.0 s RTK: credentialsExists false

`task_rtk_get_corrrection_data`

* no credentials (needs to removed)
* while wait for WiFi Connection
  * 1.0s, 0.1s: connection to AP lost (blocking)

`task_send_rtk_position_via_ble`

* while (!bleConnected)
  * 0.1s (non-blocking)
* when not connected in while-loop
  * 3x 0.1s, 1.0s wait

### ESP32 board (yellow) LED codes

* flashing: running on USB power
* steady: Charging
* off: fully charged
