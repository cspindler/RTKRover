# rwa Telemetry & Fleet Infrastructure — Project Plan

> Shared context document for the `rtk-rover`, `rwa-player`, and `rwa-backend` repositories.
> Lives in Claude project knowledge and in the root of each repo. Update it when architectural
> decisions change — it is the single source of truth for the cross-repo contract.

Status: v3 2026-08-23 (v1 draft 2026-06-10, v2 draft 2026-06-29)

This is currently tracked in the `rtk-rover` repository, to document the implementation process.
Once done, move it back into the directory containing all project repos (or symlink it), so other repos can reference to it as well.

Deployment target: ~10 units, one permanent installation, 2-month run.

---

## 1. System overview

An audio augmented reality (AAR) installation. Visitors wear a headset assembly (headphones
with a mounted sensor board) and carry an iPhone (provided by us) that runs the soundwalk app
and acts as the gateway between the assembly and our backend.

| Component | Repo | Role |
| --- | --- | --- |
| rtk-rover | `rtk-rover` (local checkout: `RTKRover_mod`) | ESP32 firmware of the **RTK headtracker**: RTK GNSS + head tracking + telemetry, streamed over BLE |
| RWAHT | `rwa-headtracker` | ESP32 firmware of the plain **headtracker**: head tracking only (no GNSS, no telemetry) |
| rwa-player | `rwa-player` | iOS app (previously called rwa-client): binaural soundwalk playback, BLE central, telemetry gateway |
| rwa-backend | `rwa-backend` | docker-compose stack on AWS EC2: ingest, storage, dashboards |

Note on naming rwa-player: The app/repo previously was called rwa-client. Repo name and in-app
strings are done; directories inside the Xcode project still carry `rwaClient` (renaming those
with an Xcode project relying on them is dangerous).

### 1.1 Glossary

The words below are the contract. Code, docs, UI strings and wire fields in all repos use them;
the "retired" column lists what they replace.

| Term | Meaning | Identity / notes | Retired words |
| --- | --- | --- | --- |
| **Headset assembly** (short: *assembly*) | What the visitor wears: headphones + microphone + ESP32 board + LiPo cell + BNO080 IMU, plus ZED-F9P receiver and antenna in the RTK variant. Two kinds: the **RTK headtracker** (firmware rtk-rover; positioning, heading and diagnostics telemetry) and the plain **headtracker** (firmware RWAHT; heading only). | **Assembly label** = the BLE advertised name = the sticker.<br>RTK variant: assigned at build time from `tools/fleet-secrets.ini` (`ble_name`, defaulting to the section name, e.g. `rwa-hs-2`); assemblies without a fleet entry fall back to `rtkrover-<chip-id>`.<br>RWAHT variant: `rwahtNN`. | "headset", "tracker", "rover" (for the hardware), "device" (for the ESP32) |
| **Rover** | The RTK *role* of the GNSS receiver in the assembly: the mobile receiver whose position is corrected against a fixed base/reference station (the refnet NTRIP caster). Names a function, not hardware. | Lives on in the firmware/repo name *rtk-rover* and in GNSS prose only. | — |
| **Board** | The bare ESP32 Feather. | CP2104 serial / chip id, mapped to the assembly label in `tools/known-boards.txt`. | "board label" (→ assembly label), "unit" for the bare board |
| **Phone** | The iPhone running **RWA Player** (the *app*); the telemetry gateway. | Named after its unit, and its personal hotspot SSID is the unit label. Both are conventions applied by hand at provisioning time: iOS does not let the app read the phone's name, so the unit label is typed into Settings or provisioned. | — |
| **Unit** | Assembly + phone + accessories: what is handed to a visitor. | **Unit label** `rwa-hs-N`. This is `device_id` on the wire (field name kept for backend compatibility; "device" in SQL and backend prose means the unit). By convention unit label == the unit's assembly label == hotspot SSID. | `hs-03`-style ids, "rwa-phone-N" |
| **Session** | One process launch of RWA Player. | `session_id` (UUID), minted at launch. Not rotated on BLE (re)connect. | — |
| **Source** | Where the *data* in an event stems from (§4.2). Not "who sent it": every event reaches the backend through the app. | Closed enum `rtk_headtracker` / `headtracker` / `phone` / `creator`, present on every event. | `rtk_tracker`, `ios_gps`, `osc_sim`, `headtracker_rtk`, `ios_motion`, `ios_app` |
| **seq** | Gateway-assigned event id: the Player's SQLite row id (§4.2). The dedup key. | Persists across launches; restarts only with an app wipe/reinstall. | the 2^32 app-seq offset |
| **dev_seq** | The RTK headtracker's own frame counter (CBOR key 1): per boot, assigned when the firmware *creates* the event. A gap/loss diagnostic, never a key. | Only on events created by the firmware. | `seq` as the device counter |
| **t_dev_ms** | `millis()` on the assembly when the firmware *created* the event; per boot. | Only on events created by the firmware. | — |

