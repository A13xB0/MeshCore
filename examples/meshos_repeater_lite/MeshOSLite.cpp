#include "MeshOSLite.h"

const char* lite_hex_chars = "0123456789ABCDEF";

static void liteToHex(const uint8_t* src, uint8_t len, char* dest) {
  for (uint8_t i = 0; i < len; i++) {
    *dest++ = lite_hex_chars[src[i] >> 4];
    *dest++ = lite_hex_chars[src[i] & 0x0F];
  }
  *dest = 0;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "#name" -> hashtag channel hash, or a hex hash "A5" / "0xA5"
static bool parseChannelHash(const char* arg, uint8_t& hash) {
  while (*arg == ' ') arg++;
  if (*arg == '#') {
    uint8_t secret[16];
    mesh::Utils::sha256(secret, sizeof(secret), (const uint8_t *) arg, strlen(arg));
    mesh::Utils::sha256(&hash, 1, secret, sizeof(secret));
    return true;
  }
  if (arg[0] == '0' && (arg[1] | 0x20) == 'x') arg += 2;
  size_t len = strlen(arg);
  if ((unsigned)(len - 1) > 1) return false;   // one or two hex digits (one byte)
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

// a repeater ID prefix as hex: "A0" or "A07E" (optional 0x), 1..3 bytes -> id/len
static bool parseEntryId(const char* arg, uint8_t* id, uint8_t& len) {
  if (arg[0] == '0' && (arg[1] | 0x20) == 'x') arg += 2;
  memset(id, 0, LITE_MAX_ENTRY_LEN);
  int nibbles = 0;
  bool ok = *arg != 0;
  for ( ; ok && *arg; arg++) {
    if (*arg == ' ') continue;
    int v = hexNibble(*arg);
    if (v < 0 || nibbles >= LITE_MAX_ENTRY_LEN * 2) {
      ok = false;
    } else {
      id[nibbles / 2] = (id[nibbles / 2] << 4) | v;
      nibbles++;
    }
  }
  if (!ok || nibbles < 2 || (nibbles & 1)) return false;
  len = nibbles / 2;
  return true;
}

// lists the most-heard hashes, busiest first: "A5 12, 11 3, ..."
static int formatTopHeard(const uint8_t* counts, char* dest, int dest_len) {
  uint8_t done[256];
  memset(done, 0, sizeof(done));
  *dest = 0;
  if (dest_len < 11) return 0;

  int len = 0, n = 0;
  while (true) {
    int best = -1;
    for (int i = 0; i < 256; i++) {
      if (done[i] || counts[i] == 0) continue;
      if (best < 0 || counts[i] > counts[best]) best = i;
    }
    if (best < 0) break;
    done[best] = 1;

    len += snprintf(&dest[len], dest_len - len, "%s%02X %u", n ? ", " : "", best, counts[best]);
    n++;
    if (len + 10 >= dest_len) break;
  }
  return n;
}

bool MeshOSLite::blockChannel(uint8_t hash) {
  if (isChannelBlocked(hash)) return true;             // already blocked
  if (lite.blocked_n >= LITE_MAX_BLOCKED) return false;  // list full
  lite.blocked[lite.blocked_n++] = hash;
  saveLite();
  return true;
}

bool MeshOSLite::unblockChannel(uint8_t hash) {
  uint8_t n = lite.blocked_n;
  for (uint8_t i = 0; i < n; i++) {
    if (lite.blocked[i] == hash) {
      for (uint8_t j = i; j + 1 < n; j++) lite.blocked[j] = lite.blocked[j + 1];
      lite.blocked_n = n - 1;
      saveLite();
      return true;
    }
  }
  return false;
}

bool MeshOSLite::addEntry(const uint8_t* id, uint8_t len) {
  if ((uint8_t)(len - 1) >= LITE_MAX_ENTRY_LEN) return false;    // len must be 1..3
  for (uint8_t i = 0; i != LITE_MAX_ENTRIES && i < lite.entry_n; i++) {
    if (lite.entries[i].len == len && memcmp(lite.entries[i].id, id, len) == 0) return true;  // already blocked
  }
  if (lite.entry_n >= LITE_MAX_ENTRIES) return false;  // list full
  LiteEntry* e = &lite.entries[lite.entry_n];
  memset(e, 0, sizeof(*e));
  e->len = len;
  memcpy(e->id, id, len);
  entry_hits[lite.entry_n] = 0;
  lite.entry_n++;
  saveLite();
  return true;
}

bool MeshOSLite::removeEntry(const uint8_t* id, uint8_t len) {
  int i = findEntry(id, len);
  if (i < 0) return false;

  uint8_t n = lite.entry_n;
  for (uint8_t j = i; j + 1 < n; j++) {
    lite.entries[j] = lite.entries[j + 1];
    entry_hits[j] = entry_hits[j + 1];
  }
  lite.entry_n = n - 1;
  memset(&lite.entries[lite.entry_n], 0, sizeof(LiteEntry));
  entry_hits[lite.entry_n] = 0;
  saveLite();
  return true;
}

void MeshOSLite::beginLite(FILESYSTEM* fs) {
  _lite_fs = fs;

  bool found = fs->exists(MESHOS_LITE_FILE);
  if (found) {
    File file = fs->open(MESHOS_LITE_FILE);
    if (file) {
      lite.loadSerial(file);
      file.close();
    }
  }

  // sanitise what was loaded
  if (lite.blocked_n > LITE_MAX_BLOCKED) lite.blocked_n = LITE_MAX_BLOCKED;
  uint8_t n = 0;
  for (uint8_t i = 0; i < lite.entry_n && i < LITE_MAX_ENTRIES; i++) {
    if (lite.entries[i].len >= 1 && lite.entries[i].len <= LITE_MAX_ENTRY_LEN) {
      if (n != i) lite.entries[n] = lite.entries[i];
      n++;
    }
  }
  for (uint8_t i = n; i < LITE_MAX_ENTRIES; i++) {
    memset(&lite.entries[i], 0, sizeof(LiteEntry));
  }
  lite.entry_n = n;

  if (!found) {
    NodePrefs* prefs = getNodePrefs();
    if (prefs->disable_fwd) {   // flashed over a companion: make it a repeater
      prefs->disable_fwd = 0;
      if (prefs->advert_interval < 30) prefs->advert_interval = 30;   // hourly local advert
      if (prefs->flood_advert_interval == 0) prefs->flood_advert_interval = 47;
      savePrefs();
      Serial.println("LITE: first run on companion settings - forwarding ON, local advert hourly");
    }
    saveLite();
  }
}

bool MeshOSLite::handleLiteCommand(char* command, char* reply) {
  while (*command == ' ') command++;

  if (memcmp(command, "block.channel ", 14) == 0 || memcmp(command, "unblock.channel ", 16) == 0) {
    bool block = command[0] == 'b';
    char* arg = &command[block ? 14 : 16];

    uint8_t hash;
    if (!parseChannelHash(arg, hash)) {
      strcpy(reply, "Err - give a hex hash (00-FF) or a #hashtag");
      return true;
    }
    if (block) {
      if (blockChannel(hash)) {
        sprintf(reply, "OK - channel %02X blocked (not repeated)", hash);
      } else {
        sprintf(reply, "Err - block list full (%d)", LITE_MAX_BLOCKED);
      }
    } else {
      if (unblockChannel(hash)) {
        sprintf(reply, "OK - channel %02X unblocked", hash);
      } else {
        sprintf(reply, "Err - channel %02X was not blocked", hash);
      }
    }
    return true;
  }

  if (strcmp(command, "get blocked") == 0) {
    if (lite.blocked_n == 0) {
      strcpy(reply, "blocked: none");
      return true;
    }
    strcpy(reply, "blocked:");
    int len = 8;
    for (int i = 0; i < lite.blocked_n && i < LITE_MAX_BLOCKED; i++) {
      sprintf(&reply[len], "%s%02X", i == 0 ? " " : ",", lite.blocked[i]);
      len += 3;
    }
    snprintf(&reply[len], 160 - len, " (%lu not repeated)", not_repeated);
    return true;
  }

  if (memcmp(command, "chan.hash ", 10) == 0) {
    char* arg = &command[10];

    uint8_t hash;
    if (!parseChannelHash(arg, hash)) {
      strcpy(reply, "Err - give a #hashtag or a hex hash");
      return true;
    }
    snprintf(reply, 160, "%s -> %02X%s", arg, hash, isChannelBlocked(hash) ? " (blocked)" : "");
    return true;
  }

  if (memcmp(command, "set repeat.public ", 18) == 0) {
    char* arg = &command[18];
    while (*arg == ' ') arg++;

    if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
      unblockChannel(PUBLIC_CHANNEL_HASH);
      sprintf(reply, "OK - public channel %s", "repeated");
    } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
      blockChannel(PUBLIC_CHANNEL_HASH);
      sprintf(reply, "OK - public channel %s", "NOT repeated");
    } else {
      strcpy(reply, "Err - on|off");
    }
    return true;
  }

  if (strcmp(command, "get repeat.public") == 0) {
    sprintf(reply, "> %s", isChannelBlocked(PUBLIC_CHANNEL_HASH) ? "off" : "on");
    return true;
  }

  if (memcmp(command, "block.entry ", 12) == 0 || memcmp(command, "unblock.entry ", 14) == 0) {
    bool block = command[0] == 'b';
    char* arg = &command[block ? 12 : 14];
    while (*arg == ' ') arg++;

    uint8_t id[LITE_MAX_ENTRY_LEN], len;
    if (!parseEntryId(arg, id, len)) {
      strcpy(reply, "Err - give the repeater's ID in hex, 1-3 bytes (e.g. A0 or A07E)");
      return true;
    }
    char hex[LITE_MAX_ENTRY_LEN*2 + 2];
    liteToHex(id, len, hex);

    if (block) {
      if (addEntry(id, len)) {
        sprintf(reply, "OK - channel messages entering via %s are not repeated", hex);
      } else {
        sprintf(reply, "Err - entry block list full (%d)", LITE_MAX_ENTRIES);
      }
    } else {
      if (removeEntry(id, len)) {
        sprintf(reply, "OK - entry %s unblocked", hex);
      } else {
        sprintf(reply, "Err - entry %s was not blocked", hex);
      }
    }
    return true;
  }

  if (strcmp(command, "get blocked.entry") == 0) {
    if (lite.entry_n == 0) {
      strcpy(reply, "blocked entries: none");
      return true;
    }
    strcpy(reply, "blocked entries:");
    int len = 16;
    for (int i = 0; i < lite.entry_n && i < LITE_MAX_ENTRIES; i++) {
      char hex[LITE_MAX_ENTRY_LEN*2 + 2];
      liteToHex(lite.entries[i].id, lite.entries[i].len, hex);
      len += snprintf(&reply[len], 160 - len, "%s %s (%lu)", i == 0 ? "" : ",", hex, entry_hits[i]);
      if (len >= 138) break;
    }
    return true;
  }

  if (strcmp(command, "chanstats") == 0) {
    int len = sprintf(reply, "channels (public %02X): ", PUBLIC_CHANNEL_HASH);
    if (formatTopHeard(chan_heard, &reply[len], 160 - len) == 0) {
      strcpy(&reply[len], "none heard yet");
    }
    return true;
  }
  if (strcmp(command, "entrystats") == 0) {
    int len = sprintf(reply, "entry repeaters: ");
    if (formatTopHeard(entry_heard, &reply[len], 160 - len) == 0) {
      strcpy(&reply[len], "none heard yet");
    }
    return true;
  }
  if (strcmp(command, "clear chanstats") == 0) {
    memset(chan_heard, 0, sizeof(chan_heard));
    memset(entry_heard, 0, sizeof(entry_heard));
    memset(entry_hits, 0, sizeof(entry_hits));
    not_repeated = 0;
    strcpy(reply, "OK");
    return true;
  }

  if (strcmp(command, "prefs.legacy") == 0) {
    // drop /prefs.json so the next boot re-imports the settings MeshCore 1.16 or older saved
    if (_lite_fs && _lite_fs->exists("/com_prefs")) {
      _lite_fs->remove("/prefs.json");
      strcpy(reply, "OK - restart to load the settings MeshCore 1.16 or older saved (/com_prefs)");
    } else {
      strcpy(reply, "Err - no /com_prefs (settings from MeshCore 1.16 or older) on this board");
    }
    return true;
  }

  if (strcmp(command, "ver") == 0) {
    snprintf(reply, 160, "%s %s (MeshCore %s, Build: %s)", MESHOS_LITE_NAME, MESHOS_LITE_VERSION, FIRMWARE_VERSION, FIRMWARE_BUILD_DATE);
    return true;
  }
  if (strcmp(command, "ver.meshos") == 0) {
    sprintf(reply, "%s %s", MESHOS_LITE_NAME, MESHOS_LITE_VERSION);
    return true;
  }
  return false;   // not ours, let MyMesh handle it
}

