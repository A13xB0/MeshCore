#pragma once

#include "MeshOSPrefs.h"

// ---------------------------------------------------------------------------
// MeshOS full repeater (ESP32)
//
// The stock MeshCore repeater plus:
//   * the spam filter (shared with Repeater Lite, ../meshos_shared/SpamFilter.h)
//   * the MeshOS settings (MeshOSPrefs) and their CLI commands
//   * the data behind the web UI's JSON API: packet log, channel stats, finder,
//     graph, regions, trace, signal and airtime
//   * the observer's signed payloads
//   * a persistent cache of neighbour names learned from adverts
//
// The web server, observer MQTT client, AirLink bridge, self-update and WiFi
// station live in their own classes (see main.cpp) and call into this one.
// ---------------------------------------------------------------------------

#define MESHOS_NET_FILE     "/meshos_net.json"
#define MESHOS_NAMES_FILE   "/meshos_names.bin"

// MeshOS product version (separate from the MeshCore version it is built on).
// Reported by `ver.meshos` and as "ver" in the status JSON.
#ifndef MESHOS_VERSION
  #define MESHOS_VERSION   "1.6.1"
#endif

// Board names reported in the status JSON and the AirLink banner.
#ifndef MESHOS_BOARD_NAME
  #define MESHOS_BOARD_NAME   "Heltec V4"
#endif
#ifndef MESHOS_HW_NAME
  #define MESHOS_HW_NAME      "Heltec V4 OLED"
#endif

class AbstractBridge;   // the AirLink bridge; fed every received packet from logRx

class MeshOSFull : public MyMesh {
public:
  MeshOSFull(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : MyMesh(board, radio, ms, rng, rtc, tables) { }

  // Call after MyMesh::begin(): loads the MeshOS settings and the names cache.
  void beginFull(FILESYSTEM* fs);
  // Call from the main loop: airtime sampling and deferred names-cache saves.
  void meshosTick();

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply) override;

  // ---- settings ----
  MeshOSPrefs& meshosPrefs() { return _prefs; }
  NodePrefs* nodePrefs() { return getNodePrefs(); }
  uint8_t getTransport() const { return _prefs.transport; }
  bool isWifiTransport() const { return _prefs.transport == TRANSPORT_WIFI; }
  const char* transportName() const { return ::transportName(_prefs.transport); }
  bool setupDone() const { return _prefs.setup_done; }
  bool inheritedPrefs() const { return _prefs.inherited; }
  bool firstRun() const { return _first_run; }
  const char* wifiSsid() const { return _prefs.wifi_ssid; }
  bool webPwdSet() const { return _prefs.web_pwd[0] != 0; }
  uint32_t getBlePin() const { return _prefs.blePin(); }
  bool observerEnabled() const { return _prefs.observer; }
  bool airlinkEnabled() const { return _prefs.airlink; }
  bool airlinkPublic() const { return _prefs.airlink_pub; }
  const char* airlinkChannels() const { return _prefs.airlink_ch; }

  // ---- AirLink hooks ----
  mesh::PacketManager* getPacketManager() { return _mgr; }
  void setAirLink(AbstractBridge* bridge) { _airlink = bridge; }
  // was this packet's entry repeater (first path hop) blocked by the spam filter?
  bool isEntryBlockedPkt(const mesh::Packet* pkt) const { return _filter.matchEntry(pkt) >= 0; }

  // ---- web API payloads ----
  String channelsJson();            // /api/channels: block lists + heard stats
  String logJson(uint32_t since);   // /api/log: packets newer than sequence number `since`
  String finderJson();              // /api/finder: directly-heard nodes
  String regionsJson();             // /api/regions: scoped-flood region tree
  String graphJson();               // /api/graph: this repeater + directly-heard nodes
  String traceJson();               // /api/trace: last trace result, or {ok:false}
  String sigJson();                 // status "sig": last-heard signal

  // rolling airtime (ms) / packets sent over the last 5 min and 24 h, for status "air"
  uint32_t airTx5() const;
  uint32_t airRx5() const;
  uint32_t airSent5() const;
  uint32_t airTx24() const;
  uint32_t airRx24() const;
  uint32_t airSent24() const;

  // ---- observer ----
  String observerHeartbeat();        // signed status payload for the status topic
  bool observerNextRx(String& out);  // next heard-packet payload for the rx topic, or false

protected:
  bool allowPacketForward(const mesh::Packet* packet) override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;
  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                    const uint8_t* app_data, size_t app_data_len) override;

