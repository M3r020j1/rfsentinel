# RFSentinel — Full project documentation

> **Language:** English · [Français](docs/fr/DOCUMENTATION.md)

> Reference document: vision, architecture, trade-offs, API, usage, real project
> state and constants. Every value is taken from the code of version 1.0.0.

---

## 1. Product vision

**RFSentinel** is a standalone sensor for **passive radio-environment
monitoring** (Wi-Fi 2.4 GHz + Bluetooth Low Energy), built around a **LilyGO
T-Dongle S3 (ESP32-S3)** dongle.

It:

- **detects nearby devices passively** — published Wi-Fi networks and
  advertising Bluetooth LE peripherals — without joining anything;
- **records observations in its own flash** (LittleFS), with circular rotation;
- ships an **embedded web interface**, bilingual FR/EN with light and dark
  themes, to browse, sort, filter, map, export and purge;
- **looks devices up in the public WiGLE tables** and supports **manual upload**
  of the journal in WiGLE format;
- **raises MQTT alerts** when a new device appears;
- runs **without any required network**: a built-in rescue access point takes
  over, and scanning never stops;
- shows on its **TFT screen** the address to open and the state of the scans.

Use cases: passive wardriving, network mapping, proximity audit, radio
environment watch, BLE inventory, hunting a transmitter in a room.

**Legal and ethical frame**: RFSentinel only observes **freely broadcast**
frames — Wi-Fi beacons and BLE advertisements. It connects to no network,
decrypts nothing, intercepts no communication and identifies nobody. The user
remains responsible for complying with local regulations on radio capture and on
the use made of the data.

---

## 2. Hardware target

| Item | Value |
|---|---|
| Board | LilyGO **T-Dongle S3** (ESP32-S3) |
| Flash | **16 MB internal** — no SD card, the SD/FatFs dependency was removed |
| PSRAM | **None** — everything is sized for internal RAM |
| Screen | TFT **ST7735**, 128×160 panel driven as **160×80 landscape** |
| Wi-Fi | 2.4 GHz only, **100 % passive** scan |
| BLE | NimBLE, **central + observer**, advertisements only |
| GPS | **None** — latitude/longitude entered by hand, `0` = unknown |
| USB | USB-C, USB-Serial-JTAG device |

### Screen wiring

Read from `display.c`, confirmed on the board:

| Role | GPIO | Constant |
|---|---|---|
| SPI clock | **5** | `RFS_PIN_LCD_CLK` |
| SPI MOSI | **3** | `RFS_PIN_LCD_MOSI` |
| Data/Command | **2** | `RFS_PIN_LCD_DC` |
| Reset | **1** | `RFS_PIN_LCD_RST` |
| Chip select | **4** | `RFS_PIN_LCD_CS` |
| Backlight | **38**, **active low** | `RFS_PIN_LCD_BL` |

The bus is `SPI2_HOST` at 20 MHz, SPI mode 0, a queue of 10 transfers, 16
bits per pixel in BGR order. The T-Dongle S3 landscape 160×80 format comes from
`swap_xy(true)` + `mirror(false, true)` + `set_gap(1, 26)`,
matching the official LilyGO example. The backlight turns on with the GPIO low
and off with the GPIO high.

---

## 3. Architecture

### 3.1 Layout

**ESP-IDF v5.3.2** project (no Arduino), standard structure:

```
rfsentinel/
├── CMakeLists.txt            # project (min + idf_project_default)
├── README.md                 # repository landing page (English)
├── DOCUMENTATION.md          # this file (English)
├── LICENSE                   # CC BY-NC-SA 4.0
├── partitions.csv            # nvs 24K, phy_init 4K, factory 3M, storage 13M
├── sdkconfig.defaults        # pinned build config (16 MB, no PSRAM, NimBLE…)
├── bin/                      # release images + SHA256SUMS
├── docs/fr/                  # French README and documentation
├── logo/                     # SVG (WebUI) + OLED bitmap
│   ├── rfsentinel-logo.svg
│   ├── rfsentinel-icon.svg
│   ├── rfsentinel-oled-icon.svg
│   └── rfsentinel-oled-wordmark.svg
├── screen/                   # screenshots used on the landing page
├── tools/generate_lcd_logo.py  # SVG -> 1 bpp bitmap for the screen
├── tests/                    # host-side tests (Python, JS, C)
└── main/
    ├── CMakeLists.txt        # SRCS + EMBED_FILES (ui/index.html + 2 logos)
    ├── idf_component.yml     # pinned dependencies (littlefs ==1.23.1)
    ├── version.h             # RFS_VERSION, single source of the version
    ├── main.c                # bootstrap + housekeeping loop
    ├── config.c/h            # NVS configuration, secrets, validation
    ├── auth.c/h              # WebUI session (cookie) + anti brute-force
    ├── net.c/h               # Wi-Fi STA + rescue AP + NTP + timestamp quality
    ├── scanner.c/h           # passive Wi-Fi and BLE scans, calibration, hunting
    ├── store.c/h             # LittleFS journal, WiGLE CSV, rotation, blacklist, export
    ├── mqtt.c/h              # MQTT alerts (esp-mqtt)
    ├── presence.c/h          # watched MACs, presence alerts
    ├── wigle.c/h             # WiGLE upload and lookup (API v2, HTTPS)
    ├── web.c                 # embedded HTTP server (esp_http_server + cJSON)
    ├── display.c/h           # ST7735 screen (boot, status, auto-off, language)
    ├── oled_font.h           # 8×16 glyphs (uint16_t, one word per column)
    ├── oled_logo.h           # 64×64 1 bpp logo (bitmap)
    ├── ui/index.html         # WebUI SPA, embedded through EMBED_FILES
    └── lcd/esp_lcd_st7735.c/h  # local ST7735 driver
```

### 3.2 Data flow

```
[Wi-Fi passive]─┐
               ├─► scanner ─► store (CSV /rfs/observations) ─► wigle (manual upload)
[BLE adv]──────┘      │
                      ├─► mqtt (new-device alerts)
                      ├─► presence (watched devices)
                      └─► web (JSON API + UI)  ◄─── browser
```

- The scanner runs in **its own FreeRTOS task** and is never blocked by the rest;
  the main task stack is raised to 6144 bytes.
- `store` is the **only** user of the filesystem; `web` only serialises
  JSON and reads through bounded streams.
- `net` supplies UTC time **best effort**: NTP when the link allows it,
  otherwise a stored reference plus uptime.
- `config` is loaded **once** into RAM (`app_config_t`) and written to
  NVS sparingly.
- The main loop in `main.c` only calls `net_tick()` and
  `display_tick()` every second, with a screen refresh every 5 s.

### 3.3 Design principles

1. **Bounded dynamic allocation**: static buffers on the critical path, the heap
   reserved for one-off requests, and frees on every exit.
2. **Never blocking** in the main loop: periodic scans, non-blocking network
   tick, upload and purge run in the background with an intermediate status.
3. **Resilience to outages**: LittleFS, journal in files, gentle rotation.
   Nothing is destroyed on restart, the configuration survives in NVS.
   `format_if_mount_failed=false`: a failed mount on its own never authorises a
   format. The `storage` partition is read through in full and LittleFS is only
   initialised when every byte reads `0xFF`, that is a card that has never been
   used. A read error, a programmed byte, an encrypted or missing partition means
   an error is returned and nothing gets erased.
4. **Limited flash writes**: NVS reserved for configuration, clock reference
   written at most once an hour, journal in 32 KB segments.
5. **Minimal dependencies**: ESP-IDF + `joltwallet/littlefs` (managed
   component, pinned version) + cJSON + NimBLE + esp_http_server +
   esp_http_client + esp-mqtt.
6. **Secrets never exposed**: the WiGLE token, passwords and the Wi-Fi key never
   leave the device through API responses, logs or exports.
7. **A feature is not operational because it compiles**: it is operational when
   it has been observed on the board.

---


## 4. Technical choices and rationale

