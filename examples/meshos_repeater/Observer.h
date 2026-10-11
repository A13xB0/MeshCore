#pragma once

#include "MeshOSFull.h"

// The MeshOS observer (opt-in telemetry, `set observer on`). While enabled and
// WiFi is up, connects to the MeshOS MQTT broker and publishes every heard
// packet plus a signed status heartbeat once a minute.
#ifdef ESP32

class Observer {
  MeshOSFull& _mesh;
  void* _mqtt = NULL;         // PubSubClient* (opaque in the header)
  unsigned long _next_pub = 0;
  bool _was_on = false;
  char _client_id[16] = {};

  bool connect();

  // stats for the status JSON "mqtt" object
  uint32_t _published = 0, _dropped = 0, _connects = 0;
  unsigned long _sign_us = 0;
  bool _connected = false;

public:
  Observer(MeshOSFull& mesh) : _mesh(mesh) { }
  void loop();                // call from the main loop; no-ops unless observer is on + WiFi is up
  String mqttJson();          // the "mqtt" object for /api/status
};

#endif
