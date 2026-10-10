#pragma once

#include "MeshOSPrefs.h"

// Work-in-progress reconstruction of the MeshOS full repeater. For now it holds
// the recovered config struct and the spam filter (shared with Repeater Lite),
// so the filter helpers compile against the full build's struct offsets. The
// web interface, transport switching, AirLink, observer and OTA are catalogued
// in FEATURES.md and not yet reconstructed.

#define PUBLIC_CHANNEL_HASH   0x11

class MeshOSFull : public MyMesh {
  MeshOSPrefs prefs;                 // sits at the base-class end -> blocked_n at +0x90
  uint8_t chan_heard[256] = {};
  uint8_t entry_heard[256] = {};
  uint32_t not_repeated = 0;
  uint32_t entry_hits[MESHOS_MAX_ENTRIES] = {};

  bool isChannelBlocked(uint8_t hash) const {
    for (uint8_t i = 0; i < prefs.blocked_n && i < MESHOS_MAX_BLOCKED; i++) {
      if (prefs.blocked[i] == hash) return true;
    }
    return false;
  }
  int matchEntry(const mesh::Packet* packet) const;

protected:
  bool allowPacketForward(const mesh::Packet* packet) override;

public:
  MeshOSFull(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : MyMesh(board, radio, ms, rng, rtc, tables) { }

  bool blockChannel(uint8_t hash);
  bool unblockChannel(uint8_t hash);
};
