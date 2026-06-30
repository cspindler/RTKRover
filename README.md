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

If you are NOT using the web form of the RTKBaseManager, then to connect to a caster you will need to create (from `CasterSecrets_example.h`) and fill out the `CasterSecrets.h` that lives in your src folder with your own credentials (and replace the vars with the k prefixed values e.g.: `mountPoint` to `kMountPoint` in the [`main.cpp`](./src/main.cpp)).

```cpp
#ifndef CASTER_SECRETS_H
#define CASTER_SECRETS_H
const char kCasterHost[] = "rtk2go.com";
const char kCasterPort[] = "2101";
const char kMountPoint[] = "YOUR_MOUNT_POINT";
const char kCasterUser[] = "YOUR_USER_EMAIL";        // User must provide their own email address to use RTK2Go
const char kCasterPass[] = "";                       // Not neccecary, more info: rtk2go.com

// Device name
const char kDeviceName[] = "YOUR_DEVICE_NAME";

// WiFi access
const char kWifiSsid[] = "YOUR_SSID_WITHOUT_SPACES"; // Wifi to connect the rover with
const char kWifiPw[] = "YOUR_WIFI_PASSWORD";

#endif /*** CASTER_SECRETS_H ***/

```

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
* 0.125s 4x (blocking)
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
