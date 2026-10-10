#pragma once

#include "MeshOSFull.h"

// The MeshOS observer: when enabled, connects to the project's MQTT observer at
// meshcore.co.uk and publishes a signed status heartbeat. Off by default and
// operator-controlled (set observer on). Functional reconstruction of the real
// opt-in telemetry feature; the exact broker topic/credentials are the author's.
#ifdef ESP32

class Observer {
  MeshOSFull& _mesh;
  void* _mqtt = NULL;         // PubSubClient* (opaque in the header)
  unsigned long _next_pub = 0;
  bool _was_on = false;
  char _client_id[16];

  bool connect();

public:
  Observer(MeshOSFull& mesh) : _mesh(mesh) { }
  void loop();                // call from the main loop; no-ops unless observer is on + WiFi is up
};

#endif