| # | Choice | Alternative rejected | Why |
|---|---|---|---|
| 1 | **ESP-IDF v5.3.2** | Arduino / PlatformIO | fine control of httpd, NimBLE, NVS and esp_lcd; reproducible build |
| 2 | **LittleFS** (`joltwallet/littlefs` ==1.23.1) | SPIFFS | wear levelling, folders, clean deletes; pinned version = reproducible builds |
| 3 | **Internal flash, no SD** | SD card with FatFs | pulling a card no longer destroys the journal; no connector, no jamming |
| 4 | **Mount point `/rfs`** | `/sd` | no SD card any more; one explicit path |
| 5 | **Rotation by files and segments** | full wipe | you never lose everything; the oldest files leave one at a time |
| 6 | **32 KB segments inside today's file** | one unbounded file per day | bounds how much history a rotation can take with it |
| 7 | **100 % passive Wi-Fi scan** (`WIFI_SCAN_TYPE_PASSIVE`) | active scan | stealth, legality, no association, no emitted traffic |
| 8 | **BLE through advertisements only** | GATT connection | observe without interacting; `filter_duplicates=1` |
| 9 | **NimBLE central + observer** | observer only | hunting a nearby transmitter needs the central role; peripheral and broadcaster stay off |
| 10 | **CSV `WigleWifi-1.6`** | proprietary format | the journal reloads into WiGLE as is, with no conversion |
| 11 | **Manual WiGLE upload** | automatic upload | user control, quota, ethics |
| 12 | **WiGLE lookup on the sensor** | direct call from the browser | the token never leaves the device |
| 13 | **Hand-entered position + geocoding** | onboard GPS | the dongle has no GPS; address lookup avoids typing coordinates |
| 14 | **Home-made `<canvas>` map + OSM tiles** | Leaflet | no external library to embed, falls back to a grid without Internet |
| 15 | **Two separate RSSI thresholds** (`min_rssi`, `mqtt_min_rssi`) | single threshold | a distant device can be recorded without triggering an alert |
| 16 | **STA + rescue AP** | STA only | usable off-network; the AP starts if STA fails, **without interrupting scans** |
| 17 | **NTP + stored reference** | raw timestamp | usable timestamps even offline, with an honest `ESTIMATED` quality |
| 18 | **Time set from the browser** | no offline fallback | in AP-only mode no time source is reachable; quality stays `ESTIMATED`, without pretending NTP |
| 19 | **NVS for configuration** | config file | atomicity, robustness, no dependency on the filesystem |
| 20 | **Embedded UI** (`EMBED_FILES`) | external CDN | works in AP mode without Internet |
| 21 | **Bilingual FR/EN + light and dark themes** | single language, single theme | the device is read outdoors as well as indoors |
| 22 | **Session cookie + anti brute-force** | basic auth alone | the device must not be controllable by anyone on the network |
| 23 | **Forced password change when still `admin/admin`** | default values accepted | basic hygiene |
| 24 | **Pre-converted bitmap resources** | SVG parsing on the MCU | an ESP32 does not parse SVG; 8×16 font and 64×64 logo in 1 bpp in flash |
| 25 | **WiGLE over HTTPS (full CA bundle)** | HTTP | the token never travels in clear text |
| 26 | **De-duplication + blacklist** | raw recording | avoid spamming repeats and permanently ignore known devices |
| 27 | **Reduced WiGLE buffers, capped rows** | copy the whole response | without PSRAM the heap is the scarce resource: 6 KB of body, 10 rows maximum |

---

## 5. Data model

### 5.1 Observation (`s3_obs_t`, `store.h`)

```c
typedef struct {
    char mac[18];     // "AA:BB:CC:DD:EE:FF" (Wi-Fi BSSID or BLE BDADDR)
    char ssid[33];    // Wi-Fi SSID or BLE name (may be empty)
    char auth[16];    // Wi-Fi: "Open", "WPA2"… / BLE: address type
    char ts[20];      // "YYYY-MM-DD HH:MM:SS" in UTC
    int  channel;     // Wi-Fi channel (0 for BLE)
    int  freq;        // kHz (2412..2472 for Wi-Fi, 0 for BLE)
    int  rssi;        // dBm (negative)
    int  mfgr;        // reserved (0)
    bool is_ble;      // false = Wi-Fi, true = BLE
    uint8_t time_quality;  // 0 unknown, 1 estimated, 2 NTP synchronised
} s3_obs_t;
```

### 5.2 Writing to the journal

One line per observation, in the field order of the WiGLE format: MAC, SSID
(escaped), lock, UTC time, channel, radio (24 GHz), signal, configured latitude
and longitude, manufacturer, then `WIFI` or `BLE` as the last column. Fields the
sensor cannot know stay empty.

For BLE, the `auth` field carries the **address type**: `BLE-PUBLIC`, `BLE-STATIC`,
`BLE-RPA`, `BLE-NRPA` or `BLE-UNKNOWN`. It is written in the `RCOIs` column
of the journal file, never in the `Type` column, which keeps `BLE` and still reads as a
standard WiGLE CSV. That internal marker is stripped on export (§5.3), so the
downloaded file only shows WiGLE fields.

The type comes from `main/ble_address.h`, which sorts every address it meets into
`PUBLIC`, `STATIC`, `RPA`, `NRPA` or `UNKNOWN`. Addresses arrive least
significant byte first; NimBLE type 0 or 2, a public address or a resolved
public identity, gives `PUBLIC`. For type 1, a random address, the two most
significant bits of `addr[5]` decide: `0xc0` static, `0x40` RPA, `0x00` NRPA. A type 3
whose bits do not announce a static address, or an unrecognised type, falls
back to `UNKNOWN`.

### 5.3 Journal (`store.c`)

- One file **per day**: `/rfs/observations/obs_YYYYMMDD.csv`, with the WiGLE
  header on the first line.
- Today's file is split into **32 KB segments** (`RFS_SEGMENT_MAX_BYTES`): a
  rotation never takes more than one segment, so at most 32 KB of history.
- A new segment also opens when the **timestamp quality changes**, so that reliable
  times and estimated times never mix inside one segment.
- **Circular rotation**: past `RFS_USAGE_LIMIT_PCT` (90 % of the partition),
  the oldest files leave **one at a time**; a cap of `RFS_OBS_MAX_FILES` (32)
  files also bounds growth. Never a full format, and **scanning never stops**
  because the journal is full.
- **De-duplication**: a `s_seen` table of 128 MACs in RAM; a device becomes
  "new" again after `rediscover_min` minutes of absence.
- **Blacklist**: MACs ignored at write time, added and removed from the UI,
  persisted in NVS (`bl_` plus an index `bl_idx`), cache of 64 entries.
- **Sorting and filtering on demand**: keys `date`, `mac`, `ssid`, `rssi`, `tsq`,
  `type`, ascending or descending, with a time-range filter.
- **Export**: sequential read of every segment into a single CSV stream in WiGLE
  format, without ever loading the journal into memory.
  The BLE address filter (§5.2) applies at read time: lines whose address type is
  excluded by the setting never leave the file, and the internal marker written in
  the `RCOIs` column is stripped, so the export only shows WiGLE fields. A BLE line
  recorded before the marker existed carries an unknown type: it passes in « All »
  mode and disappears in the other two.
- **Export length**: the HTTP answer is a `multipart` whose length has to be
  announced before the first byte. `store_export_open()` therefore walks the
  filtered journal once to count the bytes, then puts the cursor back at zero.
  The two passes read the same frozen copy of the journal; if a rotation happens
  between them, the read stops on `ESP_ERR_INVALID_STATE` rather than serving a
  file whose length would not match the header.

- **First mount on a fresh card**: `store_init()` mounts `storage`, and on failure
  calls `store_partition_is_blank()`, which walks the whole partition in 1 KB
  blocks — yielding every 64 KB so the main loop is not starved. Only an all-`0xFF`
  partition, the signature of a card that has never been written, is formatted and
  mounted. Anything else keeps the journal exactly as it is and reports the storage
  as unavailable, which the interface shows instead of an empty list.

### 5.4 Configuration (`config.h`, in NVS)

`app_config_t` groups: Wi-Fi (SSID and key), WebUI (user and password), WiGLE
(API name and token), rescue AP (SSID and password), latitude/longitude,
`min_rssi`, `mqtt_min_rssi`, `rediscover_min`, `scan_wifi`,
`scan_ble`, `ble_address_filter`, `scan_autostart`, `ui_lang`, MQTT (enable,
broker, port,
user, password, topic, client id, TLS), NTP (server and timezone) and
`device_id` and the network `hostname`.

Rules:

