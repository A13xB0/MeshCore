#pragma once

#include "MeshOSFull.h"

// ---------------------------------------------------------------------------
// MeshOS AirLink bridge (ESP32 with PSRAM only)
//
// Links the MeshOS app's phone-to-phone mesh, carried to the repeater over
// Bluetooth LE, to LoRa. Up to AIRLINK_MAX_PHONES phones connect at once.
//
// The repeater never decrypts anything: whole mesh Packets (ciphertext as sent)
// move between the phones and LoRa. Which packets cross is decided from the
// packet type and, for group packets, the 1-byte channel hash:
//   * direct messages, their acks and path returns always cross
//   * group text/data cross only on a selected channel - the public channel
//     (airlink.public) plus any #hashtags in airlink.channels
//   * channel packets whose entry repeater is blocked by the spam filter never
//     reach the phones
//   * adverts and control traffic do not cross
//
// Phone frames are a 1-byte length followed by a serialised mesh Packet. A
// short frame starting with 0x00 is a companion-protocol probe (the app thinks
// this is a companion radio) and is answered with a JSON banner instead.
//
// Settings apply after a restart.
// ---------------------------------------------------------------------------

#ifdef ESP32

#include "helpers/bridges/BridgeBase.h"

#ifndef AIRLINK_MAX_PHONES
  #define AIRLINK_MAX_PHONES   3        // "Up to 3 phones"
#endif

#ifndef AIRLINK_MAX_CH
  #define AIRLINK_MAX_CH       16       // extra hashtag channels (airlink_ch holds ~that many)
#endif

// Lifetime counters, published in airlink.status and the /api/status JSON.
struct AirLinkStats {
  uint32_t rx = 0;             // mesh Packets received from phones
  uint32_t dup = 0;            // ... that we had already seen (dropped)
  uint32_t relayed = 0;        // packets relayed phone -> phone (not onto LoRa)
  uint32_t to_lora = 0;        // packets injected phone -> LoRa
  uint32_t to_phones = 0;      // LoRa packets handed to the phones
  uint32_t limited = 0;        // dropped by the per-phone rate cap ("capped")
  uint32_t dropped = 0;        // dropped for any other reason (queue full, no mem)
  uint32_t confirmed = 0;      // phone sends we saw acked on LoRa
  uint32_t rewrapped = 0;      // LoRa packets another bridge gave the phones, kept off LoRa
  uint32_t echo_lora = 0;      // ... suppressed because we put them on LoRa (own LoRa hops)
  uint32_t echo_heard = 0;     // ... suppressed because we heard them on LoRa first
  uint32_t entry_blocked = 0;  // channel packets withheld: entry repeater is blocked
  uint32_t left = 0;           // phones that have disconnected
  uint8_t  last_reason = 0;    // last BLE disconnect reason
  uint8_t  peak_phones = 0;    // most phones connected at once
  uint32_t joins = 0;          // total phone joins
};

// Opaque handle to the multi-connection BLE companion transport (defined in the
// .cpp). One per AirLink bridge; owns up to AIRLINK_MAX_PHONES GATT links.
class PhoneLink;

class AirLink : public BridgeBase {
public:
  AirLink(MeshOSFull& mesh, NodePrefs* prefs, mesh::PacketManager* mgr, mesh::RTCClock* rtc)
    : BridgeBase(prefs, mgr, rtc), _mesh(mesh) { }

  // AbstractBridge
  void begin() override;
  void end() override;
  void loop() override;
  void sendPacket(mesh::Packet* packet) override;        // LoRa TX/RX hook -> maybe relay to phones
  void onPacketReceived(mesh::Packet* packet) override;  // phone -> LoRa (queue inbound)

  // status line for `get airlink` / airlink.status and the web status card
  String statusLine();
  // the fields of the "airlink" object in /api/status (without the braces)
  String statsJson();

private:
  struct PhoneSlot {
    bool     active = false;
    uint16_t conn_id = 0;
    uint16_t mtu = 23;
    uint16_t link_interval = 0;   // units of 1.25 ms
    uint16_t timeout = 0;         // units of 10 ms
    int8_t   last_dbm = 0;
    int8_t   weakest_dbm = 0;
    bool     signed_in = false;
    char     name[24] = {};       // learned from the companion handshake
    uint32_t sends = 0;           // for the per-phone rate cap
    uint32_t window_start = 0;
  };

  MeshOSFull& _mesh;
  PhoneLink*  _link = NULL;       // BLE companion transport (PSRAM-backed)
  AirLinkStats _st;
  PhoneSlot _phones[AIRLINK_MAX_PHONES];
  uint8_t   _n_connected = 0;

  // selected group-channel hashes (public + resolved hashtags), built at begin()
  uint8_t  _sel_hash[1 + AIRLINK_MAX_CH];
  uint8_t  _n_sel = 0;

  void   resolveChannels();                       // airlink.public/.channels -> _sel_hash[]
  bool   channelSelected(uint8_t hash) const;
  bool   shouldCross(const mesh::Packet* pkt) const;   // does this packet cross the bridge?
  void   relayToPhones(mesh::Packet* pkt);
  void   drainPhones();                            // pull phone frames -> LoRa / other phones
  String bannerJson();                             // the "meshos-rpt/1" probe reply
  void   onPhoneJoined(uint16_t conn, uint16_t mtu);
  void   onPhoneLeft(uint16_t conn, uint8_t reason, int8_t last_dbm, int8_t weakest_dbm);
  int    findSlot(uint16_t conn) const;
  bool   rateOk(PhoneSlot& p);                     // per-phone send cap
};

#endif // ESP32
