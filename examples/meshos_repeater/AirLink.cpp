#include "AirLink.h"

#ifdef ESP32

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Companion (Nordic UART) GATT profile - the same service the single-connection
// SerialBLEInterface advertises, so the MeshOS app discovers the repeater exactly
// as it would a companion radio. AirLink just accepts several links at once.
#define AL_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define AL_CHAR_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // phone -> repeater (write)
#define AL_CHAR_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // repeater -> phone (notify)

#ifndef AL_FRAME_MAX
  #define AL_FRAME_MAX   256        // one length-prefixed mesh Packet
#endif
#define AL_RECV_RING     16         // PSRAM-backed inbound frame ring
#define AL_RATE_WINDOW   1000UL     // per-phone send cap window (ms)
#define AL_RATE_MAX      12         // ... max phone->LoRa injections per window

// A companion-protocol probe we answer with a banner (the app sometimes opens a
// link thinking the node is a companion radio). First byte 0x00 == app "APP_START".
static const uint8_t COMPANION_PROBE = 0x00;
static const char REPEATER_BANNER[] =
  "MeshOS repeater: not a companion radio - use the app's Repeater manager";

// ===========================================================================
// PhoneLink - the multi-connection BLE companion transport.
//
// Rebuilt on the same ESP32 BLE stack SerialBLEInterface uses, but holding up to
// AIRLINK_MAX_PHONES links at once. TX notifications go to every subscribed phone
// (each phone keeps only the packets it can decrypt - end-to-end encrypted as
// sent), which is exactly what the LoRa->phones broadcast needs. Inbound frames
// are tagged with their conn_id and queued in a PSRAM ring. Connect / disconnect
// events are queued for AirLink::loop() so it can print the recovered log lines.
// ===========================================================================
class PhoneLink : public BLEServerCallbacks, public BLECharacteristicCallbacks {
public:
  struct Conn { bool used=false; uint16_t conn_id=0; uint16_t mtu=23; };
  struct InFrame { uint16_t conn_id; uint16_t len; uint8_t buf[AL_FRAME_MAX]; };
  struct Evt { uint8_t kind; uint16_t conn_id; uint16_t mtu; uint8_t reason; };  // kind:1=join 2=left

  bool allocBuffers() {
    // peer queues live in PSRAM - honours "needs PSRAM" and "no memory for peer queues"
    _ring = (InFrame*) heap_caps_malloc(sizeof(InFrame) * AL_RECV_RING, MALLOC_CAP_SPIRAM);
    return _ring != NULL;
  }

