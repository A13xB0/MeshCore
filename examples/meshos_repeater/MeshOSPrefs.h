#pragma once

#include "../simple_repeater/MyMesh.h"
#include "../meshos_shared/SpamFilter.h"

// MeshOS settings of the full repeater, saved to /meshos_net.json alongside the
// stock MeshCore /prefs.json. Most of them apply after a reboot.

enum Transport : uint8_t {
  TRANSPORT_BLE  = 0,   // admin link over Bluetooth (the MeshOS app)
  TRANSPORT_WIFI = 2,   // WiFi access point + web UI, and joins wifi_ssid if set
};

inline const char* transportName(uint8_t transport) {
  return transport == TRANSPORT_WIFI ? "wifi" : "ble";
}

#define DEFAULT_BLE_PIN   123456   // used while ble_pin is 0 (never set)

class MeshOSPrefs : public ConfigSerializer {
public:
  uint8_t  transport = TRANSPORT_BLE;
  char     wifi_ssid[33] = {};
  char     wifi_pwd[64] = {};
  uint8_t  wifi_lock = 0;
  char     web_pwd[33] = {};        // web UI password; empty = none
  FilterLists filter;               // spam filter block lists
  uint8_t  configured = 0;          // settings have been saved at least once
  uint8_t  setup_done = 0;          // the web setup wizard has been completed
  uint8_t  inherited = 0;           // WiFi settings were copied from a companion image
  uint16_t ble_pin = 0;             // 0 = DEFAULT_BLE_PIN
  uint8_t  ble_rebond = 0;
  uint8_t  airlink = 0;             // run the AirLink phone bridge
  uint8_t  airlink_pub = 0;         // ... and pass the public channel over it
  char     airlink_ch[96] = {};     // ... plus these #hashtag channels (comma separated)
  uint8_t  observer = 0;            // publish telemetry to the MeshOS MQTT observer

  uint32_t blePin() const { return ble_pin ? ble_pin : DEFAULT_BLE_PIN; }

  void structure() override {
    def("transport", transport);
    def("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    def("wifi_pwd", wifi_pwd, sizeof(wifi_pwd));
    def("wifi_lock", wifi_lock);
    def("blocked_n", filter.channel_count);
    def("blocked", filter.channels, sizeof(filter.channels));
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
    def("entry_n", filter.entry_count);
    def("entry", filter.entries, sizeof(filter.entries));
  }
};