Phone-side identity settings (Settings -> Identity, `player-settings.plist` provisioning):

- **Unit ID** (`unitId`): required. Becomes `device_id`.
- **Headset assembly** (`assemblyId`): optional override of the BLE name to connect to. When
  empty, the app connects to the assembly advertising the unit label. Set it only when a phone
  runs with an assembly that is not its unit's own (a spare `rwa-hs-7`, or an RWAHT `rwaht01`).
- Telemetry reports `assembly_id` = the advertised name of the assembly *actually connected*, so
  a unit/assembly mismatch is visible in the backend instead of hidden by the convention.

### Hardware (per RTK headtracker assembly)

- Adafruit Feather ESP32 Huzzah (FreeRTOS runtime)
- SparkFun GPS-RTK-SMA Breakout ZED-F9P (Qwiic/I²C): RTK GNSS
- SparkFun BNO080 Breakout (I²C): head-tracking IMU
- Ardusimple compact helical tripleband GNSS antenna

### Connectivity

- assembly → phone: BLE (head tracking, position, **telemetry**)
- assembly → refnet NTRIP caster: WiFi via the phone's personal hotspot (RTCM corrections)
- phone → backend: HTTPS over cellular (good coverage on site; near-real-time uploads)

### Identity & privacy

- Identities are the fixed labels of §1.1 (unit label, assembly label);
  nothing is derived from the visitor.
- Visitors are not identifiable (we provide the units). No PII is collected.

---

## 2. Goals

1. **Traces / debugging**: stream structured debug events from firmware and app to a
   central location; alert on errors and dead units.
2. **Fleet observability**: live map of all deployed units; per-component health.
3. **RTK GNSS quality statistics** (primary): accumulate fix-quality data over the
   2-month run to identify dead zones / problematic zones on the installation map,
   and to distinguish *GNSS degradation* from *correction-delivery problems*.
4. **OTA groundwork** (secondary): make decisions now that keep firmware OTA and
   soundwalk-content OTA cheap to add later. No OTA delivery in v1.

---

## 3. Architecture: gateway pattern

All telemetry flows **assembly → BLE → app → HTTPS → backend**. The ESP32 never talks
to the backend directly.

- refnet (NTRIP) ⇢ [WiFi via phone hotspot] ⇢ ESP32 (rtk-rover)
- ESP32 (rtk-rover) ⇢ [BLE] ⇢ phone app (rwa-player) ⇢ [HTTPS] ⇢ backend (rwa-backend)

![rtk_rover_telemetry_architecture](./rtk_rover_telemetry_architecture.svg)

Rationale:

- The BLE pipe already exists; telemetry is one more characteristic.
- ESP32 WiFi stays dedicated to the NTRIP stream (RAM, power, reliability).
- The app **enriches** firmware events with context only it knows (soundwalk ID, session,
  app version, wall-clock time) — essential for correlating GNSS quality with content.
- Store-and-forward buffering belongs on the phone (SQLite), not the microcontroller.

---

## 4. Event schema v1 (the contract)

Two representations, same semantics:

- **BLE (assembly → app)**: CBOR-encoded, length-prefixed frames (see §5).
- **HTTPS (app → backend)**: JSON batches as below. The app is responsible for the mapping.

### 4.1 Batch envelope (`POST /v1/batch`)

