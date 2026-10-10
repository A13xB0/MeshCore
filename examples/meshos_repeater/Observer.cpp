#include "Observer.h"

#ifdef ESP32

#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

// All recovered from the shipped firmware's string table (the MQTT broker is a
// bare IP, not meshcore.co.uk which is only the web/API host). The username and
// password are the shared device credentials embedded in every MeshOS repeater
// build.
#define OBSERVER_HOST     "167.99.90.25"
#define OBSERVER_PORT     1883
#define OBSERVER_USER     "meshos-device"
#define OBSERVER_PASS     "8b680d584757980d1808b938cedbccfc"
#define CLIENT_PREFIX     "meshosrpt-"             // meshosrpt-<id>
#define TOPIC_STATUS_FMT  "meshos/rpt/%s/status"
#define TOPIC_RX_FMT      "meshos/rpt/%s/rx"
#define TOPIC_CONTROL     "meshos/chat/control"
#define HEARTBEAT_MS      60000UL

static WiFiClient net;

static void mqttControl(char* topic, uint8_t* payload, unsigned int len) {
  Serial.print("[MQTT] control: ");
  Serial.write(payload, len);
  Serial.println();
}

bool Observer::connect() {
  PubSubClient* mqtt = (PubSubClient*) _mqtt;
  if (!mqtt) {
    mqtt = new PubSubClient(net);
    mqtt->setServer(OBSERVER_HOST, OBSERVER_PORT);
    mqtt->setBufferSize(512);
    mqtt->setCallback(mqttControl);
    _mqtt = mqtt;
    char id[20];
    mesh::Utils::toHex(id, _mesh.self_id.pub_key, 3);
    snprintf(_client_id, sizeof(_client_id), CLIENT_PREFIX "%s", id);
  }
  if (mqtt->connected()) return true;
  if (mqtt->connect(_client_id, OBSERVER_USER, OBSERVER_PASS)) {
    Serial.printf("[MQTT] connected as %s -> %s\n", _client_id, OBSERVER_HOST);
    mqtt->subscribe(TOPIC_CONTROL);
    return true;
  }
  Serial.printf("[MQTT] connect failed rc=%d\n", mqtt->state());
  return false;
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

  char id[20];
  mesh::Utils::toHex(id, _mesh.self_id.pub_key, 3);
  char topic[32];
  snprintf(topic, sizeof(topic), TOPIC_STATUS_FMT, id);
  String hb = _mesh.observerHeartbeat();
  mqtt->publish(topic, hb.c_str());
}

#endif
