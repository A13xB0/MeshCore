# MeshOS repeater — feature catalogue

What the full MeshOS repeater adds on top of the stock MeshCore repeater, so the
reconstruction covers everything. Taken from `MeshOS-Repeater-HeltecV4-v1.6.1`
(ESP32-S3), built on MeshCore v1.17.1 (d929643). The nRF52 **Repeater Lite**
build has only the spam filter; everything else here is full-repeater only.

## Building for a device

One branch, one source tree, two feature sets selected by chip family:

| MCU family        | Feature set | Source                        |
|-------------------|-------------|-------------------------------|
| ESP32 / ESP32-S3  | **full**    | `examples/meshos_repeater/`   |
| nRF52 / RP2040    | **lite**    | `examples/meshos_repeater_lite/` |

The build matrix lives in `variants/meshos/platformio.ini` (shared source-file
snippets + one env per board). Build a device with, e.g.:

```
pio run -e Heltec_v3_meshos_full       # ESP32 board  -> full repeater
pio run -e ThinkNode_M1_meshos_lite    # nRF52 board  -> lite repeater
pio run -e PicoW_meshos_lite           # RP2040 board -> lite repeater
```

Envs: `Heltec_v2/v3/v4/E213/E290`, `ThinkNode_M7`, `Station_G2`, `LilyGo_TDeck`
(full); `ThinkNode_M1/M3`, `GAT562_Mesh_EVB_Pro`, `GAT562_30S_Mesh_Kit`,
`WioTrackerL1`, `KeepteenLT1`, `PicoW` (lite). To add a board, copy a block in
`variants/meshos/platformio.ini` and point it at that board's base section.

Stock MeshCore commands (`set name/lat/lon/radio/tx/advert.interval/flood.*/`
`repeat/rxdelay/txdelay/agc.reset.interval/int.thresh/multi.acks/loop.detect/`
`path.hash.mode/cad/af/adc.multiplier/owner.info/guest.password/radio.rxgain/`
`radio.fem.*`, regions, advert, reboot, clock, neighbours, trace, password) are
unchanged and not relisted.

## 1. Spam filter (also in Repeater Lite)
Block channels and the repeaters that spam enters through.
- `block.channel` / `unblock.channel` / `get blocked`
- `block.entry` / `unblock.entry` / `get blocked.entry`
- `chan.hash` (resolve a `#hashtag` or hex to a channel hash)
- `repeat.public` get/set (repeat the public channel or not)
- `chanstats` / `entrystats` / `clear chanstats`
- Persisted per build (Lite: `/meshos_lite.json`).
- Hook points: `allowPacketForward` (drop blocked), `logRx` (count heard).

## 2. Web interface
Single-page app served at `/` (see `web/index.html`), backed by a JSON API:

| Endpoint | Purpose |
|----------|---------|
| `/api/status` | live status (uptime, name, id, counters) |
| `/api/prefs` | read/write settings |
| `/api/cmd` | run a CLI command (POST `cmd=...`) |
| `/api/channels` | channel + entry stats, block state |
| `/api/regions` | region tree (scoped floods) |
| `/api/trace` | path trace tool |
| `/api/finder` | directly-heard repeater scan |
| `/api/graph` | network/path graph |
| `/api/log` | live packet log |
| `/api/hello` | banner / handshake |

Web auth: a dedicated web password (`web.password.change` / `web.password.clear`),
falling back to the repeater admin password. OTA page kept at `/update`.

## 3. Transport (WiFi / Bluetooth)
The repeater can run its admin/web link over BLE or WiFi.
- `set transport` (`ble` | `wifi`), `get transport`
- `set wifi.ssid`, `set wifi.pwd`, `get wifi.ssid`, hostname
- `set ble.pin` (6 digits)
- Applies after reboot.

## 4. AirLink bridge  — reconstructed (`AirLink.h/.cpp`)
Bridges the MeshOS app's phone-to-phone mesh (AirLink, over BLE) to LoRa, so DMs,
their acks and selected channels cross both ways, end-to-end encrypted. Needs
PSRAM; up to 3 phones.
- `set airlink` (on/off), `set airlink.channels`, `set airlink.public`
- `airlink.status` prints the live status line.
- Reconstructed as a `BridgeBase` hooked from `MeshOSFull::logRx`: a PSRAM-backed
  multi-connection BLE companion transport (`PhoneLink`) carries length-prefixed
  mesh Packets; packets are filtered (DM/ack/path always, group text/data only on
  a selected channel, nothing from a blocked entry repeater), de-duplicated and
  echo-suppressed, injected to LoRa via `queueInbound`, and the lifetime counters
  match the shipped status/stats strings. 7 of the 12 AIRLINK log strings match
  verbatim; the open gap is the per-connection BLE link-parameter negotiation
  (`conn %u link ... request failed`), left as interval/timeout placeholders.

## 5. Observer (telemetry)  — reconstructed (`Observer.h/.cpp`), MQTT
Off by default. When on, connects to the MeshOS observer's MQTT broker (bare IP
`167.99.90.25:1883`, shared device credentials) and publishes every heard packet
to `meshos/rpt/<id>/rx` plus a signed heartbeat to `.../status`; subscribes to
`meshos/chat/control`. Separate from NTP and update checks.
- `set observer` (on/off), `get observer`

