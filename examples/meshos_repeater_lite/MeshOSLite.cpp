#include "MeshOSLite.h"

using namespace meshos;

#define REPLY_MAX   160   // size of the reply buffer every caller passes in

// Formats the busiest hashes as "A5 12, 11 3, ..." (most-heard first) until
// `dest` is nearly full. Returns how many were written.
static int formatTopHeard(const uint8_t* counts, char* dest, int dest_len) {
  const int ITEM_MAX = 10;   // longest single item: ", FF 255"
  *dest = 0;
  if (dest_len <= ITEM_MAX) return 0;

  bool listed[256] = {};
  int len = 0, written = 0;
  while (true) {
    int busiest = -1;
    for (int i = 0; i < 256; i++) {
      if (listed[i] || counts[i] == 0) continue;
      if (busiest < 0 || counts[i] > counts[busiest]) busiest = i;
    }
    if (busiest < 0) break;
    listed[busiest] = true;

    len += snprintf(&dest[len], dest_len - len, "%s%02X %u", written ? ", " : "", busiest, counts[busiest]);
    written++;
    if (len + ITEM_MAX >= dest_len) break;
  }
  return written;
}

static File openForWrite(FILESYSTEM* fs, const char* path) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return fs->open(path, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return fs->open(path, "w");
#else
  return fs->open(path, "w", true);
#endif
}

static File openForRead(FILESYSTEM* fs, const char* path) {
#if defined(RP2040_PLATFORM)
  return fs->open(path, "r");   // RP2040 LittleFS needs an explicit mode
#else
  return fs->open(path);
#endif
}

void MeshOSLite::saveLitePrefs() {
  if (!_fs) return;
  _fs->remove(MESHOS_LITE_FILE);
  File file = openForWrite(_fs, MESHOS_LITE_FILE);
  if (file) {
    _lite_prefs.saveSerial(file);
    file.close();
  }
}

void MeshOSLite::beginLite(FILESYSTEM* fs) {
  _fs = fs;

  bool found = fs->exists(MESHOS_LITE_FILE);
  if (found) {
    File file = openForRead(fs, MESHOS_LITE_FILE);
    if (file) {
      _lite_prefs.loadSerial(file);
      file.close();
    }
  }
  _filter.sanitise();

  if (!found) {
    applyFirstRunDefaults();
    saveLitePrefs();
  }
}

// First boot of the Lite firmware. If the board was previously a companion
// radio, its prefs have forwarding off - turn the repeater on.
void MeshOSLite::applyFirstRunDefaults() {
  NodePrefs* prefs = getNodePrefs();
  if (!prefs->disable_fwd) return;

  prefs->disable_fwd = 0;
  if (prefs->advert_interval < 30) prefs->advert_interval = 30;   // local advert hourly
  if (prefs->flood_advert_interval == 0) prefs->flood_advert_interval = 47;
  savePrefs();
  Serial.println("LITE: first run on companion settings - forwarding ON, local advert hourly");
}

// block.channel <hash|#tag>  /  unblock.channel <hash|#tag>
bool MeshOSLite::handleChannelBlockCommand(char* command, char* reply) {
  char* arg;
  bool block;
  if ((arg = afterPrefix(command, "block.channel ")))        block = true;
  else if ((arg = afterPrefix(command, "unblock.channel "))) block = false;
  else return false;

  uint8_t hash;
  if (!parseChannelHash(arg, hash)) {
    strcpy(reply, "Err - give a hex hash (00-FF) or a #hashtag");
    return true;
  }

  if (block) {
    if (_filter.blockChannel(hash)) {
      saveLitePrefs();
      sprintf(reply, "OK - channel %02X blocked (not repeated)", hash);
    } else {
      sprintf(reply, "Err - block list full (%d)", FILTER_MAX_CHANNELS);
    }
  } else {
    if (_filter.unblockChannel(hash)) {
      saveLitePrefs();
      sprintf(reply, "OK - channel %02X unblocked", hash);
    } else {
      sprintf(reply, "Err - channel %02X was not blocked", hash);
    }
  }
  return true;
}

// block.entry <id>  /  unblock.entry <id>
bool MeshOSLite::handleEntryBlockCommand(char* command, char* reply) {
  char* arg;
  bool block;
  if ((arg = afterPrefix(command, "block.entry ")))        block = true;
  else if ((arg = afterPrefix(command, "unblock.entry "))) block = false;
  else return false;

  uint8_t id[FILTER_MAX_ENTRY_LEN], len;
  if (!parseEntryId(arg, id, len)) {
    strcpy(reply, "Err - give the repeater's ID in hex, 1-3 bytes (e.g. A0 or A07E)");
    return true;
  }
  char hex[FILTER_ENTRY_HEX_LEN];
  bytesToHex(id, len, hex);

  if (block) {
    if (_filter.addEntry(id, len)) {
      saveLitePrefs();
      sprintf(reply, "OK - channel messages entering via %s are not repeated", hex);
    } else {
      sprintf(reply, "Err - entry block list full (%d)", FILTER_MAX_ENTRIES);
    }
  } else {
    if (_filter.removeEntry(id, len)) {
      saveLitePrefs();
      sprintf(reply, "OK - entry %s unblocked", hex);
    } else {
      sprintf(reply, "Err - entry %s was not blocked", hex);
    }
  }
  return true;
}