  bool begin(uint32_t pin) {
    BLEDevice::setMTU(AL_FRAME_MAX);
    BLEDevice::setSecurityCallbacks(NULL);
    _server = BLEDevice::createServer();
    if (!_server) return false;
    _server->setCallbacks(this);
    BLEService* svc = _server->createService(AL_SERVICE_UUID);
    _tx = svc->createCharacteristic(AL_CHAR_TX_UUID,
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    _tx->addDescriptor(new BLE2902());
    BLECharacteristic* rx = svc->createCharacteristic(AL_CHAR_RX_UUID,
            BLECharacteristic::PROPERTY_WRITE);
    rx->setCallbacks(this);
    svc->start();
    _server->getAdvertising()->addServiceUUID(AL_SERVICE_UUID);
    _server->getAdvertising()->start();
    _running = true;
    return true;
  }

  void stop() {
    if (_server) _server->getAdvertising()->stop();
    _running = false;
  }
  bool isRunning() const { return _running; }

  // broadcast a frame to every connected phone (each keeps what it can decrypt)
  void broadcast(const uint8_t* buf, uint16_t len) {
    if (!_tx || _n == 0) return;
    _tx->setValue((uint8_t*)buf, len);
    _tx->notify();
  }

  // pop one inbound frame; false if none
  bool nextFrame(InFrame& out) {
    if (_ring_count == 0) return false;
    out = _ring[_ring_tail];
    _ring_tail = (_ring_tail + 1) % AL_RECV_RING;
    _ring_count--;
    return true;
  }
  bool nextEvent(Evt& out) {
    if (_evt_count == 0) return false;
    out = _evt[_evt_tail];
    _evt_tail = (_evt_tail + 1) % 8;
    _evt_count--;
    return true;
  }
  uint8_t connected() const { return _n; }
  uint16_t mtuOf(uint16_t conn) const {
    for (auto& c : _conns) if (c.used && c.conn_id == conn) return c.mtu;
    return 23;
  }

  // BLEServerCallbacks
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override {
    uint16_t conn = p->connect.conn_id;
    uint16_t mtu = _server->getPeerMTU(conn);
    for (auto& c : _conns) {
      if (!c.used) { c.used = true; c.conn_id = conn; c.mtu = mtu; _n++; pushEvt(1, conn, mtu, 0); break; }
    }
    // keep advertising so further phones (up to the slot cap) can still join
    _server->getAdvertising()->start();
  }
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* p) override {
    for (auto& c : _conns) if (c.used && c.conn_id == p->mtu.conn_id) c.mtu = p->mtu.mtu;
  }
  void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override {
    uint16_t conn = p->disconnect.conn_id;
    for (auto& c : _conns) {
      if (c.used && c.conn_id == conn) { c.used = false; if (_n) _n--; pushEvt(2, conn, 0, p->disconnect.reason); break; }
    }
    _server->getAdvertising()->start();
  }
  // BLECharacteristicCallbacks
  void onWrite(BLECharacteristic* ch, esp_ble_gatts_cb_param_t* p) override {
    std::string v = ch->getValue();
    if (v.empty() || v.size() > AL_FRAME_MAX || !_ring) return;
    if (_ring_count >= AL_RECV_RING) { _overflow++; return; }  // caller counts drops
    InFrame& f = _ring[_ring_head];
    f.conn_id = p->write.conn_id;
    f.len = (uint16_t) v.size();
    memcpy(f.buf, v.data(), f.len);
    _ring_head = (_ring_head + 1) % AL_RECV_RING;
    _ring_count++;
  }

  uint32_t overflow() const { return _overflow; }

private:
  void pushEvt(uint8_t kind, uint16_t conn, uint16_t mtu, uint8_t reason) {
    if (_evt_count >= 8) return;
    _evt[_evt_head] = { kind, conn, mtu, reason };
    _evt_head = (_evt_head + 1) % 8;
    _evt_count++;
  }

  BLEServer* _server = NULL;
  BLECharacteristic* _tx = NULL;
  bool _running = false;
  Conn _conns[AIRLINK_MAX_PHONES];
  uint8_t _n = 0;

  InFrame* _ring = NULL;
  uint8_t  _ring_head = 0, _ring_tail = 0, _ring_count = 0;
  uint32_t _overflow = 0;

  Evt _evt[8];
  uint8_t _evt_head = 0, _evt_tail = 0, _evt_count = 0;
};

// ===========================================================================
// AirLink
// ===========================================================================

void AirLink::begin() {
  if (_initialized) return;

  // boards without PSRAM cannot run the bridge
  if (!psramFound()) {
    Serial.println("AIRLINK: needs a board with PSRAM - bridge not started");
    return;
  }
  _link = new PhoneLink();
  if (!_link) { Serial.println("AIRLINK: no memory - bridge not started"); return; }
  if (!_link->allocBuffers()) {
    Serial.println("AIRLINK: no memory for peer queues");
    delete _link; _link = NULL; return;
  }

  resolveChannels();

  uint32_t pin = _mesh.getBlePin();
  if (!_link->begin(pin)) {
    Serial.println("AIRLINK: no memory - bridge not started");
    delete _link; _link = NULL; return;
  }

  _initialized = true;
  Serial.printf("AIRLINK: bridge up (public %s, channels '%s'), up to %d phones\n",
                _mesh.airlinkPublic() ? "on" : "off",
                _mesh.airlinkChannels(), (int) AIRLINK_MAX_PHONES);
}