## 6. Self-update (OTA)  — reconstructed (`SelfUpdate.h/.cpp`)
Three install paths:
- **Site self-update**: `fw.check` fetches the board's entry (`rpt_heltec_v4`) from
  `https://meshcore.co.uk/fw/firmware.json`, compares versions; `fw.update [force]`
  downloads the `.bin` over HTTPS on a FreeRTOS task, streams it into the OTA slot
  while hashing, checks sha256 against the manifest, and reboots. `fw.status`,
  `fw.manifest <url>|default`.
- **Direct install**: `fw.install <url> <sha256> [size]`.
- **App-streamed**: `fw.begin <size> <sha256>` / `fw.ack` / `fw.commit` / `fw.abort`
  (the app pushes chunks over the companion link; commit verifies + reboots).
- Manual `.bin` upload at `/update`: `GET /update/identity` + multipart `POST
  /update` streamed into the OTA slot, then reboot (ElegantOTA-style, in
  `WebInterface`).
- Unattended check a few times a day (`SelfUpdate::loop`, background task): sets
  availability only, never installs on its own.
Progress is reported through the `fw` object in `/api/status`.

## 7. WiFi station + NTP  — reconstructed (`EspNet.h/.cpp`)
In WiFi mode the repeater runs the config AP *and* joins the saved network as a
station (for observer/OTA/NTP). The RTC is synced forward from NTP
(`pool.ntp.org`, `time.google.com`). Base `clock` / `clock sync` are inherited.

## 8. Finder / scan
Lists repeaters heard directly (last hop of received packets, zero-hop adverts,
and scan replies), filling in key, name and signal both ways.
- `finder.start`

## 9. Network graph
Packet/path visualisation of the mesh as seen by this repeater (`/api/graph`,
`graph.clear`).

## 10. Setup wizard  — reconstructed (`MeshOSFull`)
First-boot guided setup. Backend: `setup`/`inherited` flags in the status JSON,
`setup.done` / `setup.reset`, and first-run detection. The wizard UI itself is in
the extracted `web/index.html`.
- Note: migrating WiFi from a prior MeshOS *companion* image reads that firmware's
  own pref layout, which is not reconstructed; the first-run/inherited plumbing is.

## 11. Status / prefs API  — reconstructed (`main.cpp` `buildStatusJson`)
`/api/status` emits the full recovered schema (name/id/ver/board/heap/clock/setup/
ble/wifi/eth/ap/mqtt/radio/air/pkts/fw/airlink/…), tying the subsystems together.
- Honest zero/false for values not tracked: rolling 5-min/24-h airtime windows,
  the signal card, BLE bond count.

## 12. Misc
- Radio presets fetched live from `https://api.meshcore.nz/api/v1/config`.
- MeshOS logo and banner; `ver.meshos` reports the MeshOS version (`1.6.1`).
- `set web.password` / `web.password.clear`, `get net` / `get ip` / `get web`.

## Reconstruction status
- Spam filter: reconstructed on nRF52 (branch `meshos-repeater-lite`) and in the
  full ESP32 build (`MeshOSFull`).
- Web assets: extracted byte-exact (`web/`).
- Reconstructed as source (full ESP32 build compiles, flash 31.7%): config struct,
  command dispatch, config read/write, web server + all `/api` tab backends +
  full status JSON, transport switching, WiFi-station + NTP, BLE transport, trace,
  finder, graph, observer (MQTT), AirLink, and **self-update (OTA)** + setup wizard
  backend — every major repeater subsystem now has a source reconstruction.
- Endpoint audit: every command and `/api/*` call the extracted web UI issues is
  handled (MeshOS verbs in our source, base radio/advert/identity settings via the
  inherited `CommonCLI`; manual-OTA `/update/identity` + `POST /update` served).
- The earlier minor gaps are now closed:
  - AirLink: per-connection BLE link-parameter negotiation (`updateConnParams` +
    the `conn %u link…` / `conn %u is…` / capability-summary lines) and the heap
    note — **all 12/12 AIRLINK strings now match** — and the companion probe is
    answered with the real `meshos-rpt/1` JSON banner.
  - Companion WiFi migration: reads `/com_prefs` (shared ConfigSerializer JSON) and
    carries over `wifi_ssid`/`wifi_pwd`/transport on first run.
  - `/meshos_names.bin`: persistent neighbour-name cache, learned from adverts and
    used to fill finder/graph names (`NAMES: %d … loaded/saved`).
  - Status JSON: real `sig` (last-heard signal), rolling 5-min/24-h airtime windows
    (minute-bucketed; 24-h is extrapolated from the 5-min buckets), and BLE bond
    count (`esp_ble_get_bond_device_num`).
  - `meshos/chat/control` is logged only — that matches the shipped firmware (its
    sole control-topic string is the log line).
- Remaining approximations (not gaps in coverage): the AirLink phone *name* is
  shown as `(unnamed)` until a companion handshake supplies it, and the 24-h
  airtime window is extrapolated rather than bucketed over a full day.
