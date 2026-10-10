#pragma once

#include "MeshOSPrefs.h"

// Work-in-progress reconstruction of the MeshOS full repeater. For now it holds
// the recovered config struct and the spam filter (shared with Repeater Lite),
// so the filter helpers compile against the full build's struct offsets. The
// web interface, transport switching, AirLink, observer and OTA are catalogued
// in FEATURES.md and not yet reconstructed.

#define PUBLIC_CHANNEL_HASH   0x11
#define MESHOS_NET_FILE       "/meshos_net.json"

class MeshOSFull : public MyMesh {
  MeshOSPrefs prefs;                 // sits at the base-class end -> blocked_n at +0x90
  uint8_t chan_heard[256] = {};
  uint8_t entry_heard[256] = {};
  uint32_t not_repeated = 0;
  uint32_t entry_hits[MESHOS_MAX_ENTRIES] = {};
  FILESYSTEM* _net_fs = NULL;

  // The shipped binary keeps isChannelBlocked a separate function because its integrated
  // CLI/web handler calls it from several places. Until that handler is reconstructed, noinline
  // stands in for that call-graph effect so the filter functions compile to the author's shapes.
  __attribute__((noinline)) bool isChannelBlocked(uint8_t hash) const {
    for (uint8_t i = 0; i < prefs.blocked_n && i < MESHOS_MAX_BLOCKED; i++) {
      if (prefs.blocked[i] == hash) return true;
    }
    return false;
  }
  int findEntry(const uint8_t* id, uint8_t len) const {
    for (int i = 0; i < prefs.entry_n; i++) {
      if (prefs.entries[i].len == len && memcmp(prefs.entries[i].id, id, len) == 0) return i;
    }
    return -1;
  }
  int matchEntry(const mesh::Packet* packet) const;

  void saveConfig();            // write prefs to /meshos_net.json
  void saveFilter() { saveConfig(); }

  bool blockChannel(uint8_t hash);
  bool unblockChannel(uint8_t hash);
  bool addEntry(const uint8_t* id, uint8_t len);
  bool removeEntry(const uint8_t* id, uint8_t len);
  bool handleFilterCommand(char* command, char* reply);
  bool handleConfigCommand(char* command, char* reply);

protected:
  bool allowPacketForward(const mesh::Packet* packet) override;

public:
  MeshOSFull(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : MyMesh(board, radio, ms, rng, rtc, tables) { }

  void beginFull(FILESYSTEM* fs);
  uint8_t getTransport() const { return prefs.transport; }
  uint32_t getBlePin() const { return prefs.ble_pin ? prefs.ble_pin : 123456; }
  String channelsJson();   // /api/channels: block state + heard stats
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply) override;
};
