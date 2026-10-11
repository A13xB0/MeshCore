#pragma once

#include "MeshOSFull.h"

// ---------------------------------------------------------------------------
// MeshOS WiFi-station + NTP time sync (ESP32)
//
// In WiFi transport mode the repeater runs an access point (for the config web
// UI, served by WebInterface) *and* joins the operator's WiFi as a station, so
// the observer, self-update and NTP can reach the internet. This module brings
// the station link up from the saved wifi_ssid / wifi_pwd and keeps the RTC in
// step with NTP (pool.ntp.org, time.google.com). The clock is only ever moved
// forward.
// ---------------------------------------------------------------------------

#ifdef ESP32

#define NTP_SERVER_1   "pool.ntp.org"
#define NTP_SERVER_2   "time.google.com"
// 1 Jan 2023 - any epoch below this means NTP has not answered yet
#define NTP_SANE_EPOCH 1672531200UL

class EspNet {
  MeshOSFull& _mesh;
  bool _sta_started = false;
  bool _was_connected = false;
  bool _ntp_started = false;
  bool _synced = false;
  unsigned long _next_ntp_check = 0;

public:
  EspNet(MeshOSFull& mesh) : _mesh(mesh) { }

  // Promote the interface to AP+STA and join the saved network (no-op if no SSID).
  void beginSTA();

  // Call from the main loop: (re)arms NTP on connect and syncs the RTC forward.
  void loop();

  bool staConnected() const;
};

#endif // ESP32
