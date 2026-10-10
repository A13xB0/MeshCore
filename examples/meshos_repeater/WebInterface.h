#pragma once

#include "MeshOSFull.h"

// The MeshOS repeater's web interface: a WiFi access point serving the embedded
// single-page app (web/index.html) and a small JSON API. This is a functional
// reconstruction of the runtime the shipped firmware starts when transport=wifi;
// the SPA itself is the byte-exact extracted asset.
#ifdef ESP32

class WebInterface {
  MeshOSFull& _mesh;
  void* _server = NULL;   // AsyncWebServer* (opaque here to keep the header light)
  bool _running = false;

public:
  WebInterface(MeshOSFull& mesh) : _mesh(mesh) { }

  // Full /api/status JSON is assembled in main.cpp (where every subsystem is
  // visible) and handed to the server through this provider.
  typedef String (*StatusProvider)();
  void setStatusProvider(StatusProvider fn) { _status_fn = fn; }

  // Bring up a WiFi AP named after the repeater and start the HTTP server.
  void begin(const char* ap_ssid, const char* ap_pwd);
  bool running() const { return _running; }

private:
  StatusProvider _status_fn = NULL;
};

#endif
