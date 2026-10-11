#include "MeshOSFull.h"
#include "MiniJson.h"
#include "helpers/AbstractBridge.h"

using namespace meshos;

static uint8_t parseOnOff(const char* arg, uint8_t current) {
  arg = skipSpaces(arg);
  if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) return 1;
  if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) return 0;
  return current;
}

static const char* onOff(bool on) { return on ? "on" : "off"; }

// ===========================================================================
// Settings file
// ===========================================================================

void MeshOSFull::saveConfig() {
  if (!_fs) return;
  _fs->remove(MESHOS_NET_FILE);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File file = _fs->open(MESHOS_NET_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File file = _fs->open(MESHOS_NET_FILE, "w");
#else
  File file = _fs->open(MESHOS_NET_FILE, "w", true);
#endif
  if (file) {
    _prefs.saveSerial(file);
    file.close();
  }
}

void MeshOSFull::beginFull(FILESYSTEM* fs) {
  _fs = fs;
  bool found = fs->exists(MESHOS_NET_FILE);
  if (found) {
    File file = fs->open(MESHOS_NET_FILE);
    if (file) {
      _prefs.loadSerial(file);
      file.close();
    }
  }
  _filter.sanitise();
  loadNames();

  _first_run = !_prefs.configured && !found;
  if (_first_run) inheritCompanionWifi();
}

// On the first boot after replacing a MeshOS companion image, carry over its
// WiFi settings. The companion saved them in /com_prefs, in the same
// ConfigSerializer JSON format, so the keys can be read straight out of it.
void MeshOSFull::inheritCompanionWifi() {
  if (!_fs->exists("/com_prefs")) return;
  File file = _fs->open("/com_prefs");
  if (!file) return;

  String json;
  while (file.available() && json.length() < 2048) json += (char) file.read();
  file.close();

  char ssid[sizeof(_prefs.wifi_ssid)] = {};
  char pwd[sizeof(_prefs.wifi_pwd)] = {};
  if (!extractJsonStr(json.c_str(), "wifi_ssid", ssid, sizeof(ssid)) || !ssid[0]) return;
  extractJsonStr(json.c_str(), "wifi_pwd", pwd, sizeof(pwd));

  StrHelper::strncpy(_prefs.wifi_ssid, ssid, sizeof(_prefs.wifi_ssid));
  StrHelper::strncpy(_prefs.wifi_pwd, pwd, sizeof(_prefs.wifi_pwd));
  _prefs.transport = TRANSPORT_WIFI;   // the companion was running over WiFi
  _prefs.inherited = 1;
  saveConfig();                        // so this only happens once
  Serial.printf("NET: first run - inherited WiFi '%s' from the companion prefs, transport %s\n",
                _prefs.wifi_ssid, transportName());
}

bool MeshOSFull::extractJsonStr(const char* json, const char* key, char* out, size_t out_size) {
  return minijson::getString(json, key, out, out_size);
}

// ===========================================================================
// CLI
// ===========================================================================

void MeshOSFull::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  if (handleFilterCommand(command, reply)) return;
  if (handleConfigCommand(command, reply)) return;
  if (handleTraceCommand(command, reply)) return;
  MyMesh::handleCommand(sender_timestamp, command, reply);
}