- `hostname` is stored in NVS and defaults to `rfsentinel`. It contains 1–32
  lowercase ASCII letters, digits or hyphens, beginning and ending with a letter
  or digit. After a reboot it is used for both the DHCP hostname and the mDNS
  hostname; `.local` is added only for mDNS access. Invalid stored values fall
  back to the default.
- `config_needs_password_change()` returns true while the credentials are still
  `admin`/`admin`; the interface then forces the change.
- `config_has_password()`, `config_has_token()` and
  `config_has_ap_password()` return booleans only, **never the value**.
- The WiGLE token is never read back by the interface after saving: six sensitive
  fields show eight dots, and an eye icon lets you replace them, never recover them.
- `config_save_time_ref()` limits the clock-reference write to once an hour to
  spare the flash.
- `ble_address_filter` (NVS key `ble_filter`, unsigned byte, default « stable »)
  decides which BLE addresses the journal keeps and which ones the export lets
  through: 0 all, 1 public and static random, 2 public only. A value out of range
  read back from NVS falls back to the default at load time, and
  `config_set_ble_address_filter()` refuses anything above 2.

---


## 6. Web API (`web.c`)

`esp_http_server`, JSON responses through cJSON. `httpd_start` returns an
`esp_err_t` that **is checked**, and `max_uri_handlers` is sized to 29 for the
28 handlers spread over 27 routes.

Every route requires a session except `/api/login`. The session is a `sid` cookie
set `HttpOnly`, `SameSite=Strict`, `Path=/`, `Max-Age=3600`. Three failures in a
row — through the login route or through the HTTP Basic fallback — trigger a 30-second
lockout.

| Method | Route | Purpose |
|---|---|---|
| GET | `/` | web interface (embedded HTML) |
| GET | `/logo/rfsentinel-logo.svg` | logo (embedded) |
| GET | `/logo/rfsentinel-icon.svg` | icon (embedded) |
| POST | `/api/login` | login `{user, pass}` → session cookie |
| POST | `/api/logout` | logout |
| GET | `/api/config` | read configuration, secrets masked |
| POST | `/api/config` | partial-merge update, validated internally |
| GET | `/api/scan/status` | mode, IP, time and quality, version, counters, journal usage, MQTT state, scan diagnostics |
| POST | `/api/scan/start` | start scanning |
| POST | `/api/scan/stop` | stop scanning |
| GET | `/api/observations` | paginated list — `page`, `type`, `sort`, `dir`, `cursor`, `from`, `to` |
| GET | `/api/history` | 10 most recent local appearances of a MAC (`mac`) |
| GET | `/api/presence` | watched devices, MQTT topic and absence duration |
| POST | `/api/presence` | add or remove a watched device (`action`, `mac`, `label`) |
| GET | `/api/blacklist` | list of exclusions |
| POST | `/api/blacklist/add` | add an exclusion `{mac, label}` |
| POST | `/api/blacklist/remove` | remove an exclusion `{mac}` |
| POST | `/api/wigle/upload` | trigger the manual journal upload to WiGLE |
| GET | `/api/wigle/upload/status` | upload progress and result |
| GET | `/api/wigle` | what WiGLE knows about a MAC (`mac`, `type`) |
| POST | `/api/purge` | purge by date range `{from, to}` |
| GET | `/api/purge/status` | purge progress |
| GET | `/api/calibration` | state of the running measurement |
| POST | `/api/calibration` | `start`, `stop`, `apply`, `find_start`, `find_next` |
| GET | `/api/calibration/find` | BLE candidates found during a hunt |
| GET | `/api/export` | CSV export of the journal (`from`, `to`) as an attachment |
| GET | `/api/time` | current time and timestamp quality |
| POST | `/api/time` | set the clock from the browser's time |

Useful details:

- `/api/observations` returns 14 rows per page; a `type` other than `wifi` or
  `ble` is rejected with `bad_type`.
- `/api/config` accepts `ble_address_filter` (0, 1 or 2) and returns it on read.
  A value that is not a whole number, is negative or is above 2 answers `400` with
  `ble_filter_invalid`; a failure to store it in NVS answers `500` with
  `ble_filter_save_failed`. Either way the previous setting stays in place.
- `/api/scan/status` adds five BLE diagnostic counters — `ble_public`,
  `ble_static`, `ble_rpa`, `ble_nrpa`, `ble_unknown` — which count the addresses
  met since start-up, **before** filtering: they therefore show the volume of
  temporary addresses the setting keeps out of the journal.
- `/api/export` requires `from` and `to` as epoch seconds, refuses invalid or
  inverted bounds, and answers `text/csv` with `Content-Disposition: attachment`
  and `Cache-Control: no-store`.
- `/api/scan/status` also exposes Wi-Fi scan attempts and failures, the channel
  being probed and the age of the last cycle: that is what made it possible to
  diagnose a silent scan from the interface.
- POST routes are declared as `HTTP_POST`. Bodies are read with a 4096-byte cap.
- `/api/presence` is the only route declared twice, once for GET and once for POST,
  which gives 28 handlers for 27 distinct paths.
- Upload works in the background across every journal file and aggregates the
  results; the interface follows progress through `/api/wigle/upload/status`.

---

## 7. WebUI (`ui/index.html`)

Single-file SPA embedded through `EMBED_FILES`, with no external JavaScript
dependency. Bilingual FR/EN with the choice stored locally, **light and dark
themes** switched by a button and remembered in `localStorage` under the key
`rfs-theme`; dark is the default.

Views and cards:

- **Login** — credentials, forced password change on first launch, explicit message
  during the anti brute-force lockout.
- **Status** — network mode, IP, version, local time with UTC in a tooltip, last
  synchronisation, Wi-Fi and BLE counters really read from the journal, storage
  usage, MQTT state, scan diagnostics, time-set button, and a line giving the
  breakdown of BLE addresses since start-up — public, static, RPA, NRPA and
  unknown.
- **Observations** — paginated list of 14 rows, column sorting, Wi-Fi/BLE filter,
  date-range filter in local time, clickable rows.
- **Device history** — presences known to WiGLE for the clicked MAC, in
  chronological order, with a point on the map to the right.
- **Map** — hand-drawn `<canvas>`: fallback grid, then real OpenStreetMap tiles
  assembled (`https://tile.openstreetmap.org/{z}/{x}/{y}.png`, 256 px) when the
  workstation has Internet. Zoom computed from the real spread of the point cloud
  and clamped between 6 and 17; projection relative to the cloud bounds, longitude
  correction by latitude, accuracy circle around the selected point, rescaling to
  the displayed size and screen density.
- **Configuration** — Wi-Fi, rescue AP, position, thresholds, scan options,
  language, WiGLE, MQTT, NTP, timezone and device identifier. A « BLE addresses »
  selector picks the filter applied to the journal and to the exports — All,
  Stable (default), Public only — with a help line reminding that old BLE lines
  without a type stay excluded from filtered exports. The six sensitive fields
  are masked, with an eye icon to replace them.
- **Geocoding** — address lookup against the French national address base
  (`https://data.geopf.fr/geocodage/search`) to fill latitude and longitude.
- **Export** — download of the whole journal or of a range, as WiGLE CSV ready to
  send back to wigle.net.
- **Purge** — deletion by date range with explicit confirmation and progress
  tracking; nothing is erased blindly.
- **BLE calibration** — signal measurement of a device at three positions to derive
  a threshold, then application of the resulting threshold; hunt for a nearby
  transmitter by comparing three readings, with a candidate list.
- **Excluded devices** — blacklist with labels.
- **Presence alerts** — watched MAC addresses with label, adjustable MQTT topic and
  absence duration.
- **WiGLE upload** — button, confirmation, intermediate status, result; logout is
  reachable in the banner.

The map loads **no** mapping library at all: every rendering is local, which is
what makes it work in AP mode without Internet, on the fallback grid.

---


## 8. MQTT (`mqtt.c`)

- `esp-mqtt`; broker, port, user, password, topic, client id and TLS configurable.
  Started at boot, not only after saving the configuration.
- `mqtt_on_obs(obs)` is called by the scanner **after recording**. It filters on
  `mqtt_min_rssi`, applies de-duplication with `rediscover_min`, then
  publishes at **QoS 1, no retain**.
