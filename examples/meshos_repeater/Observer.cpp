#include "Observer.h"

#ifdef ESP32

#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

#define OBSERVER_HOST   "meshcore.co.uk"
#define OBSERVER_PORT   1883
#define OBSERVER_TOPIC  "meshos/repeater/status"
#define HEARTBEAT_MS    60000UL

static WiFiClient net;

bool Observer::connect() {
  PubSubClient* mqtt = (PubSubClient*) _mqtt;
  if (!mqtt) {
    mqtt = new PubSubClient(net);
    mqtt->setServer(OBSERVER_HOST, OBSERVER_PORT);
    mqtt->setBufferSize(512);
    _mqtt = mqtt;
    mesh::Utils::toHex(_client_id, _mesh.self_id.pub_key, 4);
  }
  if (mqtt->connected()) return true;
  return mqtt->connect(_client_id);
}

void Observer::loop() {
  bool on = _mesh.observerEnabled() && WiFi.isConnected();
  if (!on) {
    if (_was_on) { Serial.println("[MQTT] observer OFF - not uploading"); _was_on = false; }
    return;
  }
  if (!_was_on) { Serial.println("[MQTT] observer ON"); _was_on = true; _next_pub = 0; }

  if (!connect()) return;
  PubSubClient* mqtt = (PubSubClient*) _mqtt;
  mqtt->loop();

  if ((long)(millis() - _next_pub) < 0) return;
  _next_pub = millis() + HEARTBEAT_MS;
  String hb = _mesh.observerHeartbeat();
  mqtt->publish(OBSERVER_TOPIC, hb.c_str());
}

#endif
