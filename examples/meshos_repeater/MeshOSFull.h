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

  // recent-packet ring for the web /api/log (Packets tab)
  struct PktLog {
    uint32_t seq, t;
    uint8_t dir, type, route, len, hops, hs, ch, fl;
    int8_t snr4;
    int16_t rssi;
    uint8_t path[8];
  };
  static const uint8_t LOG_SIZE = 48;
  PktLog _log[LOG_SIZE] = {};
  uint8_t _log_head = 0, _log_count = 0;
  uint32_t _log_seq = 0;
  float _last_snr = 0, _last_rssi = 0;
  void recordPkt(mesh::Packet* pkt, int len, uint8_t dir, uint8_t fl);

  // queue of heard packets awaiting upload to the observer's /rx topic
  struct ObsRx { uint32_t t; int8_t snr4; int16_t rssi; uint8_t len; uint8_t raw[184]; };
  static const uint8_t OBS_Q = 8;
  ObsRx _obs_q[OBS_Q] = {};
  uint8_t _obs_head = 0, _obs_count = 0;
  uint32_t _obs_dropped = 0;
  void observerCaptureRx(mesh::Packet* pkt);

  // pending over-the-air trace for the web Trace tab
  uint32_t _trace_tag = 0;
  bool _trace_active = false, _trace_done = false;
  uint8_t _trace_len = 0, _trace_flags = 0;
  uint8_t _trace_hashes[16] = {}, _trace_snrs[16] = {};

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
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override {
    _last_snr = snr; _last_rssi = rssi;
    MyMesh::logRxRaw(snr, rssi, raw, len);
  }
  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len) override;

public:
  MeshOSFull(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : MyMesh(board, radio, ms, rng, rtc, tables) { }

  void beginFull(FILESYSTEM* fs);
  uint8_t getTransport() const { return prefs.transport; }
  uint32_t getBlePin() const { return prefs.ble_pin ? prefs.ble_pin : 123456; }
  String channelsJson();            // /api/channels: block state + heard stats
  String logJson(uint32_t since);   // /api/log: recent packets since a sequence number
  String finderJson();              // /api/finder: directly-heard nodes
  bool observerEnabled() const { return prefs.observer; }
  String observerHeartbeat();       // signed status payload for the MQTT observer
  bool observerNextRx(String& out); // next heard-packet payload for the /rx topic, or false
  String regionsJson();             // /api/regions: scoped-flood region tree
  String graphJson();               // /api/graph: this repeater + directly-heard nodes
  bool handleTraceCommand(char* command, char* reply);  // trace <hops>
  String traceJson();               // /api/trace: last trace result, or {ok:false}
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply) override;
};
