# MeshOS repeater — feature catalogue

What the MeshOS repeater adds on top of the stock MeshCore repeater. The source
was rebuilt from `MeshOS-Repeater-HeltecV4-v1.6.1` (ESP32-S3, MeshCore v1.17.1,
d929643). The nRF52/RP2040 **Repeater Lite** build has only the spam filter;
everything else here is full-repeater only.

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
- Persisted with each build's settings (Lite: `/meshos_lite.json`, full:
  `/meshos_net.json`).
- One implementation for both builds: `examples/meshos_shared/SpamFilter.h`.
  Hooked from `allowPacketForward` (drop blocked) and `logRx` (count heard).

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

## 4. AirLink bridge (`AirLink.h/.cpp`)
Bridges the MeshOS app's phone-to-phone mesh (AirLink, over BLE) to LoRa, so DMs,
their acks and selected channels cross both ways, end-to-end encrypted. Needs
PSRAM; up to 3 phones.
- `set airlink` (on/off), `set airlink.channels`, `set airlink.public`
- `airlink.status` prints the live status line.
- A `BridgeBase` hooked from `MeshOSFull::logRx`: a PSRAM-backed
  multi-connection BLE transport (`PhoneLink`) carries length-prefixed mesh
  Packets; packets are filtered (DM/ack/path always, group text/data only on a
  selected channel, nothing from a blocked entry repeater), de-duplicated and
  echo-suppressed, and injected to LoRa via `queueInbound`.

## 5. Observer (telemetry, MQTT) (`Observer.h/.cpp`)
Off by default. When on, connects to the MeshOS observer's MQTT broker (bare IP
`167.99.90.25:1883`, shared device credentials) and publishes every heard packet
to `meshos/rpt/<id>/rx` plus a signed heartbeat to `.../status`; subscribes to
`meshos/chat/control`. Separate from NTP and update checks.
- `set observer` (on/off), `get observer`

## 6. Self-update (OTA) (`SelfUpdate.h/.cpp`)
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

## 7. WiFi station + NTP (`EspNet.h/.cpp`)
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

## 10. Setup wizard (`MeshOSFull`)
First-boot guided setup. Backend: `setup`/`inherited` flags in the status JSON,
`setup.done` / `setup.reset`, and first-run detection. On the first boot after a
MeshOS *companion* image, its WiFi settings are copied from `/com_prefs`. The
wizard UI itself is in `web/index.html`.

## 11. Status / prefs API (`main.cpp` `buildStatusJson` / `buildPrefsJson`)
`/api/status` reports name/id/ver/board/heap/clock/setup/ble/wifi/eth/ap/mqtt/
radio/air/pkts/fw/sig/airlink, tying the subsystems together.

## 12. Misc
- Radio presets fetched live from `https://api.meshcore.nz/api/v1/config`.
- MeshOS logo and banner; `ver.meshos` reports the MeshOS version (`1.6.1`).
- `set web.password` / `web.password.clear`, `get net` / `get ip` / `get web`.

## Code map

| File | What it does |
|------|--------------|
| `../meshos_shared/SpamFilter.h` | spam filter lists, matching, counters and hex/argument parsing (both builds) |
| `MeshOSPrefs.h` | full-repeater settings (`/meshos_net.json`) |
| `MeshOSFull.h/.cpp` | the full repeater mesh class: filter + settings CLI, packet log, web API payloads, observer payloads, names cache, airtime |
| `main.cpp` | setup/loop, `/api/status` + `/api/prefs`, CLI commands that need AirLink/self-update/WiFi |
| `WebInterface.h/.cpp` | WiFi AP + HTTP routes |
| `AirLink.h/.cpp` | AirLink phone bridge and its BLE transport |
| `Observer.h/.cpp` | MQTT observer client |
| `SelfUpdate.h/.cpp` | `fw.*` commands and OTA install paths |
| `EspNet.h/.cpp` | WiFi station + NTP |
| `MiniJson.h` | tiny flat-JSON readers (manifest, `/com_prefs`) |
| `web/`, `web_assets.h` | the embedded web pages (see `web/README.md`) |
| `../meshos_repeater_lite/` | the Lite build |

## Known issues

Behaviour kept as found; worth fixing separately:

- The full build never calls `SpamFilter::noteHeard`, so the Channels tab's
  heard-channel / heard-entry lists (`chans`, `en` in `/api/channels`) stay empty.
- AirLink resolves `airlink.channels` hashtags without the `#` and with a 32-byte
  secret, unlike the spam filter (`#` included, 16 bytes), so the same hashtag
  can resolve to different channel hashes.
- `airTx24` / `airRx24` scale the 5-minute window by 288/5 instead of 288, so the
  24 h airtime reads 5x low; the hourly buckets are not used.
- The status "sig" hop reads `path[path_len - 1]` using the *encoded* `path_len`,
  which is only right for 1-byte path hashes.
- `MESHOS_BOARD_NAME` / `MESHOS_HW_NAME` default to the Heltec V4 for every
  board, and `FW_BOARD_KEY` defaults to `rpt_heltec_v4`, so other boards report
  (and look for updates as) a Heltec V4 unless the env overrides them.
- The observer broker credentials are compiled in (they are shared by every
  MeshOS repeater).