Headers: `Authorization: Bearer <INGEST_TOKEN>`, `Content-Type: application/json`,
optionally `Content-Encoding: gzip`.

```json
{
  "schema": 1,
  "device_id": "rwa-hs-3",
  "session_id": "2A6F…-uuid",
  "fw_version": "0.44.3+6e967cd",
  "app_version": "3.1.13 (212)",
  "soundwalk_id": "walk-harbor-v2",
  "events": [ { …event }, { …event } ]
}
```

- `device_id`: the **unit label** (§1.1).
- `session_id`: generated once per process launch of RWA Player. New launch = new UUID. It is
  **not** rotated on headset connect, walk start, or anything else.
- `fw_version`: the connected RTK headtracker's firmware version as last reported in its
  `heartbeat`; the literal `unknown` until the first heartbeat of the launch arrived.
- Envelope fields apply to every event in the batch (they rarely change mid-batch;
  if they do, the app closes the batch and starts a new one).

### 4.2 Common event fields

| Field | Type | Assigned by | Notes |
| --- | --- | --- | --- |
| `seq` | uint64 | app | The Player's SQLite row id of the event (`pending_events.id`, AUTOINCREMENT). Monotonic per phone install, never reused. **Dedup key.** |
| `time` | RFC3339 string | app | Wall clock at BLE receipt / app emit. For firmware events this is when the *phone got* the frame, which can be up to ~40 s after `t_dev_ms` (ring buffer, §5). |
| `source` | string | app | Where the data stems from: `rtk_headtracker`, `headtracker`, `phone`, `creator`. Present on every event. |
| `type` | string | — | One of the types below |
| `dev_seq` | uint32 | firmware | The RTK headtracker's frame counter at creation: restarts at 1 on every boot, gaps mark dropped frames. Diagnostic only. Absent on events created by the app. |
| `t_dev_ms` | uint32 | firmware | `millis()` on the assembly at creation; restarts on every boot. Absent on events created by the app. Together with `dev_seq` this marks an event as "created by the firmware". |

**Dedup.** The identity of an event is `(device_id, seq)`: one unit, one ever-increasing
counter owned by the gateway, which is the only party that persists and retries. The backend's
unique index is `(device_id, session_id, seq, time)`:

- `session_id` is in the key only as the namespace that makes a counter restart harmless:
  `seq` restarts at 1 after an app wipe or reinstall (or when a spare phone takes over a unit
  label), and without `session_id` the first events of the new install would be silently
  dropped as duplicates of the old install's. It is not part of the identity and must not be
  read as such; concurrent sessions on one unit are impossible.
- `time` is in the unique index only because TimescaleDB requires the partition column there.
  Events recorded in the same instant are told apart by `seq`.

The firmware's counter (`dev_seq`) is deliberately **not** part of the key: it has no
cross-boot uniqueness, so a headset reboot mid-session would otherwise collide.

### 4.3 Event types

Every type carries the common fields above. The matrix says which values of `source` occur
for each type; "firmware" cells are created on the RTK headtracker and carry
`dev_seq`/`t_dev_ms`, "app" cells are created by RWA Player.

| type | `rtk_headtracker` | `headtracker` | `phone` | `creator` |
| --- | --- | --- | --- | --- |
| `gnss_fix` | firmware | - | app (CoreLocation) | app (OSC position) |
| `heading` | app (from the BLE text protocol) | app (from the BLE text protocol) | app (CoreMotion) | - |
| `heartbeat` | firmware | - | app | - |
| `ntrip_status`, `imu_status`, `error` | firmware | - | - | - |
| `app_event` | - | - | app | - |

There is exactly one `gnss_fix` stream per source: for the RTK headtracker the
firmware record is the fix; the app does not build its own copy from the text
protocol. The `phone` stream (CoreLocation) is continuous (even while the RTK
headtracker drives the hero), so the two positioning systems can be compared
over the same walk; `source` separates them. Which source *drives the hero* is a
separate statement, reported as `position_source` in the app heartbeat.

**`gnss_fix`**: emitted at 1 Hz from UBX-NAV-PVT (+ correction-age from RXM-COR/RTCM bookkeeping).
This is the dead-zone dataset; do not thin it out.