- Checking and publishing happen inside the same critical section, because BLE
  callbacks and the Wi-Fi task arrive in parallel.
- Payload, capped at 384 bytes:

```json
{"device_id":"…","mac":"…","type":"WIFI","ssid":"…","rssi":-72,
 "ts":"2026-11-11 10:50:10","tsq":"NTP_SYNCED","lat":48.85660,"lon":2.35222}
```

`tsq` gives the timestamp quality and takes three values: `NTP_SYNCED`,
`ESTIMATED` — the clock restarts from an earlier reference, without revalidation —
and `UNKNOWN` when the device has no idea of the time, in which case `ts` is
`"0"`. A consumer can therefore discard badly timestamped messages.

---

### 8.1 Watched devices (`presence.c`)

`presence.c` keeps a list of 32 MAC addresses and their labels, persisted in its own
NVS namespace (`rfs_presence`). Watching is independent of the recording thresholds
and of the exclusion list:

- on the Wi-Fi side, `presence_on_obs()` is called **before** the exclusion test
  and before any signal filtering;
- on the BLE side, `presence_matches()` is consulted before the filters,
  otherwise a watched device would be dropped for a weak signal; the observation is
  then forwarded even when it is not journalled;
- the first reading triggers an alert whatever the signal level; a return alert is
  only sent after an absence of at least `absence_s` (120 s by default, 30 s
  minimum), with a new attempt at most every 5 s;
- `mqtt_presence_publish()` publishes at QoS 1 on a dedicated topic, never the
  alerts topic: the configuration refuses both names being identical.

Payload, capped at 512 bytes:

```json
{"event":"presence","device_id":"…","mac":"…","type":"BLE","label":"…",
 "ssid":"…","rssi":-71,"ts":"2026-11-11 10:50:10","tsq":"NTP_SYNCED"}
```

`label` carries the label entered in the interface, so a consumer can tell several
watched devices apart without knowing their MAC addresses.

---

### 8.2 Connecting with Home Assistant

RFSentinel behaves like an ordinary MQTT client: it **publishes**, it listens to
nothing and exposes no entry point. A home automation system connected to the same
broker — Home Assistant through its MQTT integration, Node-RED, openHAB or a script —
**listens** to the topic it cares about and decides what to do with it. Nothing on
the consumer side is specific to RFSentinel.

| Default topic | Trigger | Useful fields |
|---|---|---|
| `/rfsentinel/alert` | new device journalled | `type`, `mac`, `ssid`, `rssi`, `ts`, `tsq`, `lat`, `lon` |
| `/rfsentinel/presence` | watched device appears or returns | `event`, `label`, `type`, `mac`, `ssid`, `rssi`, `ts`, `tsq` |

Both topics can be renamed on the configuration page and cannot carry the same name
(§8.1). Both publications are QoS 1 with no retain: a consumer that was switched off
gets no catch-up when it reconnects.

In Home Assistant, the automation is built on the **"When an MQTT message has been
received"** trigger, with the topic to listen to. The action is then free: persistent
notification, push to a phone, switching on a light, spoken message. The JSON fields
are read back in a template through `trigger.payload_json`:

```yaml
{{ trigger.payload_json.type }} detected — SSID: {{ trigger.payload_json.ssid or 'unknown' }} — MAC: {{ trigger.payload_json.mac }} — RSSI: {{ trigger.payload_json.rssi }} dBm
```

Two fields deserve an explicit condition in the automation.

`rssi` behaves differently depending on the topic. On `/rfsentinel/alert` it is
always a number, but a BLE device appears there with 127 when the signal was not
measured: a threshold such as "RSSI above -80" would therefore exclude those devices
by accident. On `/rfsentinel/presence` the same case is written `null`, and a
template that prints the field without a filter then produces "dBm" with no number.

`tsq` is `NTP_SYNCED`, `ESTIMATED` or `UNKNOWN`, and lets you discard messages sent
before the clock is synchronised: in the last case `ts` is `"0"`, which skews any
time-based automation on first boot.

The presence topic only speaks when a watched device appears or reappears: after an
absence of at least `absence_s`, its return triggers a new publication, and nothing
is published during the absence (§8.1). There is no departure message. An automation
that must react to a disappearance — switching a light off when the phone leaves the
flat — cannot expect that from RFSentinel: it switches on when the message arrives
and turns itself off after a `for:` duration longer than the radio rediscovery
period.

---

## 9. WiGLE (`wigle.c`)

Five entry points of API v2 are used, all over **HTTPS** with the full CA bundle and
**Basic** authentication (API name + token):

| Purpose | URL |
|---|---|
| Upload | `https://api.wigle.net/api/v2/file/upload` |
| Wi-Fi search | `https://api.wigle.net/api/v2/network/search` |
| Bluetooth search | `https://api.wigle.net/api/v2/bluetooth/search` |
| Wi-Fi detail | `https://api.wigle.net/api/v2/network/detail` |
| Bluetooth detail | `https://api.wigle.net/api/v2/bluetooth/detail` |

Points verified live against the API, after repeated failures:

- the address parameter is called **`netid`** in both tables; `mac`, `bdaddr` and
  `bssid` do not exist and cause 400 errors;
- the number of rows is requested with **`resultsPerPage`**;
- the position of a simple search is **`trilat` / `trilong`**, not `lat` / `lon` —
  a search that succeeded still left the map empty;
- dated positions come from the **`detail`** endpoints: `network/detail` returns
  `locationData[]` (`latitude`, `longitude`, `time`, `signal`, `accuracy`) and
  `bluetooth/detail` returns `locationClusters[].locations[]` with the same fields;
  without a detail point, the trilateration position from the header is reused with
  the covered period;
- a `401` is translated into `RFS_ERR_WIGLE_AUTH` so the interface talks about
  rejected credentials rather than an unknown device.

Memory constraints, imposed by the absence of PSRAM: 6 KB body buffer, 32 rows
maximum, the row array allocated once for the whole search instead of one block per
request, and frees on every early exit. The free heap is logged before and after a
search so it can be diagnosed on the board.

Those precautions are not enough while the screen is on: the panel framebuffer takes
the largest internal RAM block and blocks the TLS session from opening. The largest
available block only appears once the automatic switch-off happens, one minute after
power-on (§11.5). A WiGLE search started before that moment can fail for lack of
memory: the search stops on `ESP_ERR_NO_MEM` and answers `error:nomem`, which is
distinguishable from an address WiGLE does not know.

Upload is a POST multipart with a dedicated boundary, triggered **only** by a user
click. The API name and the token never leave the device.

---


## 10. Network and clock (`net.c`)

### 10.1 Two modes, one goal: never lose data

The device first tries to join the stored Wi-Fi network. That attempt is bounded:
`STA_CONNECT_TIMEOUT_MS` (8 s) per attempt, `STA_MAX_RETRY` (4) attempts in total.
Beyond that it switches to the access point `RFSentinel-Setup` (password
`012345678`, address `192.168.4.1`) and the interface stays reachable from a phone
or a computer.

The important point is that the switch does not interrupt collection: scanning
continues and the journal is written just as well in access-point mode as in client
mode. A dongle placed somewhere with no available network therefore fills its
journal, which can be read or uploaded later.

### 10.2 Non-blocking loop

`net_tick()` is called regularly by the main loop. It advances the network state
in small steps — one connection attempt, one timeout check, one NTP request — without
ever blocking. A slow Wi-Fi association or an unreachable NTP server therefore
suspends neither the screen, nor the interface, nor the scans.

### 10.3 The clock and its quality

A timestamp without a time reference is worthless for mapping. The device therefore
knows three states, recorded with every observation:

| Quality | Meaning | Source |
|---|---|---|
| `TS_UNKNOWN` | time not set | no usable source |
| `TS_ESTIMATED` | time reconstructed | `FALLBACK_EPOCH` (2025-01-01 UTC) + uptime |
| `TS_NTP_SYNCED` | time verified | NTP server |

The third state is never simulated. If the time is set from the browser — route
`POST /api/time`, which sends local time and the offset — the quality becomes
`TS_ESTIMATED` and not `TS_NTP_SYNCED`: the information comes from the user's
workstation, not from a recognised time source. This distinction reappears in the
export, where the `comment` field flags observations with estimated time.

