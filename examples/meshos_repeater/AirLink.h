#pragma once

#include "MeshOSFull.h"

// ---------------------------------------------------------------------------
// MeshOS AirLink bridge
//
// AirLink links the MeshOS app's phone-to-phone mesh (carried to the repeater
// over Bluetooth LE) to LoRa. Quoting the firmware's own web help:
//
//   "links the MeshOS app's phone-to-phone mesh (AirLink, over Bluetooth) to
//    LoRa: direct messages, their acks and the channels below cross both ways,
//    end-to-end encrypted as sent. Up to 3 phones. Works in Bluetooth and WiFi
//    mode (boards with PSRAM); applies after a restart."
//
// The repeater never decrypts anything: it moves whole mesh Packets (ciphertext
// as sent) between the phones and LoRa, and it decides *which* packets cross by
// looking only at the packet type and, for group packets, the 1-byte channel
// hash. Direct messages / their acks / path returns always cross; group text and
// group data cross only when their channel is selected (the public channel, gated
// by airlink.public, plus any hashtag channels in airlink.channels). Channel
// messages whose entry repeater is blocked by the spam filter are kept off the
// phones, matching the Finder help ("Block a spammer's entry repeater and its
// channel messages are not repeated here or passed to AirLink phones; direct
// messages, acks and adverts still go through").
//
// This is a *functional* reconstruction, gated to ESP32 + PSRAM like the real
// thing. The bridging policy, de-duplication, echo suppression, per-phone slot
// accounting and the status/stats counters are reconstructed faithfully from the
// shipped firmware's string table and web interface. The multi-connection BLE
// GATT companion transport (PhoneLink, below) is rebuilt on the same ESP32 BLE
// stack the single-connection SerialBLEInterface uses; the exact on-wire layout
// of the companion frames is not recoverable from strings, so PhoneLink exchanges
// length-prefixed mesh Packets and answers a companion-protocol probe with the
// repeater banner ("not a companion radio - use the app's Repeater manager"),
// which is all the recovered strings evidence.
// ---------------------------------------------------------------------------

#ifdef ESP32

#include "helpers/bridges/BridgeBase.h"

#ifndef AIRLINK_MAX_PHONES
  #define AIRLINK_MAX_PHONES   3        // "Up to 3 phones"
#endif

#ifndef AIRLINK_MAX_CH
  #define AIRLINK_MAX_CH       16       // extra hashtag channels (airlink_ch holds ~that many)
#endif

// Lifetime counters, published in airlink.status and the /api status JSON. Field
// names mirror the recovered status format string exactly.
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
  // "...,"public":%s,"chans":"%s"" fragment for the web status JSON
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
  void   relayToPhonesRaw(const uint8_t* framed, uint16_t len);
  void   drainPhones();                            // pull phone frames -> LoRa / other phones
  void   onPhoneJoined(uint16_t conn, uint16_t mtu, uint16_t interval, uint16_t timeout);
  void   onPhoneLeft(uint16_t conn, uint8_t reason, int8_t last_dbm, int8_t weakest_dbm);
  int    findSlot(uint16_t conn) const;
  bool   rateOk(PhoneSlot& p);                     // per-phone send cap
};

#endif // ESP32