void AirLink::end() {
  if (!_initialized) return;
  if (_link) { _link->stop(); delete _link; _link = NULL; }
  _n_connected = 0;
  for (auto& p : _phones) p = PhoneSlot();
  _initialized = false;
}

// Resolve airlink.public + airlink.channels (#hashtags) to the 1-byte channel
// hashes we filter group packets on. The public channel's hash is fixed; a
// hashtag channel's hash is the first byte of sha256(secret), and a hashtag's
// secret is derived from its text (MeshCore's deterministic hashtag convention).
void AirLink::resolveChannels() {
  _n_sel = 0;
  if (_mesh.airlinkPublic()) _sel_hash[_n_sel++] = PUBLIC_CHANNEL_HASH;

  const char* s = _mesh.airlinkChannels();
  char tag[33];
  while (*s && _n_sel < (1 + AIRLINK_MAX_CH)) {
    while (*s == ',' || *s == ' ' || *s == '#') s++;       // skip separators + leading '#'
    int n = 0;
    while (*s && *s != ',' && n < 32) tag[n++] = *s++;
    if (n == 0) continue;
    tag[n] = 0;
    // secret := sha256(tag), hash := sha256(secret)[0]  (1-byte channel hash)
    uint8_t secret[32], hash[1];
    mesh::Utils::sha256(secret, sizeof(secret), (const uint8_t*) tag, n);
    mesh::Utils::sha256(hash, 1, secret, sizeof(secret));
    _sel_hash[_n_sel++] = hash[0];
  }
}

bool AirLink::channelSelected(uint8_t hash) const {
  for (uint8_t i = 0; i < _n_sel; i++) if (_sel_hash[i] == hash) return true;
  return false;
}

// Which packets cross the bridge? Direct messages, their acks and path returns
// always cross (the phone that owns the key decrypts them). Group text/data cross
// only on a selected channel, and never when their entry repeater is blocked by
// the spam filter. Adverts and control traffic do not cross.
bool AirLink::shouldCross(const mesh::Packet* pkt) const {
  switch (pkt->getPayloadType()) {
    case PAYLOAD_TYPE_TXT_MSG:
    case PAYLOAD_TYPE_REQ:
    case PAYLOAD_TYPE_RESPONSE:
    case PAYLOAD_TYPE_PATH:
    case PAYLOAD_TYPE_ACK:
      return true;
    case PAYLOAD_TYPE_GRP_TXT:
    case PAYLOAD_TYPE_GRP_DATA:
      if (pkt->payload_len < 1) return false;
      if (!channelSelected(pkt->payload[0])) return false;
      return true;
    default:
      return false;   // adverts, trace, control, multipart, raw, anon_req
  }
}

// LoRa TX/RX hook (called from MeshOSFull::logTx / logRx). Relay qualifying
// packets to the phones, suppressing echoes of what we ourselves just injected.
void AirLink::sendPacket(mesh::Packet* packet) {
  if (!_initialized || !_link || _link->connected() == 0) return;
  if (!shouldCross(packet)) {
    // withheld channel packet from a blocked entry repeater is worth counting
    if ((packet->getPayloadType() == PAYLOAD_TYPE_GRP_TXT ||
         packet->getPayloadType() == PAYLOAD_TYPE_GRP_DATA) &&
        _mesh.isEntryBlockedPkt(packet)) _st.entry_blocked++;
    return;
  }
  if (_mesh.isEntryBlockedPkt(packet)) { _st.entry_blocked++; return; }

  // de-dup: if we already injected this packet from a phone, it is our own echo
  if (_seen_packets.wasSeen(packet)) { _st.echo_lora++; return; }

  relayToPhones(packet);
}