// block.channel / unblock.channel / block.entry / unblock.entry / chan.hash
bool MeshOSFull::handleFilterCommand(char* command, char* reply) {
  char* arg;

  bool block_channel = (arg = afterPrefix(command, "block.channel ")) != NULL;
  if (block_channel || (arg = afterPrefix(command, "unblock.channel "))) {
    uint8_t hash;
    if (!parseChannelHash(arg, hash)) {
      strcpy(reply, "Err - give a hex hash (00-FF) or a #hashtag");
    } else if (block_channel) {
      bool ok = _filter.blockChannel(hash);
      if (ok) saveConfig();
      sprintf(reply, ok ? "OK - channel %02X blocked (not repeated)" : "Err - block list full", hash);
    } else {
      bool ok = _filter.unblockChannel(hash);
      if (ok) saveConfig();
      sprintf(reply, ok ? "OK - channel %02X unblocked" : "Err - channel %02X was not blocked", hash);
    }
    return true;
  }

  bool block_entry = (arg = afterPrefix(command, "block.entry ")) != NULL;
  if (block_entry || (arg = afterPrefix(command, "unblock.entry "))) {
    uint8_t id[FILTER_MAX_ENTRY_LEN], len;
    if (!parseEntryId(arg, id, len)) {
      strcpy(reply, "Err - give the repeater's ID in hex, 1-3 bytes (e.g. A0 or A07E)");
      return true;
    }
    char hex[FILTER_ENTRY_HEX_LEN];
    bytesToHex(id, len, hex);
    if (block_entry) {
      bool ok = _filter.addEntry(id, len);
      if (ok) saveConfig();
      sprintf(reply, ok ? "OK - channel messages entering via %s are not repeated" : "Err - entry block list full", hex);
    } else {
      bool ok = _filter.removeEntry(id, len);
      if (ok) saveConfig();
      sprintf(reply, ok ? "OK - entry %s unblocked" : "Err - entry %s was not blocked", hex);
    }
    return true;
  }

  if (strcmp(command, "chan.hash") == 0) {
    sprintf(reply, "> public %02X", PUBLIC_CHANNEL_HASH);
    return true;
  }
  return false;
}