| Field | Type | Notes |
| --- | --- | --- |
| `lat`, `lon` | double | degrees (UBX 1e-7 scaled → float on device) |
| `height_m` | float | ellipsoidal height |
| `fix_type` | uint8 | UBX fixType: 0 none … 3 = 3D, 4 = GNSS+DR |
| `carr_soln` | uint8 | 0 = none, 1 = RTK float, 2 = RTK fixed |
| `h_acc_mm`, `v_acc_mm` | uint32 | u-blox accuracy estimates |
| `num_sv` | uint8 | satellites used |
| `pdop` | float | |
| `corr_age_ms` | uint32 | age of last RTCM correction applied; 0xFFFFFFFF = never |

App-created fixes (`source` = `phone` from CoreLocation, `creator` from the simulator) carry
`lat`, `lon` and whatever of the above CoreLocation can provide (`h_acc_mm`, `v_acc_mm`,
`height_m`, `fix_type`; `carr_soln` = 0). `num_sv`, `pdop`, `corr_age_ms` are absent.

**`heading`**: app-created at 1 Hz while a heading source is running: `azimuth`, `elevation`
(degrees, after north calibration), `step_count`, `assembly_id` (BLE name of the connected
assembly; absent for `phone`).

**`heartbeat`** (firmware): every 15 s from the RTK headtracker.

| Field | Notes |
| --- | --- |
| `uptime_ms`, `free_heap` | basic health |
| `wifi_rssi` | hotspot link quality (dBm) |
| `ntrip_connected` | bool |
| `fw_version` | redundant with envelope, but lets the backend detect mismatches. Cut off after 32 bytes on the BLE leg — enough for semver plus git hash |
| `dropped_frames` | cumulative count of telemetry frames dropped on-device (ring-buffer overflow); resets on boot |
| `batt_mv` | LiPo pack voltage in mV, read from the Feather's 2:1 divider on A13 (ADC1, unaffected by WiFi). Single-cell: ~4200 full, ~3300 empty. 0 = unknown. Consumers derive a percentage; the firmware ships no discharge curve |

**`heartbeat`** (app, `source` = `phone`): every 15 s from RWA Player: `uptime_ms` (app),
`assembly_connected` (bool), `assembly_id` (BLE name of the connected assembly), `assembly_rssi`
(dBm), `walk_running` (bool), `position_source` / `heading_source` (the `source` value currently
driving the hero, or `none`), `batt_pct` (phone battery).

**`ntrip_status`**: on state change: `state` ("connected" / "disconnected" / "reconnecting"),
`reconnects` (counter), `bytes_rx` (cumulative).

**`imu_status`**: every 60 s, and only while a phone is connected over BLE:
`calib_status`, `report_rate_hz`, `resets`. Unlike `heartbeat`, nothing is recorded while the
assembly sits disconnected, so a gap in this stream means "no phone attached", not "IMU dead".

**`error`**: `severity` (1 = warn, 2 = error, 3 = fatal), `code` (stable short string),
`msg` (free text, ≤ 200 chars).

The codes are part of the contract (they will be alert labels). What the firmware sends today
(rtk-rover 0.44.3):

| `code` | sev | when |
| --- | --- | --- |
| `wifi_disconnected` | 1 | hotspot lost, trying to get back on |
| `ntrip_connect_failed` | 1 | could not reach the caster, or it never answered the request |
| `ntrip_rtcm_timeout` | 1 | no corrections for 10 s, dropping the caster connection |
| `ntrip_bad_response` | 2 | caster answered, but not with a correction stream (`msg` carries the reply) |
| `ntrip_request_overflow` | 2 | the request to the caster did not fit its buffer: a config mistake, not a field fault |
| `i2c_bus_rtk_failed` | 2 | the sensor bus would not start |
| `i2c_bno080_not_detected` | 2 | head-tracking IMU not answering |
| `i2c_gnss_not_detected` | 3 | GNSS receiver not answering: the assembly is useless without it |
| `reset_brownout` | 2 | the previous boot ended in a power brownout |
| `reset_panic` | 2 | the previous boot ended in a firmware crash |
| `reset_wdt` | 2 | the previous boot ended in a watchdog timeout |