private:
  MeshOSPrefs _prefs;
  SpamFilter _filter { _prefs.filter };
  FILESYSTEM* _fs = NULL;
  AbstractBridge* _airlink = NULL;   // set by main.cpp when the AirLink bridge runs
  bool _first_run = false;           // no MeshOS settings were found at boot

  // signal of the most recent raw receive (logRxRaw runs just before logRx)
  float _last_snr = 0, _last_rssi = 0;

  void saveConfig();
  void inheritCompanionWifi();
  static bool extractJsonStr(const char* json, const char* key, char* out, size_t out_size);

  // ---- CLI ----
  bool handleFilterCommand(char* command, char* reply);
  bool handleConfigCommand(char* command, char* reply);
  bool handleTraceCommand(char* command, char* reply);

  // ---- packet log for /api/log (ring buffer of recent packets) ----
  enum : uint8_t { DIR_RX = 0, DIR_TX = 1 };
  enum : uint8_t { LOG_FLAG_ENTRY_BLOCKED = 1, LOG_FLAG_CHANNEL_BLOCKED = 2 };
  struct LoggedPacket {
    uint32_t seq;          // increasing sequence number
    uint32_t time;         // seconds since boot
    uint8_t  dir;          // DIR_RX / DIR_TX
    uint8_t  type;         // payload type
    uint8_t  route;        // route type
    uint8_t  len;          // raw length
    uint8_t  hops;         // path hash count
    uint8_t  hash_size;    // bytes per path hash
    uint8_t  channel;      // group channel hash, or 255
    uint8_t  flags;        // LOG_FLAG_*
    int8_t   snr_x4;       // SNR * 4 (rx only)
    int16_t  rssi;         // (rx only)
    uint8_t  path[8];      // first bytes of the path
  };
  static const uint8_t LOG_SIZE = 48;
  LoggedPacket _log[LOG_SIZE] = {};
  uint8_t _log_head = 0, _log_count = 0;
  uint32_t _log_seq = 0;
  void recordPacket(mesh::Packet* pkt, int len, uint8_t dir, uint8_t flags);

  // ---- observer: queue of heard packets waiting to be published ----
  struct QueuedRx {
    uint32_t time;         // seconds since boot
    int8_t   snr_x4;
    int16_t  rssi;
    uint8_t  len;
    uint8_t  raw[184];
  };
  static const uint8_t OBSERVER_QUEUE_SIZE = 8;
  QueuedRx _obs_queue[OBSERVER_QUEUE_SIZE] = {};
  uint8_t _obs_head = 0, _obs_count = 0;
  uint32_t _obs_dropped = 0;
  void queueForObserver(mesh::Packet* pkt);
  String appendSignature(const String& json);

  // ---- trace started from the web Trace tab ----
  static const uint8_t TRACE_MAX_HOPS = 16;
  uint32_t _trace_tag = 0;
  bool _trace_active = false, _trace_done = false;
  uint8_t _trace_len = 0, _trace_flags = 0;
  uint8_t _trace_hashes[TRACE_MAX_HOPS] = {}, _trace_snrs[TRACE_MAX_HOPS] = {};

  // ---- neighbour names learned from adverts, kept across reboots ----
  static const uint8_t NAME_KEY_LEN = 4;   // bytes of public key used as the key
  struct NameEntry { uint8_t pub[NAME_KEY_LEN]; char name[32]; };   // file record layout
  static const uint8_t NAMES_MAX = 48;
  static const uint32_t NAMES_SAVE_DELAY_MS = 5000;   // batch up writes
  NameEntry _names[NAMES_MAX] = {};
  uint8_t   _names_count = 0;
  bool      _names_dirty = false;
  unsigned long _names_save_at = 0;
  void loadNames();
  void saveNames();
  void rememberName(const uint8_t* pub, const char* name);
  const char* lookupName(const uint8_t* pub) const;

  // ---- airtime sampled once a minute for the status "air" object ----
  struct AirSample { uint32_t tx, rx, sent; };
  static const uint8_t AIR_MINUTES = 5, AIR_HOURS = 24;
  AirSample _air_minutes[AIR_MINUTES] = {}, _air_hours[AIR_HOURS] = {};
  uint8_t   _air_minute_idx = 0, _air_hour_idx = 0;
  AirSample _air_last = {};   // totals at the previous minute sample
  unsigned long _air_next_minute = 0, _air_next_hour = 0;
  void sampleAirtime();

  // ---- last-heard signal for the status "sig" object ----
  uint32_t _sig_time = 0;
  uint8_t  _sig_last_hop = 0, _sig_path_len = 0;
  bool     _sig_direct = false;
};