// Network / web / BLE / observer / AirLink settings. Most apply after a reboot.
bool MeshOSFull::handleConfigCommand(char* command, char* reply) {
  char* arg;

  // ---- set ----
  if ((arg = afterPrefix(command, "set transport "))) {
    if (strcmp(arg, "ble") == 0) {
      _prefs.transport = TRANSPORT_BLE;
    } else if (strcmp(arg, "wifi") == 0) {
      _prefs.transport = TRANSPORT_WIFI;
    } else {
      strcpy(reply, "Err - transport must be ble or wifi");
      return true;
    }
    saveConfig();
    sprintf(reply, "OK - transport %s, reboot to apply", transportName());
    return true;
  }
  if ((arg = afterPrefix(command, "set wifi.ssid "))) {
    StrHelper::strncpy(_prefs.wifi_ssid, arg, sizeof(_prefs.wifi_ssid));
    saveConfig();
    strcpy(reply, _prefs.wifi_ssid[0] ? "OK" : "OK - ssid cleared (wired only)");
    return true;
  }
  if ((arg = afterPrefix(command, "set wifi.pwd "))) {
    StrHelper::strncpy(_prefs.wifi_pwd, arg, sizeof(_prefs.wifi_pwd));
    saveConfig();
    strcpy(reply, "OK");
    return true;
  }
  if ((arg = afterPrefix(command, "set web.password ")) || (arg = afterPrefix(command, "web.password.change "))) {
    StrHelper::strncpy(_prefs.web_pwd, arg, sizeof(_prefs.web_pwd));
    saveConfig();
    strcpy(reply, "OK - web password changed, sign in again");
    return true;
  }
  if (strcmp(command, "web.password.clear") == 0) {
    _prefs.web_pwd[0] = 0;
    saveConfig();
    strcpy(reply, "OK - web password changed, sign in again");
    return true;
  }
  if ((arg = afterPrefix(command, "set ble.pin "))) {
    _prefs.ble_pin = atoi(arg);
    saveConfig();
    sprintf(reply, "OK - Bluetooth PIN set to %06lu, reboot to apply. Phones paired with the old PIN must forget the repeater and pair again",
            (unsigned long) _prefs.ble_pin);
    return true;
  }
  if ((arg = afterPrefix(command, "set observer "))) {
    _prefs.observer = parseOnOff(arg, _prefs.observer);
    saveConfig();
    sprintf(reply, "OK - observer %s", onOff(_prefs.observer));
    return true;
  }
  // (airlink.public / airlink.channels must be tested before plain "set airlink ")
  if ((arg = afterPrefix(command, "set airlink.public "))) {
    _prefs.airlink_pub = parseOnOff(arg, _prefs.airlink_pub);
    saveConfig();
    strcpy(reply, "OK");
    return true;
  }
  if ((arg = afterPrefix(command, "set airlink.channels "))) {
    StrHelper::strncpy(_prefs.airlink_ch, arg, sizeof(_prefs.airlink_ch));
    saveConfig();
    strcpy(reply, "OK");
    return true;
  }
  if ((arg = afterPrefix(command, "set airlink "))) {
    _prefs.airlink = parseOnOff(arg, _prefs.airlink);
    saveConfig();
    sprintf(reply, "OK - airlink %s, reboot to apply", onOff(_prefs.airlink));
    return true;
  }

  // ---- get ----
  if (strcmp(command, "get transport") == 0) {
    sprintf(reply, "> %s", transportName());
    return true;
  }
  if (strcmp(command, "get web") == 0) {
    sprintf(reply, "> %s", webPwdSet() ? "password set" : "no password");
    return true;
  }
  if (strcmp(command, "get ble.pin") == 0) {
    sprintf(reply, "> %06lu", (unsigned long) getBlePin());
    return true;
  }
  if (strcmp(command, "get wifi.ssid") == 0) {
    sprintf(reply, "> %s", _prefs.wifi_ssid);
    return true;
  }
  if (strcmp(command, "get observer") == 0) {
    sprintf(reply, "> %s", onOff(_prefs.observer));
    return true;
  }
  if (strcmp(command, "get airlink") == 0) {
    sprintf(reply, "> %s", onOff(_prefs.airlink));
    return true;
  }

  // ---- setup wizard ----
  if (strcmp(command, "setup.done") == 0) {
    _prefs.setup_done = 1;
    _prefs.configured = 1;
    saveConfig();
    strcpy(reply, "OK - setup complete");
    return true;
  }
  if (strcmp(command, "setup.reset") == 0) {
    _prefs.setup_done = 0;
    saveConfig();
    strcpy(reply, "OK - the setup wizard will show when the page next loads");
    return true;
  }

  // ---- misc ----
  if (strcmp(command, "ver.meshos") == 0) {
    sprintf(reply, "> MeshOS repeater %s", MESHOS_VERSION);
    return true;
  }
  if (strcmp(command, "graph.clear") == 0) {
    memset(_filter.chan_heard, 0, sizeof(_filter.chan_heard));
    memset(_filter.entry_heard, 0, sizeof(_filter.entry_heard));
    _filter.not_repeated = 0;
    _log_head = _log_count = 0;
    _log_seq = 0;
    strcpy(reply, "OK - graph cleared");
    return true;
  }
  return false;
}

// "trace H0,H1,..." : send a trace packet along the given 1-byte hop hashes.
bool MeshOSFull::handleTraceCommand(char* command, char* reply) {
  char* arg = afterPrefix(command, "trace ");
  if (!arg) return false;

  uint8_t hops[TRACE_MAX_HOPS];
  uint8_t hop_count = 0;
  const char* p = arg;
  while (*p && hop_count < TRACE_MAX_HOPS) {
    while (*p == ' ' || *p == ',') p++;
    int hi = hexNibble(p[0]);
    int lo = p[0] ? hexNibble(p[1]) : -1;
    if (hi < 0 || lo < 0) break;
    hops[hop_count++] = (hi << 4) | lo;
    p += 2;
  }
  if (hop_count == 0) {
    strcpy(reply, "Err - enter at least one hop hash");
    return true;
  }

  mesh::Packet* pkt = createTrace(++_trace_tag, 0, 0);
  if (!pkt) {
    strcpy(reply, "Err - packet pool empty");
    return true;
  }
  _trace_active = true;
  _trace_done = false;
  sendDirect(pkt, hops, hop_count);
  strcpy(reply, "OK");
  return true;
}

