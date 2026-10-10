# MeshOS repeater — feature catalogue

What the full MeshOS repeater adds on top of the stock MeshCore repeater, so the
reconstruction covers everything. Taken from `MeshOS-Repeater-HeltecV4-v1.6.1`
(ESP32-S3), built on MeshCore v1.17.1 (d929643). The nRF52 **Repeater Lite**
build has only the spam filter; everything else here is full-repeater only.

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

## 4. AirLink bridge
Bridges the MeshOS app's phone-to-phone mesh (AirLink, over BLE) to LoRa, so DMs,
their acks and selected channels cross both ways, end-to-end encrypted. Needs
PSRAM; up to 3 phones.
- `set airlink` (on/off), `set airlink.channels`, `set airlink.public`

## 5. Observer (telemetry)  — HTTP, not MQTT
Off by default. When on, uploads every heard packet plus a signed heartbeat to the
MeshOS observer at meshcore.co.uk over HTTP. Separate from NTP and update checks.
- `set observer` (on/off), `get observer`

## 6. Self-update (OTA)
The repeater checks meshcore.co.uk a few times a day, downloads a firmware for its
board, verifies its sha256 against the manifest, writes it to the spare slot and
reboots. Identity, settings and network config are preserved.
- `fw.check`, `fw.update`; manual `.bin` upload still at `/update` (ElegantOTA).

## 7. Finder / scan
Lists repeaters heard directly (last hop of received packets, zero-hop adverts,
and scan replies), filling in key, name and signal both ways.
- `finder.start`

## 8. Network graph
Packet/path visualisation of the mesh as seen by this repeater (`/api/graph`,
`graph.clear`).

## 9. Setup wizard
First-boot guided setup (`setup.done`, `setup.reset`), re-runnable from Settings.

## 10. Misc
- Radio presets fetched live from `https://api.meshcore.nz/api/v1/config`.
- MeshOS logo and banner; `ver.meshos` reports the MeshOS version.

## Reconstruction status
- Spam filter: reconstructed on nRF52 (branch `meshos-repeater-lite`).
- Web assets: extracted byte-exact (`web/`).
- Everything else (web server + API, transport, AirLink, observer, OTA, finder,
  graph, wizard): catalogued here, not yet reconstructed as source.
