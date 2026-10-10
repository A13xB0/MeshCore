#pragma once

#include "../simple_repeater/MyMesh.h"

#define MESHOS_LITE_NAME      "MeshOS Repeater Lite"
#define MESHOS_LITE_VERSION   "1.0"

#define MESHOS_LITE_FILE      "/meshos_lite.json"

#define LITE_MAX_BLOCKED      16    // blocked channel hashes
#define LITE_MAX_ENTRIES      8     // blocked entry repeaters
#define LITE_MAX_ENTRY_LEN    3     // max bytes of a repeater ID prefix

#define PUBLIC_CHANNEL_HASH   0x11

struct LiteEntry {
  uint8_t len;      // 1..3 bytes of ID prefix
  uint8_t id[LITE_MAX_ENTRY_LEN];
};

class LitePrefs : public ConfigSerializer {
public:
  uint8_t blocked_n = 0;
  uint8_t blocked[LITE_MAX_BLOCKED] = {};
  uint8_t entry_n = 0;
  LiteEntry entries[LITE_MAX_ENTRIES] = {};

  void structure() override {
    def("blocked_n", blocked_n);
    def("blocked", blocked, sizeof(blocked));
    def("entry_n", entry_n);
    def("entry", entries, sizeof(entries));
  }
};

// The standard repeater, plus the MeshOS spam filters: channels that are not repeated, and
// repeaters that channel spam comes in through (the first hop of a flood path).
class MeshOSLite : public MyMesh {
  LitePrefs lite;
  FILESYSTEM* _lite_fs = NULL;
  uint8_t chan_heard[256] = {};    // group packets heard, by channel hash (halved on overflow)
  uint8_t entry_heard[256] = {};   // flood group packets heard, by first path hash
  uint32_t not_repeated = 0;       // group packets dropped by the channel block list
  uint32_t entry_hits[LITE_MAX_ENTRIES] = {};

  bool isChannelBlocked(uint8_t hash) const {
    for (uint8_t i = 0; i < lite.blocked_n && i < LITE_MAX_BLOCKED; i++) {
      if (lite.blocked[i] == hash) return true;
    }
    return false;
  }
  int findEntry(const uint8_t* id, uint8_t len) const {
    for (int i = 0; i < lite.entry_n; i++) {
      if (lite.entries[i].len == len && memcmp(lite.entries[i].id, id, len) == 0) return i;
    }
    return -1;
  }
  bool blockChannel(uint8_t hash);      // add to the block list; false if it is full
  bool unblockChannel(uint8_t hash);    // remove from the block list; false if it was not blocked
  bool addEntry(const uint8_t* id, uint8_t len);     // add an entry repeater; false if full/bad len
  bool removeEntry(const uint8_t* id, uint8_t len);  // remove one; false if it was not blocked

  void saveLite() {
    if (_lite_fs) {
      _lite_fs->remove(MESHOS_LITE_FILE);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
      File file = _lite_fs->open(MESHOS_LITE_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
      File file = _lite_fs->open(MESHOS_LITE_FILE, "w");
#else
      File file = _lite_fs->open(MESHOS_LITE_FILE, "w", true);
#endif
      if (file) {
        lite.saveSerial(file);
        file.close();
      }
    }
  }

protected:
  bool allowPacketForward(const mesh::Packet* packet) override;
  void logRx(mesh::Packet* pkt, int len, float score) override;

public:
  MeshOSLite(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : MyMesh(board, radio, ms, rng, rtc, tables) { }

  void beginLite(FILESYSTEM* fs);
  bool handleLiteCommand(char* command, char* reply);

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply) override;
};