A manual setting is kept in non-volatile storage every
`TIME_REF_SAVE_INTERVAL_S` (3,600 s), which makes it possible to recover a coherent
time after a restart without network. The default NTP server is `pool.ntp.org` and
the timezone `CET-1CEST,M3.5.0,M10.5.0/3`, both editable. A resynchronisation is
attempted every `NTP_RESYNC_INTERVAL_S` (6 h).

### 10.4 Displaying the time

The set time is stored in UTC. Interface displays — last synchronisation, observation
times — go through `net_time_local` and `net_last_sync_local`, which apply the
configured timezone. A conversion done with `gmtime_r` displayed a time one hour off
from the real local time; the journal, the CSV export and the MQTT messages stay in
UTC, which is the expected behaviour for data meant to be processed elsewhere.

---

## 11. Screen (`display.c`)

### 11.1 Hardware constraint

The screen is 160 × 80 pixels and shows only one effective colour, but the ST7735
controller expects RGB565 pixels: the framebuffer is therefore
`160 × 80 × 2 = 25,600 bytes`. It is allocated with `heap_caps_malloc()` and the
flags `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`, because the SPI transfer needs
DMA-accessible memory and the board has no PSRAM. It is the largest internal RAM
block the device consumes, and it is released as soon as the screen switches off —
see §11.5.

### 11.2 Software rotation

The board's controller is configured with `swap_xy(true)`, `mirror(false, true)` and
an offset of 26 pixels in X and 1 in Y, which presents the image turned by 180°
relative to the reference used by the drawing code. `rotate_frame()` flips the
framebuffer before sending. Practical consequence: the coordinates used by the
drawing code are the coordinates visible on screen.

This rotation was added after hardware feedback — the image was upside down — and the
fix was confirmed on the board.

### 11.3 Home screen

The logo, 64 × 64, is pasted at the top of the screen (`y0 = 0`). The bottom carries
the signature `By M3r0W1g`, centred under the logo. Both fit in the available
height: 64 pixels of logo plus a 16-pixel line of text = 80 pixels, with no overlap
and no cropping.

### 11.4 Status screen

After the home screen, the display shows continuously:

- `RFSentinel v1.0.0`;
- the IP address obtained, or `Connexion...` while the network is not ready;
- `Mode: Wi-Fi` or `Mode: AP` depending on the active network mode.

The home screen returns at boot and during `RFS_BOOT_HOLD_MS` (60 s), then the
screensaver takes over to limit heating and consumption. The backlight is driven by
an active-low pin.

### 11.5 Screen off and memory recovered

`display_tick()` compares the clock against the boot time and calls
`display_blank()` as soon as `RFS_BOOT_HOLD_MS` has elapsed, that is **one
minute** after power-on. `display_blank()` switches the panel off
(`disp_on_off(false)`), raises the backlight pin, then `release_frame()` frees
the framebuffer and sets the pointer back to `NULL`. The release only happens after
the SPI transfer in progress has finished, so a buffer is never returned during a DMA
write.

That behaviour is not a comfort detail: the 25,600 bytes handed back to the internal
heap are exactly what lets `wigle.c` open a TLS session and receive a response body.
While the screen stays lit, the WiGLE search can run short of contiguous memory.
**You therefore have to wait for the screen to switch off before consulting the
WiGLE history of a device.** The serial log traces both moments useful for
diagnosis: `tampon ecran libere: libre=… bloc_max=…` on the screen side, and the
free heap before and after each call on the WiGLE side.

Once off, the screen does not light up again: the home and status screens return
immediately when `s_blanked` is true. The device returns to displaying on the next
restart.

Labels follow the language chosen in the interface (French by default, English
available).

---


## 12. Build and flash

### 12.1 Prerequisites

