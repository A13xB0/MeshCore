#include "MeshOSFull.h"

static const char* meshos_hex_chars = "0123456789ABCDEF";

static void toHex(const uint8_t* src, uint8_t len, char* dest) {
  for (uint8_t i = 0; i < len; i++) {
    *dest++ = meshos_hex_chars[src[i] >> 4];
    *dest++ = meshos_hex_chars[src[i] & 0x0F];
  }
  *dest = 0;
}

static uint8_t parseOnOff(const char* arg, uint8_t current) {
  while (*arg == ' ') arg++;
  if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) return 1;
  if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) return 0;
  return current;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool parseChannelHash(const char* arg, uint8_t& hash) {
  while (*arg == ' ') arg++;
  if (*arg == '#') {
    uint8_t secret[16];
    mesh::Utils::sha256(secret, sizeof(secret), (const uint8_t*) arg, strlen(arg));
    mesh::Utils::sha256(&hash, 1, secret, sizeof(secret));
    return true;
  }
  if (arg[0] == '0' && (arg[1] | 0x20) == 'x') arg += 2;
  size_t len = strlen(arg);
  if ((unsigned)(len - 1) > 1) return false;
  uint8_t v = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = arg[i];
    v <<= 4;
    if ((uint8_t)(c - '0') < 10)      v |= c - '0';
    else if ((uint8_t)(c - 'a') < 6)  v |= c - 'a' + 10;
    else if ((uint8_t)(c - 'A') < 6)  v |= c - 'A' + 10;
    else return false;
  }
  hash = v;
  return true;
}

static bool parseEntryId(const char* arg, uint8_t* id, uint8_t& len) {
  if (arg[0] == '0' && (arg[1] | 0x20) == 'x') arg += 2;
  memset(id, 0, MESHOS_MAX_ENTRY_LEN);
  int nibbles = 0;
  bool ok = *arg != 0;
  for ( ; ok && *arg; arg++) {
    if (*arg == ' ') continue;
    int v = hexNibble(*arg);
    if (v < 0 || nibbles >= MESHOS_MAX_ENTRY_LEN * 2) ok = false;
    else { id[nibbles / 2] = (id[nibbles / 2] << 4) | v; nibbles++; }
  }
  if (!ok || nibbles < 2 || (nibbles & 1)) return false;
  len = nibbles / 2;
  return true;
}

bool MeshOSFull::blockChannel(uint8_t hash) {
  if (isChannelBlocked(hash)) return true;
  if (prefs.blocked_n >= MESHOS_MAX_BLOCKED) return false;
  prefs.blocked[prefs.blocked_n++] = hash;
  saveFilter();
  return true;
}

bool MeshOSFull::unblockChannel(uint8_t hash) {
  uint8_t n = prefs.blocked_n;
  for (uint8_t i = 0; i < n; i++) {
    if (prefs.blocked[i] == hash) {
      for (uint8_t j = i; j + 1 < n; j++) prefs.blocked[j] = prefs.blocked[j + 1];
      prefs.blocked_n = n - 1;
      saveFilter();
      return true;
    }
  }
  return false;
}

bool MeshOSFull::addEntry(const uint8_t* id, uint8_t len) {
  if ((uint8_t)(len - 1) >= MESHOS_MAX_ENTRY_LEN) return false;
  for (uint8_t i = 0; i != MESHOS_MAX_ENTRIES && i < prefs.entry_n; i++) {
    if (prefs.entries[i].len == len && memcmp(prefs.entries[i].id, id, len) == 0) return true;
  }
  if (prefs.entry_n >= MESHOS_MAX_ENTRIES) return false;
  MeshOSEntry* e = &prefs.entries[prefs.entry_n];
  memset(e, 0, sizeof(*e));
  e->len = len;
  memcpy(e->id, id, len);
  entry_hits[prefs.entry_n] = 0;
  prefs.entry_n++;
  saveFilter();
  return true;
}

bool MeshOSFull::removeEntry(const uint8_t* id, uint8_t len) {
  int i = findEntry(id, len);
  if (i < 0) return false;
  uint8_t n = prefs.entry_n;
  for (uint8_t j = i; j + 1 < n; j++) {
    prefs.entries[j] = prefs.entries[j + 1];
    entry_hits[j] = entry_hits[j + 1];
  }
  prefs.entry_n = n - 1;
  memset(&prefs.entries[prefs.entry_n], 0, sizeof(MeshOSEntry));
  entry_hits[prefs.entry_n] = 0;
  saveFilter();
  return true;
}