void AirLink::relayToPhones(mesh::Packet* pkt) {
  uint8_t buf[AL_FRAME_MAX];
  int len = pkt->writeTo(&buf[1]);
  if (len <= 0 || len > AL_FRAME_MAX - 1) { _st.dropped++; return; }
  buf[0] = (uint8_t) len;                 // simple length prefix
  _link->broadcast(buf, (uint16_t)(len + 1));
  _st.to_phones++;
}

// phone -> LoRa: queue a phone-supplied packet into the mesh inbound path, so it
// floods out over LoRa exactly like a received packet (deduped along the way).
void AirLink::onPacketReceived(mesh::Packet* packet) {
  handleReceivedPacket(packet);   // BridgeBase: wasSeen? -> queueInbound(delay) else free
}

void AirLink::drainPhones() {
  PhoneLink::InFrame f;
  while (_link->nextFrame(f)) {
    _st.rx++;

    // a bare companion probe -> answer with the repeater banner, do not bridge
    if (f.len >= 1 && f.buf[0] == COMPANION_PROBE && f.len <= 4) {
      Serial.println("APP: companion-protocol frame received - answering with the repeater banner");
      uint8_t out[1 + sizeof(REPEATER_BANNER)];
      out[0] = (uint8_t) sizeof(REPEATER_BANNER) - 1;
      memcpy(&out[1], REPEATER_BANNER, out[0]);
      _link->broadcast(out, out[0] + 1);
      continue;
    }

    // otherwise it is a length-prefixed mesh Packet the phone wants on the mesh
    if (f.len < 2) { _st.dropped++; continue; }
    uint8_t plen = f.buf[0];
    if (plen == 0 || plen > f.len - 1) { _st.dropped++; continue; }

    int slot = findSlot(f.conn_id);
    if (slot < 0 || !rateOk(_phones[slot])) { _st.limited++; continue; }

    mesh::Packet* pkt = _mesh.getPacketManager()->allocNew();
    if (!pkt) { _st.dropped++; continue; }
    if (!pkt->readFrom(&f.buf[1], plen)) { _mesh.getPacketManager()->free(pkt); _st.dropped++; continue; }

    // relay to the *other* phones too (phone <-> phone), then inject to LoRa
    if (_link->connected() > 1) { relayToPhonesRaw(f.buf, (uint16_t)(plen + 1)); _st.relayed++; }

    if (_seen_packets.wasSeen(pkt)) { _st.dup++; _mesh.getPacketManager()->free(pkt); continue; }
    onPacketReceived(pkt);   // -> queueInbound, floods on LoRa
    _st.to_lora++;
  }
}

// (helper kept inline-ish: rebroadcast an already-framed buffer to the phones)
void AirLink::relayToPhonesRaw(const uint8_t* framed, uint16_t len) {
  _link->broadcast(framed, len);
}

bool AirLink::rateOk(PhoneSlot& p) {
  uint32_t now = millis();
  if (now - p.window_start >= AL_RATE_WINDOW) { p.window_start = now; p.sends = 0; }
  if (p.sends >= AL_RATE_MAX) return false;
  p.sends++;
  return true;
}

int AirLink::findSlot(uint16_t conn) const {
  for (int i = 0; i < AIRLINK_MAX_PHONES; i++)
    if (_phones[i].active && _phones[i].conn_id == conn) return i;
  return -1;
}

void AirLink::onPhoneJoined(uint16_t conn, uint16_t mtu, uint16_t interval, uint16_t timeout) {
  int slot = -1;
  for (int i = 0; i < AIRLINK_MAX_PHONES; i++) if (!_phones[i].active) { slot = i; break; }
  if (slot < 0) {
    Serial.printf("AIRLINK: phone on conn %u not served - all %d slots in use\n", conn, (int)AIRLINK_MAX_PHONES);
    return;
  }
  PhoneSlot& p = _phones[slot];
  p = PhoneSlot();
  p.active = true; p.conn_id = conn; p.mtu = mtu; p.link_interval = interval; p.timeout = timeout;
  p.window_start = millis();
  _n_connected++;
  _st.joins++;
  if (_n_connected > _st.peak_phones) _st.peak_phones = _n_connected;
  Serial.printf("AIRLINK: phone joined (conn %u, mtu %u, link %u x 1.25 ms, timeout %u0 ms), %d connected\n",
                conn, mtu, interval, timeout, (int)_n_connected);
  Serial.printf("APP: phone connected, mtu %u\n", mtu);
}