The `reset_*` codes are sent once at startup and describe the *previous* shutdown; they wait in
the ring buffer until a phone connects, and are evicted like any other frame once the ring wraps
(~40 s of `gnss_fix`). Clean resets (power-on, software reset) emit nothing: a reboot is then
only visible as `dev_seq` / `t_dev_ms` jumping backwards. New codes may appear at any time (§4.4);
the backend stores ones it does not know about without any change.

**`app_event`**: app-created (`source` = `phone`): `name` (e.g. `app_launched`, `walk_started`,
`walk_stopped`, `upload_failed`), `data` (small JSON object).

### 4.4 Schema evolution rules

- `schema` integer bumps on breaking changes only. Additive fields are always allowed;
  consumers ignore unknown fields.
- New event types are additive — the backend stores unrecognized types in the generic
  `events` table automatically.

---

## 5. BLE transport (assembly → app)

- One additional GATT service ("telemetry"): TX characteristic (notify) + CTRL
  characteristic (write).
- Frames: `[u8 proto_version][u16 length][CBOR payload]`, CBOR map mirroring §4 fields
  with short integer keys (mapping table below; mirrored as `telemetry_keys.h` in
  `rtk-rover` and `TelemetryKeys.swift` in `rwa-player`).
- Firmware side: ring buffer (4 KB — 8 KB exceeded the ESP32 heap budget next to
  BLE + WiFi) drained by a dedicated low-priority FreeRTOS task.
  **Drop-oldest** on overflow; never block sensor or BLE tasks. Increment a dropped-frames
  counter reported in `heartbeat`. The ring holds roughly 40 s of `gnss_fix` at 1 Hz; events
  are produced into it regardless of the BLE link state, so after a reconnect the app receives
  the surviving recent history (no ack, no replay: a frame notified to a central that then
  drops the connection is lost).
- CTRL characteristic accepts runtime commands: set log verbosity, trigger a status dump.
  (Later: firmware OTA chunk transfer — keep the command space versioned.)

### 5.1 GATT UUIDs

Same vendor family as the existing tracker service (`713D0000-…`), new service:

| | UUID |
| --- | --- |
| Telemetry service | `713D0100-503E-4C75-BA94-3148F18D941E` |
| TX (notify) | `713D0101-503E-4C75-BA94-3148F18D941E` |
| CTRL (write) | `713D0102-503E-4C75-BA94-3148F18D941E` |

The telemetry service is **not advertised**: the 31-byte advertisement is already full with the
tracker service UUID. The app connects on the tracker service as before and discovers telemetry
afterwards. This is also how the app tells the two assembly kinds apart: the plain headtracker
(RWAHT) exposes only the tracker service with `713D0002`; the RTK headtracker additionally
exposes the raw position characteristic `713D0004` and the telemetry service.

### 5.2 Framing details

- `proto_version` = `1`. The app drops frames with an unknown proto version (count, log).
- `length` = byte length of the CBOR payload only, **little-endian** u16.
- The TX characteristic is a **byte stream**: one notification may carry several
  concatenated frames, and a frame may span notifications. The app reassembles using
  the length prefix. Frames being split is normal, not a rare edge case: the device fills
  every notification to the last byte and continues the frame in the next one. It is
  guaranteed right after connecting, when the MTU is still 23 and only 20 bytes fit per
  notification while a `gnss_fix` frame is around 90 bytes long.
- Text fields are UTF-8. On the BLE leg `error.msg` is capped at 120 bytes (the §4.3
  200-char limit applies to the JSON leg).

### 5.3 CBOR key table (v1)

`type` is a uint enum on the BLE leg; the app maps it back to the §4.3 string names
for JSON. The app drops unknown type ids (count, log).

| `type` value | event |
| --- | --- |
| 1 | `gnss_fix` |
| 2 | `heartbeat` |
| 3 | `ntrip_status` |
| 4 | `imu_status` |
| 5 | `error` |

Common keys (every event): `0` = `type` (uint), `1` = `seq` (uint), `2` = `t_dev_ms` (uint).
Key 1 is the firmware's per-boot frame counter; the app maps it to `dev_seq` on the JSON leg
(§4.2) and assigns the JSON `seq` itself. The app also stamps `source` = `rtk_headtracker` on
every decoded frame; the frame carries no identity.