int MeshOSFull::matchEntry(const mesh::Packet* packet) const {
  if (prefs.entry_n == 0) return -1;
  uint8_t type = packet->getPayloadType();
  if ((type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) || !packet->isRouteFlood()) return -1;
  uint8_t pl = packet->path_len;                 // low byte: hops + hash size
  uint8_t hash_size = (pl >> 6) + 1;
  if ((pl & 63) == 0 || hash_size > MESHOS_MAX_ENTRY_LEN) return -1;
  for (uint8_t i = 0; i != MESHOS_MAX_ENTRIES && i < prefs.entry_n; i++) {
    uint8_t n = prefs.entries[i].len;
    if ((uint8_t)(n - 1) >= MESHOS_MAX_ENTRY_LEN) continue;
    if (n > hash_size) n = hash_size;
    if (memcmp(prefs.entries[i].id, packet->path, n) == 0) return i;
  }
  return -1;
}

bool MeshOSFull::allowPacketForward(const mesh::Packet* packet) {
  uint8_t type = packet->getPayloadType();
  if ((type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA) && packet->payload_len > 0
      && isChannelBlocked(packet->payload[0])) {
    return false;
  }
  int idx = matchEntry(packet);
  if (idx < 0) return MyMesh::allowPacketForward(packet);
  if (entry_hits[idx] != 0xFFFFFFFF) entry_hits[idx]++;
  return false;
}

// Dispatches the MeshOS spam-filter commands. In the shipped firmware these live
// inside one big CLI/web command handler; kept standalone here during reconstruction.
bool MeshOSFull::handleFilterCommand(char* command, char* reply) {
  if (memcmp(command, "block.channel ", 14) == 0 || memcmp(command, "unblock.channel ", 16) == 0) {
    bool block = command[0] == 'b';
    char* arg = &command[block ? 14 : 16];
    uint8_t hash;
    if (!parseChannelHash(arg, hash)) {
      strcpy(reply, "Err - give a hex hash (00-FF) or a #hashtag");
      return true;
    }
    if (block) {
      sprintf(reply, blockChannel(hash) ? "OK - channel %02X blocked (not repeated)" : "Err - block list full", hash);
    } else {
      sprintf(reply, unblockChannel(hash) ? "OK - channel %02X unblocked" : "Err - channel %02X was not blocked", hash);
    }
    return true;
  }

  if (memcmp(command, "block.entry ", 12) == 0 || memcmp(command, "unblock.entry ", 14) == 0) {
    bool block = command[0] == 'b';
    char* arg = &command[block ? 12 : 14];
    uint8_t id[MESHOS_MAX_ENTRY_LEN], len;
    if (!parseEntryId(arg, id, len)) {
      strcpy(reply, "Err - give the repeater's ID in hex, 1-3 bytes (e.g. A0 or A07E)");
      return true;
    }
    char hex[MESHOS_MAX_ENTRY_LEN * 2 + 2];
    toHex(id, len, hex);
    if (block) {
      sprintf(reply, addEntry(id, len) ? "OK - channel messages entering via %s are not repeated" : "Err - entry block list full", hex);
    } else {
      sprintf(reply, removeEntry(id, len) ? "OK - entry %s unblocked" : "Err - entry %s was not blocked", hex);
    }
    return true;
  }

  if (strcmp(command, "chan.hash") == 0) {
    sprintf(reply, "> public %02X", PUBLIC_CHANNEL_HASH);
    return true;
  }
  return false;
}

