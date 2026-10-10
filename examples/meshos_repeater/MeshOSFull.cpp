#include "MeshOSFull.h"

static const char* meshos_hex_chars = "0123456789ABCDEF";

static void toHex(const uint8_t* src, uint8_t len, char* dest) {
  for (uint8_t i = 0; i < len; i++) {
    *dest++ = meshos_hex_chars[src[i] >> 4];
    *dest++ = meshos_hex_chars[src[i] & 0x0F];
  }
  *dest = 0;
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
  return false;
}

void MeshOSFull::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  if (handleFilterCommand(command, reply)) return;
  if (handleConfigCommand(command, reply)) return;
  MyMesh::handleCommand(sender_timestamp, command, reply);
}