static void countHeard(uint8_t counts[256], uint8_t hash) {
  if (counts[hash] == 0xFF) {
    for (int i = 0; i < 256; i++) counts[i] >>= 1;
  }
  counts[hash]++;
}

void MeshOSLite::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);

  uint8_t type = pkt->getPayloadType();
  if ((type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA) && pkt->payload_len > 0) {
    countHeard(chan_heard, pkt->payload[0]);
    if (pkt->isRouteFlood() && pkt->getPathHashCount() > 0) {
      countHeard(entry_heard, pkt->path[0]);
    }
  }
}

bool MeshOSLite::allowPacketForward(const mesh::Packet* packet) {
  uint8_t type = packet->getPayloadType();
  if (type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) return MyMesh::allowPacketForward(packet);

  if (packet->payload_len > 0 && isChannelBlocked(packet->payload[0])) {
    if (not_repeated != 0xFFFFFFFF) not_repeated++;
    return false;
  }

  // channel spam entering the mesh through a blocked repeater: match the first hop of the flood path
  uint8_t path_len = packet->path_len;
  uint8_t hops = path_len & 63;
  uint8_t hash_size = (path_len >> 6) + 1;
  if (lite.entry_n > 0 && packet->isRouteFlood() && hops > 0 && hash_size <= LITE_MAX_ENTRY_LEN) {
    for (int i = 0; i < lite.entry_n && i < LITE_MAX_ENTRIES; i++) {
      uint8_t n = lite.entries[i].len;
      if (n < 1 || n > LITE_MAX_ENTRY_LEN) continue;
      if (n > hash_size) n = hash_size;
      if (memcmp(lite.entries[i].id, packet->path, n) == 0) {
        if (entry_hits[i] != 0xFFFFFFFF) entry_hits[i]++;
        return false;
      }
    }
  }
  return MyMesh::allowPacketForward(packet);
}

void MeshOSLite::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  if (handleLiteCommand(command, reply)) return;
  MyMesh::handleCommand(sender_timestamp, command, reply);
}