void MeshOSFull::saveConfig() {
  if (!_net_fs) return;
  _net_fs->remove(MESHOS_NET_FILE);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File file = _net_fs->open(MESHOS_NET_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File file = _net_fs->open(MESHOS_NET_FILE, "w");
#else
  File file = _net_fs->open(MESHOS_NET_FILE, "w", true);
#endif
  if (file) {
    prefs.saveSerial(file);
    file.close();
  }
}

void MeshOSFull::beginFull(FILESYSTEM* fs) {
  _net_fs = fs;
  if (fs->exists(MESHOS_NET_FILE)) {
    File file = fs->open(MESHOS_NET_FILE);
    if (file) {
      prefs.loadSerial(file);
      file.close();
    }
  }
  if (prefs.blocked_n > MESHOS_MAX_BLOCKED) prefs.blocked_n = MESHOS_MAX_BLOCKED;
  if (prefs.entry_n > MESHOS_MAX_ENTRIES) prefs.entry_n = MESHOS_MAX_ENTRIES;
}

// Network/web/radio-front-end settings of the MeshOS repeater. These apply after a reboot.
bool MeshOSFull::handleConfigCommand(char* command, char* reply) {
  if (memcmp(command, "set transport ", 14) == 0) {
    char* arg = &command[14];
    while (*arg == ' ') arg++;
    if (strcmp(arg, "ble") == 0) prefs.transport = 0;
    else if (strcmp(arg, "wifi") == 0) prefs.transport = 2;
    else { strcpy(reply, "Err - transport must be ble or wifi"); return true; }
    saveConfig();
    sprintf(reply, "OK - transport %s, reboot to apply", prefs.transport == 2 ? "wifi" : "ble");
    return true;
  }
  if (memcmp(command, "set wifi.ssid ", 14) == 0) {
    char* arg = &command[14];
    while (*arg == ' ') arg++;
    StrHelper::strncpy(prefs.wifi_ssid, arg, sizeof(prefs.wifi_ssid));
    saveConfig();
    strcpy(reply, prefs.wifi_ssid[0] ? "OK" : "OK - ssid cleared (wired only)");
    return true;
  }
  if (memcmp(command, "set wifi.pwd ", 13) == 0) {
    char* arg = &command[13];
    while (*arg == ' ') arg++;
    StrHelper::strncpy(prefs.wifi_pwd, arg, sizeof(prefs.wifi_pwd));
    saveConfig();
    strcpy(reply, "OK");
    return true;
  }
  if (memcmp(command, "set web.pwd ", 12) == 0) {
    char* arg = &command[12];
    while (*arg == ' ') arg++;
    StrHelper::strncpy(prefs.web_pwd, arg, sizeof(prefs.web_pwd));
    saveConfig();
    strcpy(reply, "OK");
    return true;
  }
  if (memcmp(command, "set ble.pin ", 12) == 0) {
    prefs.ble_pin = atoi(&command[12]);
    saveConfig();
    sprintf(reply, "OK - pin %06u, reboot to apply", prefs.ble_pin);
    return true;
  }
  if (memcmp(command, "set observer ", 13) == 0) {
    prefs.observer = parseOnOff(&command[13], prefs.observer);
    saveConfig();
    sprintf(reply, "OK - observer %s", prefs.observer ? "on" : "off");
    return true;
  }
  if (memcmp(command, "set airlink.public ", 19) == 0) {
    prefs.airlink_pub = parseOnOff(&command[19], prefs.airlink_pub);
    saveConfig();
    strcpy(reply, "OK");
    return true;
  }
  if (memcmp(command, "set airlink.channels ", 21) == 0) {
    char* arg = &command[21];
    while (*arg == ' ') arg++;
    StrHelper::strncpy(prefs.airlink_ch, arg, sizeof(prefs.airlink_ch));
    saveConfig();
    strcpy(reply, "OK");
    return true;
  }
  if (memcmp(command, "set airlink ", 12) == 0) {
    prefs.airlink = parseOnOff(&command[12], prefs.airlink);
    saveConfig();
    sprintf(reply, "OK - airlink %s, reboot to apply", prefs.airlink ? "on" : "off");
    return true;
  }
  if (strcmp(command, "get transport") == 0) {
    sprintf(reply, "> %s", prefs.transport == 2 ? "wifi" : "ble");
    return true;
  }
  if (strcmp(command, "get wifi.ssid") == 0) {
    sprintf(reply, "> %s", prefs.wifi_ssid);
    return true;
  }
  if (strcmp(command, "get observer") == 0) {
    sprintf(reply, "> %s", prefs.observer ? "on" : "off");
    return true;
  }
  if (strcmp(command, "get airlink") == 0) {
    sprintf(reply, "> %s", prefs.airlink ? "on" : "off");
    return true;
  }
  return false;
}

void MeshOSFull::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  if (handleFilterCommand(command, reply)) return;
  if (handleConfigCommand(command, reply)) return;
  MyMesh::handleCommand(sender_timestamp, command, reply);
}

