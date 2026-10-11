#include "EspNet.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

void EspNet::beginSTA() {
  MeshOSPrefs& mp = _mesh.meshosPrefs();
  if (!mp.wifi_ssid[0]) return;            // nothing to join
  // keep the config AP up (WebInterface started it) and add a station link
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(mp.wifi_ssid, mp.wifi_pwd);
  _sta_started = true;
}

bool EspNet::staConnected() const {
  return _sta_started && WiFi.status() == WL_CONNECTED;
}

void EspNet::loop() {
  if (!_sta_started) return;

  bool up = (WiFi.status() == WL_CONNECTED);
  if (up && !_was_connected) {
    // just (re)connected - (re)arm NTP
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);   // UTC; offsets handled as epoch
    _ntp_started = true;
    _synced = false;
    _next_ntp_check = millis() + 2000;
  }
  _was_connected = up;
  if (!up || !_ntp_started) return;

  if ((long)(millis() - _next_ntp_check) < 0) return;
  // back off once synced (hourly), poll every few seconds until then
  _next_ntp_check = millis() + (_synced ? 3600000UL : 5000UL);

  time_t now = time(NULL);
  if ((uint32_t)now < NTP_SANE_EPOCH) return;       // NTP not answered yet

  uint32_t ntp = (uint32_t)now;
  uint32_t rtc = _mesh.getRTCClock()->getCurrentTime();
  if (ntp > rtc + 2) {                               // only ever move forward
    _mesh.getRTCClock()->setCurrentTime(ntp);
    Serial.printf("NET: clock set from NTP (%lu, was %lu)\n", (unsigned long)ntp, (unsigned long)rtc);
  }
  _synced = true;
}

#endif // ESP32