void MeshOSFull::onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                             const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len) {
  MyMesh::onTraceRecv(packet, tag, auth_code, flags, path_snrs, path_hashes, path_len);
  if (!_trace_active || tag != _trace_tag) return;

  _trace_len = path_len > TRACE_MAX_HOPS ? TRACE_MAX_HOPS : path_len;
  memcpy(_trace_hashes, path_hashes, _trace_len);
  memcpy(_trace_snrs, path_snrs, _trace_len);
  _trace_flags = flags;
  _trace_done = true;
}

// ===========================================================================
// Packet hooks
// ===========================================================================

bool MeshOSFull::allowPacketForward(const mesh::Packet* packet) {
  if (_filter.shouldDrop(packet)) return false;
  return MyMesh::allowPacketForward(packet);
}

void MeshOSFull::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
  _last_snr = snr;
  _last_rssi = rssi;
  MyMesh::logRxRaw(snr, rssi, raw, len);
}

void MeshOSFull::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);

  uint8_t flags = 0;
  if (isGroupPayload(pkt->getPayloadType())) {
    if (pkt->payload_len > 0 && _filter.isChannelBlocked(pkt->payload[0])) flags |= LOG_FLAG_CHANNEL_BLOCKED;
    else if (_filter.matchEntry(pkt) >= 0) flags |= LOG_FLAG_ENTRY_BLOCKED;
  }
  recordPacket(pkt, len, DIR_RX, flags);

  if (_prefs.observer) queueForObserver(pkt);

  _sig_time = getRTCClock()->getCurrentTime();
  _sig_direct = pkt->isRouteDirect();
  _sig_path_len = pkt->path_len;
  _sig_last_hop = pkt->path_len ? pkt->path[pkt->path_len - 1] : 0;

  if (_airlink) _airlink->sendPacket(pkt);   // AirLink decides whether phones get it
}

void MeshOSFull::logTx(mesh::Packet* pkt, int len) {
  MyMesh::logTx(pkt, len);
  recordPacket(pkt, len, DIR_TX, 0);
}

void MeshOSFull::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                              const uint8_t* app_data, size_t app_data_len) {
  MyMesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len);
  AdvertDataParser parser(app_data, app_data_len);
  if (parser.isValid() && parser.hasName()) rememberName(id.pub_key, parser.getName());
}

// ===========================================================================
// Packet log (/api/log)
// ===========================================================================

void MeshOSFull::recordPacket(mesh::Packet* pkt, int len, uint8_t dir, uint8_t flags) {
  LoggedPacket& e = _log[_log_head];
  e.seq = ++_log_seq;
  e.time = millis() / 1000;
  e.dir = dir;
  e.type = pkt->getPayloadType();
  e.route = pkt->getRouteType();
  e.len = len;
  e.hops = pkt->getPathHashCount();
  e.hash_size = pkt->getPathHashSize();
  e.snr_x4 = dir == DIR_RX ? (int8_t)(_last_snr * 4) : 0;
  e.rssi = dir == DIR_RX ? (int16_t) _last_rssi : 0;
  e.channel = (isGroupPayload(e.type) && pkt->payload_len > 0) ? pkt->payload[0] : 255;
  e.flags = flags;
  uint16_t path_bytes = min((uint16_t)(e.hops * e.hash_size), (uint16_t) sizeof(e.path));
  memcpy(e.path, pkt->path, path_bytes);

  _log_head = (_log_head + 1) % LOG_SIZE;
  if (_log_count < LOG_SIZE) _log_count++;
}