Type-specific keys start at 10 (`type` disambiguates, so numbers repeat across types):

| event | key | field | CBOR type |
| --- | --- | --- | --- |
| `gnss_fix` | 10 | `lat` | double |
| | 11 | `lon` | double |
| | 12 | `height_m` | float |
| | 13 | `fix_type` | uint |
| | 14 | `carr_soln` | uint |
| | 15 | `h_acc_mm` | uint |
| | 16 | `v_acc_mm` | uint |
| | 17 | `num_sv` | uint |
| | 18 | `pdop` | float |
| | 19 | `corr_age_ms` | uint |
| `heartbeat` | 10 | `uptime_ms` | uint |
| | 11 | `free_heap` | uint |
| | 12 | `wifi_rssi` | int (negative dBm) |
| | 13 | `ntrip_connected` | bool |
| | 14 | `fw_version` | text (≤ 32 B) |
| | 15 | `dropped_frames` | uint |
| | 16 | `batt_mv` | uint |
| `ntrip_status` | 10 | `state` | uint: 0 = disconnected, 1 = connected, 2 = reconnecting |
| | 11 | `reconnects` | uint |
| | 12 | `bytes_rx` | uint |
| `imu_status` | 10 | `calib_status` | uint |
| | 11 | `report_rate_hz` | float |
| | 12 | `resets` | uint |
| `error` | 10 | `severity` | uint |
| | 11 | `code` | text (≤ 32 B) |
| | 12 | `msg` | text (≤ 120 B) |

Additive evolution: new fields get new keys (never reuse a retired number within a
type); new event types get the next free `type` value. Both sides ignore unknown keys.

### 5.4 CTRL commands (app → assembly)

Write `[u8 cmd][args…]` to the CTRL characteristic. Unknown commands are ignored
(forward compatibility; the command space is versioned by `proto_version`).

| cmd | args | effect |
| --- | --- | --- |
| `0x01` set_verbosity | u8 level | minimum `error.severity` the device emits (default 1 = everything) |
| `0x02` status_dump | - | the device sends a `heartbeat` at its next 100 ms tick |

`status_dump` sends a heartbeat and nothing else. `ntrip_status` and `imu_status` cannot be
asked for on demand — the app waits for the next state change, or the next 60 s IMU report.
If the Diagnostics tab ever needs a complete snapshot on demand, that is a firmware change,
not an app one. (The app does not write CTRL today.)

---

## 6. App responsibilities (rwa-player)

1. Decode BLE frames, map CBOR `seq` → `dev_seq`, stamp `time`, `source` = `rtk_headtracker`
   and the envelope context, append to SQLite (`pending_events`).
2. Emit its own events (`gnss_fix`/`heading` from the phone's sensors or the text protocol,
   app `heartbeat`, `app_event`) into the same store, each with its `source`.
3. Assign `seq` = the SQLite row id when building a batch.
4. Background uploader: every 15–30 s, POST oldest ≤ 500 events as one gzipped batch.
   Delete rows only on HTTP 2xx. Exponential backoff on failure. Duplicates are safe
   (backend dedupes on `(device_id, session_id, seq, time)`, §4.2).
5. Surface assembly health minimally in a debug screen (last fix quality, NTRIP state,
   connected assembly kind and id).

---

## 7. Backend (rwa-backend)

docker-compose stack on the existing EC2 instance, behind Traefik with Let's Encrypt:

| Service | Image | Role |
| --- | --- |--- |
| traefik | traefik:v3 | TLS, routing: `/v1/*` → ingest, `/grafana` → Grafana |
| db | timescale/timescaledb (pg16) | storage; not exposed publicly |
| ingest | local build (FastAPI) | `POST /v1/batch`: auth, validate, bulk insert |
| grafana | grafana/grafana | dashboards, alerting; provisioned datasource |

Storage model — two tables:

- `gnss_fix` (typed columns, Timescale hypertable) — the high-rate, heavily queried stream.
- `events` (generic, `payload jsonb`, hypertable) — everything else, including unknown types.

