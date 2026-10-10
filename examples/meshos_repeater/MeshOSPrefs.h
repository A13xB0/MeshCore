#pragma once

#include "../simple_repeater/MyMesh.h"

// The MeshOS full repeater's config block, recovered field-for-field from
// MeshOS-Repeater-HeltecV4-v1.6.1 by decompiling its structure() serialiser.
// Offsets are relative to the ConfigSerializer base (its vtable+parent occupy
// the first 12 bytes), so the first field lands at +0x0c and blocked_n at +0x90
// -- the same absolute offsets the shipped binary uses, which is what lets the
// spam-filter helpers compile to the author's exact code.

#define MESHOS_MAX_BLOCKED    16
#define MESHOS_MAX_ENTRIES    8
#define MESHOS_MAX_ENTRY_LEN  3

struct MeshOSEntry {
  uint8_t len;
  uint8_t id[MESHOS_MAX_ENTRY_LEN];
};

class MeshOSPrefs : public ConfigSerializer {
public:
  uint8_t  transport = 0;             // +0x0c  ble(0) / wifi(2)
  char     wifi_ssid[33] = {};        // +0x0d
  char     wifi_pwd[64] = {};         // +0x2e
  uint8_t  wifi_lock = 0;             // +0x6e  (single byte before web_pwd)
  char     web_pwd[33] = {};          // +0x6f  dedicated web password
  uint8_t  blocked_n = 0;             // +0x90
  uint8_t  blocked[MESHOS_MAX_BLOCKED] = {};   // +0x91
  uint8_t  entry_n = 0;               // +0xa1
  MeshOSEntry entries[MESHOS_MAX_ENTRIES] = {}; // +0xa2  (32 bytes)
  uint8_t  configured = 0;            // +0xc2
  uint8_t  setup_done = 0;            // +0xc3
  uint8_t  inherited = 0;             // +0xc4
  uint16_t ble_pin = 0;              // +0xc8  (3 bytes padding before it)
  uint8_t  ble_rebond = 0;           // +0xcc
  uint8_t  airlink = 0;              // +0xcd
  uint8_t  airlink_pub = 0;         // +0xce
  char     airlink_ch[96] = {};     // +0xcf
  uint8_t  observer = 0;            // +0x12f

  // def() order below is the author's serialisation order (the order fields are
  // written to /prefs.json), which differs from the memory layout above.
  void structure() override {
    def("transport", transport);
    def("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    def("wifi_pwd", wifi_pwd, sizeof(wifi_pwd));
    def("wifi_lock", wifi_lock);
    def("blocked_n", blocked_n);
    def("blocked", blocked, sizeof(blocked));
    def("web_pwd", web_pwd, sizeof(web_pwd));
    def("configured", configured);
    def("observer", observer);
    def("ble_pin", ble_pin);
    def("ble_rebond", ble_rebond);
    def("setup_done", setup_done);
    def("inherited", inherited);
    def("airlink", airlink);
    def("airlink_pub", airlink_pub);
    def("airlink_ch", airlink_ch, sizeof(airlink_ch));
    def("entry_n", entry_n);
    def("entry", entries, sizeof(entries));
  }
};