// {"e":[[seq,time,dir,type,route,len,hops,snr_x4,rssi,"path",channel,flags,hash_size],...]}
String MeshOSFull::logJson(uint32_t since) {
  String j = "{\"e\":[";
  bool first = true;
  for (uint8_t k = 0; k < _log_count; k++) {
    uint8_t idx = (_log_head + LOG_SIZE - _log_count + k) % LOG_SIZE;   // oldest first
    const LoggedPacket& e = _log[idx];
    if (e.seq <= since) continue;

    uint16_t path_bytes = min((uint16_t)(e.hops * e.hash_size), (uint16_t) sizeof(e.path));
    char path[sizeof(e.path) * 2 + 2];
    bytesToHex(e.path, path_bytes, path);

    if (!first) j += ',';
    first = false;
    j += '[';
    j += e.seq;    j += ',';
    j += e.time;   j += ',';
    j += e.dir;    j += ',';
    j += e.type;   j += ',';
    j += e.route;  j += ',';
    j += e.len;    j += ',';
    j += e.hops;   j += ',';
    j += e.snr_x4; j += ',';
    j += e.rssi;   j += ",\"";
    j += path;     j += "\",";
    j += e.channel; j += ',';
    j += e.flags;  j += ',';
    j += e.hash_size;
    j += ']';
  }
  j += "]}";
  return j;
}

// ===========================================================================
// Other web API payloads
// ===========================================================================

static void appendHeardList(String& j, const uint8_t counts[256]) {
  bool first = true;
  for (int hash = 0; hash < 256; hash++) {
    if (counts[hash] == 0) continue;
    if (!first) j += ',';
    first = false;
    j += '['; j += hash; j += ','; j += counts[hash]; j += ']';
  }
}

// {"public":17,"not_repeated":N,"blocked":[hash,...],"chans":[[hash,count],...],
//  "en":[[hash,count],...],"entry_blocked":[["A07E",hits,""],...]}
String MeshOSFull::channelsJson() {
  const FilterLists& lists = _filter.lists();
  String j = "{\"public\":";
  j += PUBLIC_CHANNEL_HASH;
  j += ",\"not_repeated\":";
  j += _filter.not_repeated;

  j += ",\"blocked\":[";
  for (uint8_t i = 0; i < _filter.channelCount(); i++) {
    if (i) j += ',';
    j += lists.channels[i];
  }

  j += "],\"chans\":[";
  appendHeardList(j, _filter.chan_heard);
  j += "],\"en\":[";
  appendHeardList(j, _filter.entry_heard);

  j += "],\"entry_blocked\":[";
  for (uint8_t i = 0; i < _filter.entryCount(); i++) {
    char hex[FILTER_ENTRY_HEX_LEN];
    bytesToHex(lists.entries[i].id, lists.entries[i].len, hex);
    if (i) j += ',';
    j += "[\""; j += hex; j += "\","; j += _filter.entry_hits[i]; j += ",\"\"]";
  }
  j += "]}";
  return j;
}

// /api/finder: nodes heard directly, from the base repeater's neighbour table.
// Names come from the names cache; type and their_snr need an active scan,
// which is not implemented, so they are fixed / null.
String MeshOSFull::finderJson() {
  uint32_t now = getRTCClock()->getCurrentTime();
  String j = "{\"n\":[";
#if MAX_NEIGHBOURS
  const NeighbourInfo* neighbours = getNeighbours();
  bool first = true;
  for (int i = 0; i < getMaxNeighbours(); i++) {
    const NeighbourInfo& nb = neighbours[i];
    if (nb.heard_timestamp == 0) continue;

    char id[16];
    mesh::Utils::toHex(id, nb.id.pub_key, 6);
    long age = (long) now - (long) nb.heard_timestamp;
    const char* name = lookupName(nb.id.pub_key);

    if (!first) j += ',';
    first = false;
    j += "{\"id\":\""; j += id; j += "\",";
    j += "\"hash\":"; j += nb.id.pub_key[0]; j += ',';
    j += "\"known\":8,";
    if (name) { j += "\"name\":\""; j += name; j += "\","; }
    else      { j += "\"name\":null,"; }
    j += "\"type\":2,";   // repeater
    j += "\"age\":"; j += age; j += ',';
    j += "\"snr\":"; j += String(nb.snr / 4.0, 1); j += ',';
    j += "\"their_snr\":null}";
  }
#endif
  j += "]}";
  return j;
}