void AirLink::onPhoneLeft(uint16_t conn, uint8_t reason, int8_t last_dbm, int8_t weakest_dbm) {
  int slot = findSlot(conn);
  if (slot < 0) return;
  _phones[slot].active = false;
  if (_n_connected) _n_connected--;
  _st.left++;
  _st.last_reason = reason;
  Serial.printf("AIRLINK: phone left (conn %u, reason 0x%02x, last %d dBm, weakest %d), %d connected\n",
                conn, reason, last_dbm, weakest_dbm, (int)_n_connected);
  Serial.println("APP: phone disconnected");
}

void AirLink::loop() {
  if (!_initialized || !_link) return;

  // service connect/disconnect events raised by the BLE stack
  PhoneLink::Evt e;
  while (_link->nextEvent(e)) {
    if (e.kind == 1) onPhoneJoined(e.conn_id, e.mtu, 0, 0);   // interval/timeout filled on negotiation
    else if (e.kind == 2) onPhoneLeft(e.conn_id, e.reason, 0, 0);
  }

  drainPhones();
}

String AirLink::statusLine() {
  if (!_initialized) {
    return _mesh.airlinkEnabled() ? String("> off (not on this board: needs PSRAM)")
                                  : String("> off");
  }
  char buf[320];
  snprintf(buf, sizeof(buf),
    "> on, %d phone(s), public %s, channels %s | to LoRa %lu, to phones %lu, relayed %lu, "
    "rx %lu (dup %lu), confirmed %lu, left %lu (last 0x%02x), capped %lu, dropped %lu",
    (int)_n_connected,
    _mesh.airlinkPublic() ? "on" : "off",
    _mesh.airlinkChannels()[0] ? _mesh.airlinkChannels() : "-",
    (unsigned long)_st.to_lora, (unsigned long)_st.to_phones, (unsigned long)_st.relayed,
    (unsigned long)_st.rx, (unsigned long)_st.dup, (unsigned long)_st.confirmed,
    (unsigned long)_st.left, _st.last_reason, (unsigned long)_st.limited, (unsigned long)_st.dropped);
  return String(buf);
}

String AirLink::statsJson() {
  char buf[480];
  snprintf(buf, sizeof(buf),
    "\"peers\":%d,\"rx\":%lu,\"dup\":%lu,\"relayed\":%lu,\"to_lora\":%lu,\"to_phones\":%lu,"
    "\"limited\":%lu,\"dropped\":%lu,\"confirmed\":%lu,\"rewrapped\":%lu,\"echo_lora\":%lu,"
    "\"echo_heard\":%lu,\"entry_blocked\":%lu,\"left\":%lu,\"last_reason\":%u,"
    "\"public\":%s,\"chans\":\"%s\"",
    (int)_n_connected,
    (unsigned long)_st.rx, (unsigned long)_st.dup, (unsigned long)_st.relayed,
    (unsigned long)_st.to_lora, (unsigned long)_st.to_phones, (unsigned long)_st.limited,
    (unsigned long)_st.dropped, (unsigned long)_st.confirmed, (unsigned long)_st.rewrapped,
    (unsigned long)_st.echo_lora, (unsigned long)_st.echo_heard, (unsigned long)_st.entry_blocked,
    (unsigned long)_st.left, _st.last_reason,
    _mesh.airlinkPublic() ? "true" : "false", _mesh.airlinkChannels());
  return String(buf);
}

#endif // ESP32