Known gap: the typed `gnss_fix` insert has a fixed column list, so `source` and `dev_seq` are
dropped for fixes until the columns are added (additive, §4.4); for the generic table they
land in `payload`. Duplicates are dropped silently by `ON CONFLICT DO NOTHING`; the response's
`accepted_*` counts are the submitted rows, not the inserted ones.

Volume estimate: 10 units × 1 Hz × ~5 h/day × 60 days ≈ 10–20 M rows. Comfortable.

Dashboards (Grafana, provisioned from repo):

1. **Dead-zone map**: Geomap over `gnss_fix` with `source = 'rtk_headtracker'`, color by
   `carr_soln`, second layer for `corr_age_ms > 5000` (separates GNSS dead zones from
   correction-delivery gaps).
2. **Live fleet**: last fix per unit on the map.
3. **Unit health**: heartbeat recency, fw version, error counts, RTK-fixed %, NTRIP reconnects.
4. **Alerts**: no heartbeat > 5 min, any severity ≥ 2 error, disk usage.

Operations: nightly `pg_dump` to S3 (host cron + `scripts/backup.sh`), disk-usage alert.

---

## 8. OTA groundwork (decisions made now, delivery later)

1. **Firmware**: ESP32 flash partitioned with the standard two-OTA-slot scheme from the
   first deployed build. `fw_version` (semver + git hash) reported in every heartbeat.
   Future delivery path: backend → app → BLE chunked transfer (CTRL characteristic),
   protocol already versioned.
2. **Content (soundwalks)**: versioned asset bundles described by a manifest, fetched
   from object storage (S3). Never baked into the app binary. The ingest service's host
   can later serve manifests; no backend change required now beyond keeping the door open.

---

## 9. Sequencing

1. Event schema v1 (§4) — this document.
2. Backend stack up; build dashboards against `scripts/fake_data.py` synthetic traffic.
3. rwa-player: SQLite buffer + uploader, tested with canned events.
4. rtk-rover: telemetry GATT service + trace task; watch real events land in Grafana.
5. OTA partition table ships with the first trace-task firmware build.

## 10. Decision log

| Date | Decision | Rationale |
| --- | --- | --- |
| 2026-06 | Gateway pattern (all telemetry via iPhone) | existing BLE link, enrichment, buffering, ESP32 resource budget |
| 2026-06 | Two-table storage (typed `gnss_fix` + generic `events`) | heatmap query performance vs. schema flexibility |
| 2026-06 | TimescaleDB over plain Postgres | free in compose; time-bucketing, compression |
| 2026-06 | Dedup key `(device_id, session_id, seq)` w/ split seq ranges | idempotent uploads with retries — superseded 2026-08 |
| 2026-06 | Static bearer token auth for ingest | 10 trusted devices we own; revisit if fleet grows |
| 2026-07 | rename rwa-client to rwa-player | More descriptive app name, clear distinction between creator and backend |
| 2026-07 | BLE key table v1 pinned (§5.1–5.3): UUIDs, LE u16 length, byte-stream TX, int `type` enum | unblocks firmware + app implementation in parallel |
| 2026-08 | Glossary §1.1: assembly / board / phone / unit; `device_id` = unit label `rwa-hs-N`; `assemblyId` is an optional BLE-name override on the phone | four vocabularies had grown for one object; the provisioning file set two differently-named keys to the same string |
| 2026-08 | Gateway-owned `seq` (Player SQLite row id); firmware counter demoted to `dev_seq` | the firmware counter restarts at 1 on every boot with no boot number, so a mid-session headset reboot collided under the old key; the gateway is the only party that persists and retries, and already owns a unique monotonic id. No backend change; `session_id` stays in the index as the reinstall guard |
| 2026-08 | `source` enum on every event (`rtk_headtracker` / `headtracker` / `phone` / `creator`); one `gnss_fix` stream per source | the old values mixed sensor and emitter and were absent on firmware events; the RTK assembly produced two fixes per second |
| 2026-08 | §4–5 re-checked against shipped firmware 0.44.3 | key tables matched exactly; the prose had drifted (status_dump scope, imu_status cadence, frame splitting, error codes, advertising) |