// /api/graph: node 0 is this repeater, with the directly-heard neighbours
// around it (a one-hop view). Each node is [0,"id",8,kind,hops,snr,snr,age].
String MeshOSFull::graphJson() {
  uint32_t now = getRTCClock()->getCurrentTime();
  char self_hex[20];
  mesh::Utils::toHex(self_hex, self_id.pub_key, 6);
  String j = "{\"n\":[[0,\""; j += self_hex; j += "\",8,2,0,0,0,0]";
#if MAX_NEIGHBOURS
  const NeighbourInfo* neighbours = getNeighbours();
  for (int i = 0; i < getMaxNeighbours(); i++) {
    const NeighbourInfo& nb = neighbours[i];
    if (nb.heard_timestamp == 0) continue;
    char id[20];
    mesh::Utils::toHex(id, nb.id.pub_key, 6);
    long age = (long) now - (long) nb.heard_timestamp;
    j += ",[0,\""; j += id; j += "\",8,1,1,";
    j += nb.snr; j += ','; j += nb.snr; j += ','; j += age; j += ']';
  }
#endif
  j += "]}";
  return j;
}

// Builds the region tree in the same text form as RegionMap::printChildRegions,
// one region per '|'-separated line: indent, name, '^' for home, " F" if floods
// are allowed.
static void buildRegionTree(RegionMap& map, const RegionEntry* region, int indent, uint16_t home_id, String& out) {
  for (int i = 0; i < indent; i++) out += ' ';
  const char* name = region->name;
  if (name[0] == '#') name++;
  out += name;
  if (region->id == home_id) out += '^';
  if (!(region->flags & REGION_DENY_FLOOD)) out += " F";
  out += '|';
  for (int i = 0; i < map.getCount(); i++) {
    const RegionEntry* child = map.getByIdx(i);
    if (child->parent == region->id) buildRegionTree(map, child, indent + 1, home_id, out);
  }
}

String MeshOSFull::regionsJson() {
  RegionMap& map = getRegionMap();
  const RegionEntry* home = map.getHomeRegion();
  const RegionEntry* def = map.getDefaultRegion();
  String tree;
  buildRegionTree(map, map.getRoot(), 0, home ? home->id : 0xFFFF, tree);

  String j = "{\"tree\":\""; j += tree; j += "\",";
  j += "\"default\":\""; j += (def ? def->name : "<null>"); j += "\",";
  j += "\"home\":\""; j += (home ? home->name : "*"); j += "\"}";
  return j;
}

// {"ok":true,"flags":F,"hops":[[hash,snr_x4],...]} once a reply has arrived
String MeshOSFull::traceJson() {
  if (!_trace_done) return "{\"ok\":false}";
  _trace_active = false;
  String j = "{\"ok\":true,\"flags\":"; j += _trace_flags; j += ",\"hops\":[";
  for (uint8_t i = 0; i < _trace_len; i++) {
    if (i) j += ',';
    j += '['; j += _trace_hashes[i]; j += ','; j += (int)(int8_t) _trace_snrs[i]; j += ']';
  }
  j += "]}";
  return j;
}

String MeshOSFull::sigJson() {
  if (_sig_time == 0) return String("{\"valid\":0}");
  uint32_t age = getRTCClock()->getCurrentTime() - _sig_time;
  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"valid\":1,\"age\":%lu,\"snr\":%.2f,\"rssi\":%d,\"hop\":\"%02X\",\"hops\":%u,\"route\":\"%s\"}",
    (unsigned long) age, _last_snr, (int) _last_rssi, _sig_last_hop, _sig_path_len, _sig_direct ? "D" : "F");
  return String(buf);
}

// ===========================================================================
// Observer payloads
// ===========================================================================