// "blocked: A5,11 (42 not repeated)"
void MeshOSLite::formatBlockedChannels(char* reply) {
  const FilterLists& lists = _filter.lists();
  if (_filter.channelCount() == 0) {
    strcpy(reply, "blocked: none");
    return;
  }
  int len = sprintf(reply, "blocked:");
  for (uint8_t i = 0; i < _filter.channelCount(); i++) {
    len += sprintf(&reply[len], "%s%02X", i == 0 ? " " : ",", lists.channels[i]);
  }
  snprintf(&reply[len], REPLY_MAX - len, " (%lu not repeated)", (unsigned long) _filter.not_repeated);
}

// "blocked entries: A0 (3), A07E (0)" - the number is packets dropped
void MeshOSLite::formatBlockedEntries(char* reply) {
  const FilterLists& lists = _filter.lists();
  if (_filter.entryCount() == 0) {
    strcpy(reply, "blocked entries: none");
    return;
  }
  int len = sprintf(reply, "blocked entries:");
  for (uint8_t i = 0; i < _filter.entryCount(); i++) {
    char hex[FILTER_ENTRY_HEX_LEN];
    bytesToHex(lists.entries[i].id, lists.entries[i].len, hex);
    len += snprintf(&reply[len], REPLY_MAX - len, "%s %s (%lu)", i == 0 ? "" : ",", hex,
                    (unsigned long) _filter.entry_hits[i]);
    if (len >= REPLY_MAX - 22) break;   // leave room for one more item
  }
}

bool MeshOSLite::handleLiteCommand(char* command, char* reply) {
  command = (char*) skipSpaces(command);
  char* arg;

  if (handleChannelBlockCommand(command, reply)) return true;
  if (handleEntryBlockCommand(command, reply)) return true;

  if (strcmp(command, "get blocked") == 0) {
    formatBlockedChannels(reply);
    return true;
  }
  if (strcmp(command, "get blocked.entry") == 0) {
    formatBlockedEntries(reply);
    return true;
  }

  if ((arg = afterPrefix(command, "chan.hash "))) {
    uint8_t hash;
    if (!parseChannelHash(arg, hash)) {
      strcpy(reply, "Err - give a #hashtag or a hex hash");
      return true;
    }
    snprintf(reply, REPLY_MAX, "%s -> %02X%s", arg, hash, _filter.isChannelBlocked(hash) ? " (blocked)" : "");
    return true;
  }

  if ((arg = afterPrefix(command, "set repeat.public "))) {
    if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
      if (_filter.unblockChannel(PUBLIC_CHANNEL_HASH)) saveLitePrefs();
      strcpy(reply, "OK - public channel repeated");
    } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
      if (_filter.blockChannel(PUBLIC_CHANNEL_HASH)) saveLitePrefs();
      strcpy(reply, "OK - public channel NOT repeated");
    } else {
      strcpy(reply, "Err - on|off");
    }
    return true;
  }
  if (strcmp(command, "get repeat.public") == 0) {
    sprintf(reply, "> %s", _filter.isChannelBlocked(PUBLIC_CHANNEL_HASH) ? "off" : "on");
    return true;
  }

  if (strcmp(command, "chanstats") == 0) {
    int len = sprintf(reply, "channels (public %02X): ", PUBLIC_CHANNEL_HASH);
    if (formatTopHeard(_filter.chan_heard, &reply[len], REPLY_MAX - len) == 0) {
      strcpy(&reply[len], "none heard yet");
    }
    return true;
  }
  if (strcmp(command, "entrystats") == 0) {
    int len = sprintf(reply, "entry repeaters: ");
    if (formatTopHeard(_filter.entry_heard, &reply[len], REPLY_MAX - len) == 0) {
      strcpy(&reply[len], "none heard yet");
    }
    return true;
  }
  if (strcmp(command, "clear chanstats") == 0) {
    _filter.clearStats();
    strcpy(reply, "OK");
    return true;
  }

  if (strcmp(command, "prefs.legacy") == 0) {
    // Remove /prefs.json so the next boot re-imports the settings MeshCore 1.16
    // or older saved in /com_prefs.
    if (_fs && _fs->exists("/com_prefs")) {
      _fs->remove("/prefs.json");
      strcpy(reply, "OK - restart to load the settings MeshCore 1.16 or older saved (/com_prefs)");
    } else {
      strcpy(reply, "Err - no /com_prefs (settings from MeshCore 1.16 or older) on this board");
    }
    return true;
  }

  if (strcmp(command, "ver") == 0) {
    snprintf(reply, REPLY_MAX, "%s %s (MeshCore %s, Build: %s)",
             MESHOS_LITE_NAME, MESHOS_LITE_VERSION, FIRMWARE_VERSION, FIRMWARE_BUILD_DATE);
    return true;
  }
  if (strcmp(command, "ver.meshos") == 0) {
    sprintf(reply, "%s %s", MESHOS_LITE_NAME, MESHOS_LITE_VERSION);
    return true;
  }
  return false;   // not ours, let MyMesh handle it
}

void MeshOSLite::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);
  _filter.noteHeard(pkt);
}

bool MeshOSLite::allowPacketForward(const mesh::Packet* packet) {
  if (_filter.shouldDrop(packet)) return false;
  return MyMesh::allowPacketForward(packet);
}

void MeshOSLite::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  if (handleLiteCommand(command, reply)) return;
  MyMesh::handleCommand(sender_timestamp, command, reply);
}