- [ESP-IDF](https://github.com/espressif/esp-idf) **v5.3.2**, the version the project
  is compiled and tested with;
- a Linux or macOS development machine with Python 3;
- a USB port for the board.

Hardware dependencies are fetched automatically on first configuration: the screen
driver lives in `main/lcd/` and the `joltwallet/littlefs` filesystem is resolved by
the component manager.

### 12.2 Compiling

```bash
. $IDF_PATH/export.sh                # activate the ESP-IDF environment
idf.py set-target esp32s3            # once only
idf.py reconfigure                   # fetch managed components
idf.py build
```

The result is `build/rfsentinel.bin`, alongside `build/bootloader.bin` and
`build/partition-table.bin`.

### 12.3 Flashing

The simplest route, straight from a build tree:

```bash
idf.py -p /dev/ttyACM0 flash monitor    # adapt the port to your machine
```

To write the three images by hand, honouring the partition-table addresses and the
memory parameters, install esptool first — the full per-system instructions, port
names and driver notes are in the
[README](README.md#flashing-with-esptool). In short: on Linux use a virtual
environment (`python3 -m venv` then `pip install esptool`) and add yourself to the
`dialout` group; on macOS `brew install esptool`; on Windows `py -m pip install
esptool`.

esptool 5 spells its options with dashes, and that is the form to type:

```bash
# Linux
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 bin/bootloader.bin 0x8000 bin/partition-table.bin 0x10000 bin/rfsentinel.bin
```

```bash
# macOS — the port comes from ls /dev/tty.usb*
esptool --chip esp32s3 -p /dev/tty.usbmodem31401 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 bin/bootloader.bin 0x8000 bin/partition-table.bin 0x10000 bin/rfsentinel.bin
```

```powershell
# Windows — same command, backticks for line continuation
py -m esptool --chip esp32s3 -p COM3 -b 460800 --before default-reset --after hard-reset `
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0 bin\bootloader.bin 0x8000 bin\partition-table.bin 0x10000 bin\rfsentinel.bin
```

The images published in the `bin/` folder of this repository install the same way,
with no need to compile.

If your esptool came with ESP-IDF 5.3 it is version 4.x and expects underscores:
`write_flash`, `--flash_mode`, `--flash_freq`, `--flash_size`,
`--before default_reset`, `--after hard_reset`, and the program is named
`esptool.py`. Those spellings still work on esptool 5, which accepts them with a
deprecation warning.

> **Check the exact board model before writing to flash.** Two conditions have to hold
> at once: 16 MB of flash and the ST7735 screen. An 8 MB board — same case, same look
> — will refuse to boot, and a board sold without a screen has nothing for the
> firmware to drive. The target is the LilyGO T-Dongle-S3 **with display, in its 16 MB
> version**; smaller and screenless variants exist on the market.

### 12.4 Flashing from the browser

The `webtool/` folder holds a standalone page that writes the firmware through the
Web Serial API, with nothing to install on the user's side. It only exists for Chrome
and Edge, the only browsers that expose the serial port, and the browser only allows
access from HTTPS, `localhost` or `file://`. The recommended deployment is the copy
served at <https://upd.rnd0.fr/rfsentinel/>, where the page and the images come from
the same host; the local options described below stay available as fallbacks.

The engine chosen is not `esp-web-tools`, the most common solution: its manifests
write with `flashMode`, `flashFreq` and `flashSize` set to `keep`, meaning the
settings already present in the board. Yet the main risk of this project is an 8 MB
board receiving a 16 MB partition table. The RFSentinel flasher is therefore a clean
page, built on `esptool-js` 0.7.1, which imposes `dio`, `80m` and `16MB`, reads
the real capacity before writing and refuses any smaller board. The manifest only
supplies URLs and hashes: never an address nor a memory setting, and the `0x0`,
`0x8000` and `0x10000` addresses come from the page's code.

The page is bilingual French / English. A selector in the header switches every
label, including the messages read back from the board, the choice is stored in the
browser, and the first visit follows the language of the browser. Translating the
page never changes the write plan: the addresses, the memory settings and the
capacity guard stay as described above.

The page has to work offline, therefore without ES module imports, which Chrome
forbids under `file://`. The esptool-js bundle, shipped as ES modules, is converted
into a classic script by `tools/make_webtool_vendor.py`: the final `export` clause
becomes an assignment to a global, and the result is checked with `node --check`.

The online mode reads `webtool/firmware/manifest.json` and the three images that
follow it, hence the need to commit that folder.
`tools/package_webtool_firmware.py` fills it from `build/` and refuses to copy
anything if the build does not announce `esp32s3` in `dio/80m/16MB`. Attachments of
a GitHub release do not return CORS headers: the firmware is therefore loaded from the
raw files of the repository or from GitHub Pages, which do return them.

After writing, esptool-js re-reads the MD5 hash of the flash and compares it with the
hash of the image sent; the page provides that hashing function itself, `SubtleCrypto`
being unavailable in an insecure context for the SHA-256 of downloaded images. The
`storage` partition is not rewritten, so the observation journal survives a normal
write; a full erase stays an explicit user choice.

```bash
idf.py build
python3 tools/package_webtool_firmware.py
python3 -m http.server 8000      # then http://localhost:8000/webtool/
node tests/test_webtool_ui.js    # md5, manifest, 8/16 MB check, progress
```

### 12.5 Build configuration (`sdkconfig.defaults`)

| Option | Value | Why |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | `y` | 16 MB of flash |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | table described in §3.1 |
| PSRAM | disabled | the board exposes none usable here |
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | `y` | active BLE scans |
| `CONFIG_BT_NIMBLE_ROLE_OBSERVER` | `y` | reception of passive advertisements |
| `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` | `n` | the device never lets itself be connected to |
| `CONFIG_BT_NIMBLE_ROLE_BROADCASTER` | `n` | it emits no advertisements |
| `CONFIG_LWIP_MAX_SOCKETS` | `12` | interface, MQTT, NTP and WiGLE in parallel |
| `CONFIG_FREERTOS_HZ` | `1000` | scan timings to the millisecond |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | `6144` | headroom for the main loop |
| mbedTLS certificate bundle | full | HTTPS to the WiGLE API |
| log level | `INFO` | readable on the serial port without drowning the trace |

### 12.6 Images shipped in v1.0.0

| File | Size | SHA-256 |
|---|---|---|
| `rfsentinel.bin` | 1,482,464 B | `65ec22d5a8daac6222d0a90345e6e737490440ceeb8c32f8a4fcf8571cc17fa2` |
| `bootloader.bin` | 21,648 B | `f7e8d5acf0157d246f2594c415728fe98a15e294ab8a0c1cbfe4a61841fe5297` |
| `partition-table.bin` | 3,072 B | `4c41038930270e3e0073c60762b35cd693ad237238c335617ba9a03a0cfdf913` |

Matching flash parameters: `--flash-mode dio --flash-freq 80m --flash-size 16MB`, at
addresses `0x0`, `0x8000` and `0x10000`.

Those three images come from a clean build under ESP-IDF v5.3.2, with no error. A
reference build produces one warning with no functional effect: the `draw_frame`
function in `display.c` is kept but never called.

The same files, with the same hashes, are placed in `bin/` and in
`webtool/firmware/` by `tools/package_webtool_firmware.py`: the repository flasher
serves the second folder, alongside the images loaded from `bin/`. Both sources must
stay consistent, otherwise two different firmwares carrying the same version number
would be offered depending on which button was clicked. After every build meant to be
published, rerun the tool and check that `bin/SHA256SUMS` and
`webtool/firmware/SHA256SUMS` still match the content of `build/`.

---


## 13. Project state

Version 1 is finished: the advertised functions are written, compiled and used. To
stay honest about what is guaranteed, the state is split into three levels.

**Written and tested on the development machine** — off target, in `tests/`:
export format conversion, sorting and filtering of the observation list, UTC to
local-time conversion, formatting bounds, handling of unexpected input.

**Observed on the board** — behaviour verified on screen or in the interface after
flashing: power-on and screen legibility, rotation and font fixed, login and password
change, scan launches, journal filling, the complete interface, switching to the
rescue access point, time display.

**Written, compiled, but to be reviewed on the board after the last image**:
OpenStreetMap tiles on the search page, masking and revealing of the six secret fields
on the configuration page, local time in lists and filters, the actual upload to
WiGLE, the actual purge of the journal, the actual CSV download, calibration, BLE
hunting and presence alerts. Nothing points to a problem on these points; they simply
have not each been re-verified one by one on the equipment since the last build.

**Not done**: no measurement of radio range, battery life or behaviour in motion has
been carried out. Those are field validations, by hand.

---

## 14. Possible evolutions

Ideas, in the order they would add most to daily use:

- **Several devices, one table.** Each device carries its own identifier; what is
  missing is a page that merges the journals of several nodes, and synchronisation
  between nodes to discard duplicates.
- **Route.** Observations carry a position when a GPS is attached; recording the path
  followed would allow colouring the map by instant rather than by signal strength
  alone.
- **Zones and alert thresholds.** MQTT alerts and watching chosen addresses exist, but
  the general alert threshold stays single. Zones drawn on the map, each with its own
  threshold, would refine monitoring.
- **History of new networks.** The device can spot an address never seen before; it
  does not yet keep a browsable journal with date of first encounter and first
  position.
- **Upload resume.** A WiGLE upload that fails leaves the journal intact, which is the
  safe behaviour; a queue that retries on its own would avoid going back by hand.
- **Counter on screen.** The status screen stays sober; showing the number of
  observations written since boot would give immediate proof of life without opening
  the interface.
- **Larger batches to WiGLE.** The 6 KB buffer bounds the size of one submission;
  splitting into successive slices would allow uploading a complete journal in one
  operation.
- **Encrypted interface.** Embedded HTTPS would remove credentials travelling in
  clear text, at the cost of noticeable memory.

---

## 15. Known risks and limits

| Risk | Effect | Guard-rail in place |
|---|---|---|
| 8 MB board instead of 16 MB | the data partition exceeds the memory, the board does not boot | warning in §12.3, explicit size in the partition table |
| Internal RAM only, no PSRAM | impossible to hold a large batch in RAM | everything goes through the journal, bounded buffers, frees on every exit |
| Journal saturated | writes stop | alert threshold at 90 % usage, purge commanded from the interface |
| Wrong timestamps | data unusable for mapping | time quality recorded per observation and carried into the export |
| Losing the administrator password | access lost | full reset by erasing non-volatile storage |
| Listening on the local network | credentials and data travel in clear text | intended for a controlled network; session cookie `HttpOnly` and `SameSite=Strict`, lockout after three failures |
| Wi-Fi and BLE capture | the device records identifiers of surrounding networks | publication and upload left to the user's decision, never automatic |
| Screen driver without licence header | file origin hard to read for a maintainer | noted in `LICENSE` |

What falls under law rather than code: capturing the traffic of neighbouring networks
and publishing it on a platform depends on the regulations of the country of use.
Uploading to WiGLE is a user click, never an automatism, and the platform credentials
are never returned by the device.

---

## 16. Using RFSentinel

### 16.1 Getting started

1. Flash the three images (§12.3), or the images published in `bin/`.
2. Plug the board into a USB port or a 5 V charger. The screen shows the logo, then
   the version number and the address obtained.
3. Join the network offered. On first launch no Wi-Fi is stored: the device opens the
   access point `RFSentinel-Setup` itself, password `012345678`.
4. Open `http://192.168.4.1` from that same network.

### 16.2 First login

The starting account is `admin` / `admin`. Logging in necessarily opens a password
change screen: without a new password the interface stays inaccessible. That is the
only way to protect the device, since it listens on the local network.

Once logged in, the configuration page lets you fill in the target Wi-Fi, the
timezone, the device identifier, and optionally MQTT and WiGLE. After saving the
Wi-Fi, the device restarts onto that network and the address shown on screen becomes
the one to use.

### 16.3 Everyday use

Scanning starts by itself at boot if the option is left active. The status page shows
the state of Wi-Fi and Bluetooth, how full the journal is and the latest
observations, sortable and filterable. The map page places observations and known
networks on an OpenStreetMap background. The export page prepares the file in WiGLE
format, and the upload page sends it to the platform when asked.

You can also build the list of MAC addresses to watch: each one triggers an MQTT
alert on detection, whatever the signal strength, then a new alert after absence.

The weak-signal thresholds and the MQTT alert threshold are set in the configuration;
they take effect from the next scan.

### 16.4 What RFSentinel does not do

- It connects to none of the networks it detects and tests no password. It listens to
  the frames access points broadcast publicly.
- It does not locate by itself: without an attached GPS, coordinates stay zero and the
  export says so.
- It uploads nothing to WiGLE unless asked. The only automatic publications are MQTT
  alerts, and only if MQTT is enabled.
- It is not a substitute for a network audit: it produces a coverage database, not a
  compliance diagnosis.

---

## 17. Taking the project over

### 17.1 Reading order

1. `README.md`: what the device does, on which board, how to compile.
2. This file: why the choices were made, how the mechanisms work, the values.
3. The code, in the order of §3.1: `config` and `store` first, because almost
   everything else reads from or writes to them.

The French versions of both documents live in `docs/fr/` and carry the same content.

### 17.2 Host-side tests

The `tests/` folder holds thirty-one standalone programs, run on the development
machine without ESP-IDF and without hardware: twenty in Python, nine in JavaScript
through Node, two in C. They cover the logic that does not need the board: CSV export
format, sorting and filtering, time conversions and formatting bounds, recording and
MQTT alert thresholds, de-duplication, watched devices from every angle — API, logic,
MQTT payload, interface —, recounting, concurrent access to the journal, iteration
over a fragmented LittleFS, WiGLE searches in list mode, in summary and in TLS,
reading of the export, classification and filtering of BLE addresses, and four
programs that compile a piece of the firmware on the host:
`test_display_lifecycle_host.py` replays the switch-off toggle with a simulated
DMA transfer and checks the framebuffer is only returned once that transfer finished
(§11.5); `test_wigle_memory_host.py` runs the real WiGLE search code with simulated
HTTPS and failing allocations, to check every block is released on early exits;
`test_ble_config_host.py` runs the real BLE filter setter against a simulated NVS,
both on success and on a failed write; `test_fresh_storage_host.py` runs the decision
taken by `store_init()` after a failed mount, against a blank partition, one that
already holds a journal, one that is corrupted, one whose read fails, an encrypted
partition and a missing partition.

```bash
for t in tests/test_*.py; do python3 "$t"; done
for t in tests/test_*.js; do node "$t"; done

cc -Imain tests/test_ble_address.c -o /tmp/bleaddr && /tmp/bleaddr

LFS=managed_components/joltwallet__littlefs/src/littlefs
cc -I"$LFS" tests/test_littlefs_iter_host.c "$LFS/lfs.c" "$LFS/lfs_util.c" \
   -o /tmp/lfs_iter && /tmp/lfs_iter
```

The Python and JavaScript programs run as they are, with no external dependency.
`test_ble_address.c` compiles with a plain `-Imain`, since classifying an address
has no ESP dependency. The other one compiles against the LittleFS library fetched
by ESP-IDF, hence the `-I` on the components folder; that compilation has to come
after an `idf.py reconfigure`, the only thing able to create the folder. There is
no single orchestrator in the repository: each program prints what it checks and
exits non-zero on failure.

These tests prove nothing about the radio, the screen or the filesystem: they cover
data processing, not the target.

### 17.3 Code conventions

- A feature is only held to be operational if it has been observed on the board. A
  clean compile is not enough, and §13 draws the distinction explicitly.
- Every value a user can see goes through a named constant in a header, never a
  number written in the middle of a handler.
- Buffers have a size known in advance, and every early exit frees what was allocated.
  That is the direct consequence of having no PSRAM.
- Sensitive fields never leave the device towards the interface: they are not returned
  by the API, and the interface never redisplays them.
- French is the reference language; the displayed strings exist in both languages and
  are selected by `ui_lang`.

### 17.4 What does not belong in the repository

`.gitignore` keeps the repository light and only lets in what serves to compile,
understand or use the device. What stays out, and why:

| Excluded | Reason |
|---|---|
| `build/`, `managed_components/`, `sdkconfig` | produced by `idf.py` on whoever compiles; `sdkconfig` carries local settings |
| `.build*.log`, `*.log`, `backups/` | build logs and backups of the real radio journal, therefore real environment readings |
| `*.bin`, `*.elf`, `*.map` | flash images; the published version is the one in `bin/` and on the release page |
| `./*.h`, `/lcd/`, SVG at the root | working duplicates; the source of truth is in `main/` and `logo/` |
| `notes/`, `01-*.md`, `02-*.md`, `03-*.md`, `PROGRESS.md` | internal design journal, with real addresses and real readings |
| `._*`, `.DS_Store` | macOS metadata |
| `.env`, `*.pem`, `*.key`, `credentials*`, `secrets*` | nothing confidential belongs in repository history |

One exception assumes the rule: `!bin/*.bin` and `!webtool/firmware/*.bin`. The
browser flasher needs its three images to work without a prior download — the "From
repository" mode — and `bin/` exists so the images can be flashed without compiling;
they are therefore tracked despite `*.bin`. The point to watch is that the release
page, `bin/` and `webtool/firmware/` must carry the same hashes (§12.6).

`screen/` only contains the five screenshots of the landing page, resampled to twice
the width at which the README displays them — 1012 px wide maximum for the
observations page, 940 px for the two-column ones — so they stay sharp on a Retina
screen without carrying pixels nobody will see. The login file is the exception: 23 KB
at 1400 px, and any further resampling would push it to 177 KB because anti-aliasing
destroys the compression of flat areas. Before publishing, run `exiftool` over these
files again: none should carry a timestamp, a make, a model, coordinates or XMP.

Before any publication, check what would actually go, then reread this list looking
for an identity, a network address or a forgotten secret:

```bash
git ls-files --others --exclude-standard | while IFS= read -r f; do
  grep -InH -E 'franck|rondot|contact@|/home/|/Volumes/|/Users/|[0-9]{1,3}([.][0-9]{1,3}){3}' "$f"
done
```

The only IP addresses this search should find are the rescue AP address,
`192.168.4.1`, a constant in `main/net.h`, and the `192.168.1.1` given as an
example in the broker field of the configuration page. The first-run credentials —
`admin`, `RFSentinel-Setup`, `012345678` — are expected too: they are documented
default values, not secrets.

---


## 18. Reference constants

Every value below is read from the code, with the name it carries there.

### 18.1 Identity and access

| Constant | Value | Location |
|---|---|---|
| `RFS_VERSION` | `1.0.0` | `main/version.h` |
| `CFG_AP_SSID` | `RFSentinel-Setup` | `main/config.h` |
| `CFG_AP_PASS` | `012345678` | `main/config.h` |
| `NET_AP_DEFAULT_IP` | `192.168.4.1` | `main/net.h` |
| `CFG_DEFAULT_USER` / `CFG_DEFAULT_PASS` | `admin` / `admin` | `main/config.c` |
| `NVS_NS` | `rfsentinel` | `main/config.c`, `net.c`, `store.c` |
| `SESSION_TTL_US` | 1 h | `main/auth.c` |
| Session cookie | `sid; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600` | `main/auth.c` |
| Login lockout | 30 s after 3 failures | `main/auth.c` |
| `FALLBACK_EPOCH` | `1735689600` (2025-01-01 UTC) | `main/net.c` |
| `TZ_DEFAULT` | `CET-1CEST,M3.5.0,M10.5.0/3` | `main/net.c` |
| Default NTP server | `pool.ntp.org` | fallback value in `main/config.c` |
| `NTP_RESYNC_INTERVAL_S` | 21,600 s (6 h) | `main/net.c` |
| `TIME_REF_SAVE_INTERVAL_S` | 3,600 s | `main/net.c` |
| `STA_CONNECT_TIMEOUT_MS` | 8,000 ms | `main/net.c` |
| `STA_MAX_RETRY` | 4 | `main/net.c` |

### 18.2 Scanning

| Constant | Value | Location |
|---|---|---|
| `SCAN_WIFI_INTERVAL_S` | 4 s | `main/scanner.c` |
| `SCAN_WIFI_MAX_APC` | 40 networks per round | `main/scanner.c` |
| `SCAN_BLE_INTERVAL_S` | 20 s | `main/scanner.c` |
| `SCAN_BLE_DURATION_MS` | 10,000 ms | `main/scanner.c` |
| `SCAN_BLE_MAX` | 12 devices per round | `main/scanner.c` |
| `CALIBRATION_TIMEOUT_MS` | 15 min | `main/scanner.c` |
| `SCANNER_FIND_MAX` | 48 | `main/scanner.h` |
| Scan mode | passive, `filter_duplicates=1` | `main/scanner.c` |
| BLE address filter | 0 all, 1 stable, 2 public | `main/ble_address.h` |
| BLE diagnostics | 5 counters, saturate at 4,294,967,295 | `main/scanner.c` |

### 18.3 Journal and data

| Constant | Value | Location |
|---|---|---|
| `RFS_MOUNT` | `/rfs` | `main/store.c` |
| `RFS_OBS_DIR` | `/rfs/observations` | `main/store.c` |
| `RFS_OBS_PREFIX` | `obs_` | `main/store.c` |
| `RFS_OBS_MAX_FILES` | 32 files | `main/store.c` |
| `RFS_SEGMENT_MAX_BYTES` | 32,768 B | `main/store.c` |
| `RFS_LINE_MAX` | 256 B | `main/store.c` |
| `RFS_LIST_PAGE` | 14 rows per page | `main/store.c` |
| `RFS_SORT_MAX` | `RFS_LIST_PAGE + 1` | `main/store.c` |
| `RFS_HIST_MAX` | 10 entries | `main/store.c` |
| `RFS_USAGE_LIMIT_PCT` | 90 % | `main/store.c` |
| `SEEN_CAPACITY` | 128 known addresses | `main/store.c` |
| `BL_CACHE_MAX` | 64 | `main/store.c` |
| `BL_INDEX_MAX` | `BL_CACHE_MAX * 18` | `main/store.c` |
| `OBS_SSID_MAX` / `OBS_AUTH_MAX` | 33 / 16 | `main/store.h` |

The export header is `WigleWifi-1.6,apssids=true,latlon=true,
lockedToNetwork=false,cellBands=auto`.

### 18.4 MQTT

| Constant | Value | Location |
|---|---|---|
| `MQTT_SEEN_MAX` | 16 | `main/mqtt.c` |
| `MQTT_URL_MAX` | 96 | `main/mqtt.c` |
| `OBS_MAC_STR` / `OBS_TS_STR` | 18 / 20 | `main/mqtt.c` |
| Default topic | `/rfsentinel/alert` | `main/config.c` |
| Default client id | `rfsentinel` | `main/config.c` |
| Default port | 1883, no TLS | `main/config.c` |
| Quality of service | 1, no retention | `main/mqtt.c` |
| Message size | 384 B at most | `main/mqtt.c` |
| `PRESENCE_MAX` / `PRESENCE_LABEL_MAX` | 32 / 32 | `main/presence.h` |
| `PRESENCE_RETRY_US` | 5 s | `main/presence.c` |
| NVS namespace for watching | `rfs_presence` | `main/presence.c` |
| Default presence topic | `/rfsentinel/presence` | `main/presence.c` |
| Absence before a return alert | 120 s, 30 s minimum | `main/presence.c` |
| Presence message | 512 B at most | `main/mqtt.c` |

### 18.5 Screen

| Constant | Value | Location |
|---|---|---|
| `RFS_SCREEN_W` / `RFS_SCREEN_H` | 160 / 80 | `main/display.c` |
| `RFS_LINE_H` | 16 px | `main/display.c` |
| `RFS_MARGIN` | 4 px | `main/display.c` |
| `RFS_LOGO_H` | 64 px | `main/display.c` |
| `OLED_LOGO_W` / `OLED_LOGO_H` | 64 / 64 | `main/oled_logo.h` |
| Boot signature | `By M3r0W1g` | `draw_credit` in `main/display.c` |
| `RFS_BOOT_HOLD_MS` | 60,000 ms, that is 1 min | `main/display.c` |
| Framebuffer | 25,600 B in `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`, freed on blank | `main/display.c` |
| SPI clock | 20 MHz | `main/display.c` |

| Pin | GPIO | Role |
|---|---|---|
| `RFS_PIN_LCD_CLK` | 5 | SPI clock |
| `RFS_PIN_LCD_MOSI` | 3 | data |
| `RFS_PIN_LCD_DC` | 2 | register or image |
| `RFS_PIN_LCD_RST` | 1 | reset |
| `RFS_PIN_LCD_CS` | 4 | slave select |
| `RFS_PIN_LCD_BL` | 38 | backlight, active low |

### 18.6 Interface buffers

| Constant | Value | Location |
|---|---|---|
| `RFS_UI_BUF` | 4,096 B | `main/web.c` |
| `RFS_OBS_BUF` | 8,192 B (falls back to 4,096 B) | `main/web.c` |
| `RFS_HIST_BUF` | 8,192 B | `main/web.c` |
| `RFS_POST_MAX` | 4,096 B | `main/web.c` |
| `RFS_WIGLE_BUF` | 8,192 B | `main/web.c` |
| `RFS_EXPORT_BUF` | 4,096 B | `main/web.c` |
| `WIGLE_BODY_MAX` | 6,144 B | `main/wigle.c` |
| `WIGLE_MAX_ROWS` | 10 rows | `main/wigle.c` |
| HTTP handlers | 28 declared for 27 routes | `main/web.c` |

### 18.7 First-configuration values

These apply until the user changes something.

| Setting | Initial value |
|---|---|
| Weak-signal threshold | -95 dBm |
| MQTT alert threshold | -60 dBm |
| Rediscovery delay | 60 min |
| Wi-Fi scan, BLE scan, automatic start | enabled |
| BLE address filter | stable: public and static random |
| Language | French |
| MQTT | disabled, port 1883, no TLS |
| Device identifier | `rfsentinel-01` |
| Position | latitude 0, longitude 0 |
| NTP server | `pool.ntp.org` |
| Timezone | `CET-1CEST,M3.5.0,M10.5.0/3` |

---

## 19. Versions

| Version | Date | Content |
|---|---|---|
| 1.0.0 | November 2026 | first complete release, images refreshed with the fresh-card storage fix |

Version 1.0.0 brings together Wi-Fi and BLE scanning, the journal on the board, the
web interface in French and English, the map, export in WiGLE format, upload to
WiGLE, MQTT alerts, the screen and the rescue access point. The images published later
under this same number correct the start-up on a card that has never been flashed:
the `storage` partition used to stay unmounted for good there, with no way to record
observations. The number in `main/version.h` did not move, since the advertised
functions are the same.

The number is defined once, in `main/version.h`, and picked up by the API, the
screen and the interface. A published version is not modified afterwards: fixes go
into the next version.

---

## 20. Where to look in the code

| File | Role |
|---|---|
| `main/main.c` | startup, initialisation order, main loop |
| `main/version.h` | version number |
| `main/config.c` | settings persisted in non-volatile storage |
| `main/store.c` | journal on the board, list, sort, filters, purge, blacklist |
| `main/scanner.c` | Wi-Fi and BLE scans, calibration, BLE address counters |
| `main/ble_address.h` | BLE address classification and filtering |
| `main/net.c` | connection, rescue access point, NTP, local time |
| `main/auth.c` | sessions, lockout after failures |
| `main/web.c` | HTTP routes, pages, API, export |
| `main/ui/index.html` | the whole interface, in one piece |
| `main/mqtt.c` | alerts |
| `main/presence.c` | watched addresses, presence alerts |
| `main/wigle.c` | upload and lookup |
| `main/display.c` | framebuffer, rotation, home screen, status, blanking |
| `main/oled_logo.h` | 64 × 64 logo |
| `main/lcd/esp_lcd_st7735.c` | screen controller driver |
| `partitions.csv` | flash layout |
| `sdkconfig.defaults` | build options |
| `tests/` | off-target tests |

---

## 21. Licence

RFSentinel is published under **Creative Commons Attribution-NonCommercial-
ShareAlike 4.0 International** (CC BY-NC-SA 4.0). The full text is in `LICENSE`.

In practical terms:

- you may copy, modify, adapt, compile for your board, distribute;
- provided you credit the author;
- provided you do not use it commercially;
- and provided any modified or derived version stays under the same licence, that is
  free and non-commercial in its turn. A fork stays open.

Commercial use remains possible, but only with written agreement from the author.

Two honest notes for someone taking the project over:

- CC BY-NC-SA is not an Open Source Initiative approved licence, because the
  non-commercial clause is judged discriminatory by that definition. The project
  owns this choice: the goal is free distribution with commercial exploitation
  forbidden, not compliance with that definition.
- the `joltwallet/littlefs` component, fetched by the component manager, stays under
  Apache-2.0 and follows its own terms. The driver `main/lcd/esp_lcd_st7735.c`
  carries no licence header; that uncertain origin is flagged in `LICENSE`.