// Signs `json` with this node's identity and returns it with a "sig" field
// (hex Ed25519 signature over the original text) added before the closing '}'.
String MeshOSFull::appendSignature(const String& json) {
  uint8_t sig[SIGNATURE_SIZE];
  self_id.sign(sig, (const uint8_t*) json.c_str(), json.length());
  char sig_hex[SIGNATURE_SIZE * 2 + 2];
  mesh::Utils::toHex(sig_hex, sig, sizeof(sig));

  String out = json.substring(0, json.length() - 1);
  out += ",\"sig\":\""; out += sig_hex; out += "\"}";
  return out;
}

String MeshOSFull::observerHeartbeat() {
  char id[20];
  mesh::Utils::toHex(id, self_id.pub_key, 6);
  NodePrefs* p = getNodePrefs();

  String body = "{";
  body += "\"name\":\"";      body += p->node_name;           body += "\",";
  body += "\"id\":\"";        body += id;                     body += "\",";
  body += "\"ver\":\"" FIRMWARE_VERSION "\",";
  body += "\"uptime\":";      body += millis() / 1000;        body += ',';
  body += "\"transport\":\""; body += transportName();        body += "\",";
  body += "\"blocked\":";     body += _filter.lists().channel_count; body += ',';
  body += "\"entries\":";     body += _filter.lists().entry_count;   body += ',';
  body += "\"not_repeated\":"; body += _filter.not_repeated;
  body += '}';
  return appendSignature(body);
}

// Queue a heard packet for the observer's rx topic (drops the oldest if full).
void MeshOSFull::queueForObserver(mesh::Packet* pkt) {
  QueuedRx& e = _obs_queue[(_obs_head + _obs_count) % OBSERVER_QUEUE_SIZE];
  e.time = millis() / 1000;
  e.snr_x4 = (int8_t)(_last_snr * 4);
  e.rssi = (int16_t) _last_rssi;
  e.len = pkt->writeTo(e.raw);
  if (_obs_count < OBSERVER_QUEUE_SIZE) {
    _obs_count++;
  } else {
    _obs_head = (_obs_head + 1) % OBSERVER_QUEUE_SIZE;
    _obs_dropped++;
  }
}

bool MeshOSFull::observerNextRx(String& out) {
  if (_obs_count == 0) return false;
  const QueuedRx& e = _obs_queue[_obs_head];
  _obs_head = (_obs_head + 1) % OBSERVER_QUEUE_SIZE;
  _obs_count--;

  char raw_hex[sizeof(e.raw) * 2 + 2];
  mesh::Utils::toHex(raw_hex, e.raw, e.len);
  char body[sizeof(raw_hex) + 96];
  snprintf(body, sizeof(body), "{\"t\":%lu,\"snr\":%.2f,\"rssi\":%d,\"len\":%u,\"raw\":\"%s\"}",
           (unsigned long) e.time, e.snr_x4 / 4.0, e.rssi, e.len, raw_hex);

  out = appendSignature(String(body));
  return true;
}

// ===========================================================================
// Neighbour-name cache (/meshos_names.bin: an array of NameEntry records)
// ===========================================================================

const char* MeshOSFull::lookupName(const uint8_t* pub) const {
  for (uint8_t i = 0; i < _names_count; i++) {
    if (memcmp(_names[i].pub, pub, NAME_KEY_LEN) == 0) return _names[i].name;
  }
  return NULL;
}

void MeshOSFull::rememberName(const uint8_t* pub, const char* name) {
  if (!name || !name[0]) return;

  for (uint8_t i = 0; i < _names_count; i++) {
    if (memcmp(_names[i].pub, pub, NAME_KEY_LEN) != 0) continue;
    if (strncmp(_names[i].name, name, sizeof(_names[i].name)) != 0) {
      StrHelper::strncpy(_names[i].name, name, sizeof(_names[i].name));
      _names_dirty = true;
      _names_save_at = millis() + NAMES_SAVE_DELAY_MS;
    }
    return;
  }

  // new node: append, or when full overwrite a pseudo-randomly chosen slot
  uint8_t slot = (_names_count < NAMES_MAX) ? _names_count++ : (_names[0].pub[0] % NAMES_MAX);
  memcpy(_names[slot].pub, pub, NAME_KEY_LEN);
  StrHelper::strncpy(_names[slot].name, name, sizeof(_names[slot].name));
  _names_dirty = true;
  _names_save_at = millis() + NAMES_SAVE_DELAY_MS;
}

