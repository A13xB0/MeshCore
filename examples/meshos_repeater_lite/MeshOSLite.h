#pragma once

// ---------------------------------------------------------------------------
// MeshOS Repeater Lite  (nRF52 / RP2040 / small ESP32 boards)
//
// The stock MeshCore repeater plus the MeshOS spam filter (see
// ../meshos_shared/SpamFilter.h) and its CLI commands. This is the cut-down
// sibling of the full ESP32 repeater in examples/meshos_repeater/ - no web UI,
// observer, AirLink or OTA.
//
// Settings are kept in /meshos_lite.json, separate from the stock /prefs.json.
// ---------------------------------------------------------------------------

#include "../simple_repeater/MyMesh.h"
#include "../meshos_shared/SpamFilter.h"

#define MESHOS_LITE_NAME      "MeshOS Repeater Lite"
#define MESHOS_LITE_VERSION   "1.0"

#define MESHOS_LITE_FILE      "/meshos_lite.json"

class LitePrefs : public ConfigSerializer {
public:
  FilterLists filter;

  void structure() override {
    def("blocked_n", filter.channel_count);
    def("blocked", filter.channels, sizeof(filter.channels));
    def("entry_n", filter.entry_count);
    def("entry", filter.entries, sizeof(filter.entries));
  }
};

class MeshOSLite : public MyMesh {
  LitePrefs _lite_prefs;
  SpamFilter _filter { _lite_prefs.filter };
  FILESYSTEM* _fs = NULL;

  void saveLitePrefs();
  void applyFirstRunDefaults();
  bool handleChannelBlockCommand(char* command, char* reply);
  bool handleEntryBlockCommand(char* command, char* reply);
  void formatBlockedChannels(char* reply);
  void formatBlockedEntries(char* reply);

protected:
  bool allowPacketForward(const mesh::Packet* packet) override;
  void logRx(mesh::Packet* pkt, int len, float score) override;

public:
  MeshOSLite(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : MyMesh(board, radio, ms, rng, rtc, tables) { }

  // Call after MyMesh::begin(): loads (or creates) the MeshOS settings file.
  void beginLite(FILESYSTEM* fs);

  // Handles the MeshOS commands; returns false if `command` is not one of them.
  bool handleLiteCommand(char* command, char* reply);

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply) override;
};