// /api/channels for the web UI: blocked lists + which channels/entry repeaters
// have been heard, built from the reconstructed filter state.
String MeshOSFull::channelsJson() {
  String j = "{\"public\":";
  j += PUBLIC_CHANNEL_HASH;
  j += ",\"not_repeated\":";
  j += not_repeated;

  j += ",\"blocked\":[";
  for (uint8_t i = 0; i < prefs.blocked_n && i < MESHOS_MAX_BLOCKED; i++) {
    if (i) j += ',';
    j += prefs.blocked[i];
  }

  j += "],\"chans\":[";
  bool first = true;
  for (int h = 0; h < 256; h++) {
    if (chan_heard[h] == 0) continue;
    if (!first) j += ',';
    j += '['; j += h; j += ','; j += chan_heard[h]; j += ']';
    first = false;
  }

  j += "],\"en\":[";
  first = true;
  for (int h = 0; h < 256; h++) {
    if (entry_heard[h] == 0) continue;
    if (!first) j += ',';
    j += '['; j += h; j += ','; j += entry_heard[h]; j += ']';
    first = false;
  }

  j += "],\"entry_blocked\":[";
  for (uint8_t i = 0; i < prefs.entry_n && i < MESHOS_MAX_ENTRIES; i++) {
    char hex[MESHOS_MAX_ENTRY_LEN * 2 + 2];
    toHex(prefs.entries[i].id, prefs.entries[i].len, hex);
    if (i) j += ',';
    j += "[\""; j += hex; j += "\","; j += entry_hits[i]; j += ",\"\"]";
  }
  j += "]}";
  return j;
}

void MeshOSFull::recordPkt(mesh::Packet* pkt, int len, uint8_t dir, uint8_t fl) {
  PktLog& e = _log[_log_head];
  e.seq = ++_log_seq;
  e.t = millis() / 1000;
  e.dir = dir;
  e.type = pkt->getPayloadType();
  e.route = pkt->getRouteType();
  e.len = len;
  e.hops = pkt->path_len & 63;
  e.hs = (pkt->path_len >> 6) + 1;
  e.snr4 = dir == 0 ? (int8_t)(_last_snr * 4) : 0;
  e.rssi = dir == 0 ? (int16_t)_last_rssi : 0;
  bool grp = (e.type == PAYLOAD_TYPE_GRP_TXT || e.type == PAYLOAD_TYPE_GRP_DATA);
  e.ch = (grp && pkt->payload_len > 0) ? pkt->payload[0] : 255;
  e.fl = fl;
  uint16_t n = (uint16_t)e.hops * e.hs;
  if (n > sizeof(e.path)) n = sizeof(e.path);
  memcpy(e.path, pkt->path, n);
  _log_head = (_log_head + 1) % LOG_SIZE;
  if (_log_count < LOG_SIZE) _log_count++;
}

void MeshOSFull::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);
  uint8_t fl = 0;
  uint8_t type = pkt->getPayloadType();
  if (type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA) {
    if (pkt->payload_len > 0 && isChannelBlocked(pkt->payload[0])) fl |= 2;
    else if (matchEntry(pkt) >= 0) fl |= 1;
  }
  recordPkt(pkt, len, 0, fl);
}

void MeshOSFull::logTx(mesh::Packet* pkt, int len) {
  MyMesh::logTx(pkt, len);
  recordPkt(pkt, len, 1, 0);
}

String MeshOSFull::logJson(uint32_t since) {
  String j = "{\"e\":[";
  bool first = true;
  for (uint8_t k = 0; k < _log_count; k++) {
    uint8_t idx = (_log_head + LOG_SIZE - _log_count + k) % LOG_SIZE;
    PktLog& e = _log[idx];
    if (e.seq <= since) continue;
    uint16_t n = (uint16_t)e.hops * e.hs;
    if (n > sizeof(e.path)) n = sizeof(e.path);
    char path[sizeof(e.path) * 2 + 2];
    toHex(e.path, n, path);
    if (!first) j += ',';
    j += '['; j += e.seq; j += ','; j += e.t; j += ','; j += e.dir; j += ','; j += e.type;
    j += ','; j += e.route; j += ','; j += e.len; j += ','; j += e.hops; j += ','; j += e.snr4;
    j += ','; j += e.rssi; j += ",\""; j += path; j += "\","; j += e.ch; j += ','; j += e.fl;
    j += ','; j += e.hs; j += ']';
    first = false;
  }
  j += "]}";
  return j;
}