void MeshOSFull::loadNames() {
  if (!_fs || !_fs->exists(MESHOS_NAMES_FILE)) return;
  File file = _fs->open(MESHOS_NAMES_FILE);
  if (!file) return;

  _names_count = 0;
  while (file.available() >= (int) sizeof(NameEntry) && _names_count < NAMES_MAX) {
    NameEntry& e = _names[_names_count++];
    file.read((uint8_t*) &e, sizeof(NameEntry));
    e.name[sizeof(e.name) - 1] = 0;
  }
  file.close();
  Serial.printf("NAMES: %d neighbour names loaded\n", (int) _names_count);
}

void MeshOSFull::saveNames() {
  if (!_fs) return;
#if defined(ESP32) || defined(RP2040_PLATFORM)
  File file = _fs->open(MESHOS_NAMES_FILE, "w");
#else
  _fs->remove(MESHOS_NAMES_FILE);
  File file = _fs->open(MESHOS_NAMES_FILE, FILE_O_WRITE);
#endif
  if (!file) return;
  file.write((const uint8_t*) _names, sizeof(NameEntry) * _names_count);
  file.close();
  _names_dirty = false;
  Serial.printf("NAMES: %d neighbour names saved\n", (int) _names_count);
}

// ===========================================================================
// Airtime windows
// ===========================================================================

static const unsigned long MINUTE_MS = 60000UL;

void MeshOSFull::sampleAirtime() {
  unsigned long now = millis();
  AirSample totals;
  totals.tx = getTotalAirTime();
  totals.rx = getReceiveAirTime();
  totals.sent = getNumSentFlood() + getNumSentDirect();

  if (_air_next_minute == 0) {   // first call: just record the starting totals
    _air_last = totals;
    _air_next_minute = now + MINUTE_MS;
    return;
  }
  if ((long)(now - _air_next_minute) < 0) return;

  _air_next_minute = now + MINUTE_MS;
  AirSample& bucket = _air_minutes[_air_minute_idx];
  _air_minute_idx = (_air_minute_idx + 1) % AIR_MINUTES;
  bucket.tx   = totals.tx   - _air_last.tx;
  bucket.rx   = totals.rx   - _air_last.rx;
  bucket.sent = totals.sent - _air_last.sent;
  _air_last = totals;
}

uint32_t MeshOSFull::airTx5() const {
  uint32_t sum = 0;
  for (const AirSample& s : _air_minutes) sum += s.tx;
  return sum;
}
uint32_t MeshOSFull::airRx5() const {
  uint32_t sum = 0;
  for (const AirSample& s : _air_minutes) sum += s.rx;
  return sum;
}
uint32_t MeshOSFull::airSent5() const {
  uint32_t sum = 0;
  for (const AirSample& s : _air_minutes) sum += s.sent;
  return sum;
}

// The 24 h airtime figures are estimates scaled up from the 5-minute window.
// NOTE: this multiplies by 288/5 rather than 288 (the number of 5-minute
// periods in a day), so it reads 5x lower than a straight extrapolation.
// Packets sent is the total since boot.
uint32_t MeshOSFull::airTx24() const { return airTx5() * 288 / AIR_MINUTES; }
uint32_t MeshOSFull::airRx24() const { return airRx5() * 288 / AIR_MINUTES; }
uint32_t MeshOSFull::airSent24() const { return getNumSentFlood() + getNumSentDirect(); }

void MeshOSFull::meshosTick() {
  sampleAirtime();
  if (_names_dirty && (long)(millis() - _names_save_at) >= 0) saveNames();
}
