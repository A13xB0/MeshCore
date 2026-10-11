#pragma once

#include "MeshOSFull.h"

// ---------------------------------------------------------------------------
// MeshOS self-update (OTA)
//
// Three install paths:
//
//  1. Site self-update   fw.check / fw.update [force] / fw.status / fw.manifest
//     Fetches the board's entry from a JSON manifest (default
//     https://meshcore.co.uk/fw/firmware.json), compares its version to the
//     running one, and - on fw.update - downloads the .bin over HTTPS, streams
//     it into the OTA slot while hashing it, checks the sha256 against the
//     manifest, and reboots. The download runs on its own FreeRTOS task so the
//     web UI can poll progress via the "fw" object in /api/status.
//
//  2. Direct install     fw.install <url> <sha256> [size]
//     The same download/verify/reboot path against an explicit URL+hash.
//
//  3. App-streamed        fw.begin <size> <sha256> / fw.ack / fw.commit / fw.abort
//     The MeshOS app pushes the .bin in chunks over the companion link; each
//     chunk is written to the OTA slot and folded into the running sha256, and
//     fw.commit verifies the hash and reboots.
//
// A fourth path - the manual ElegantOTA upload at /update - is served by the web
// interface and is independent of this module.
//
// ESP32 only. FW_BOARD_KEY picks this board's entry in the manifest; progress
// is reported through the "fw" object in /api/status.
// ---------------------------------------------------------------------------

#ifdef ESP32

#include <mbedtls/sha256.h>

#ifndef FW_BOARD_KEY
  #define FW_BOARD_KEY   "rpt_heltec_v4"
#endif
#define FW_MANIFEST_DEFAULT  "https://meshcore.co.uk/fw/firmware.json"

class SelfUpdate {
public:
  enum State : uint8_t { IDLE, CHECKING, AVAILABLE, DOWNLOADING, VERIFYING, INSTALLED, FAILED };

  SelfUpdate(MeshOSFull& mesh) : _mesh(mesh) { strcpy(_manifest, FW_MANIFEST_DEFAULT); }

  // CLI dispatch for the fw.* commands. Returns true if it consumed the command.
  bool handleCommand(char* command, char* reply);

  // "fw" object for /api/status (returns a JSON object)
  String statusJson();

  // write one chunk of an app-streamed firmware image (companion data path).
  // Returns false and fills reply on error. On the final byte the caller should
  // issue fw.commit to verify + reboot.
  bool writeAppChunk(const uint8_t* data, size_t len, char* reply);
  bool appStreaming() const { return _app_active; }

  // true while a manual /update (ElegantOTA) upload is in progress - blocks the
  // site updater from fighting it (matches "a manual upload (/update) is in progress")
  void setManualUpload(bool on) { _manual_upload = on; }

  // call from the main loop: runs an unattended manifest check a few times a day
  // (background task; only sets availability, never installs on its own)
  void loop();

private:
  struct Manifest {
    bool   ok = false;
    char   version[24] = {};
    char   url[160] = {};
    char   sha256[65] = {};
    uint32_t size = 0;
    char   notes[96] = {};
  };

  MeshOSFull& _mesh;
  char   _manifest[160];
  bool   _custom_manifest = false;
  Manifest _latest;

  volatile State _state = IDLE;
  volatile uint32_t _done = 0, _total = 0;
  volatile bool _avail = false;
  long   _checked = -1;              // epoch of last successful check, -1 = never
  bool   _check_failed = false;
  char   _err[80] = {};
  char   _download_url[160] = {};   // image URL for the running/queued install
  char   _want_sha[65] = {};
  bool   _manual_upload = false;

  // app-streamed (fw.begin/ack/commit)
  bool   _app_active = false;
  uint32_t _app_total = 0, _app_done = 0;
  char   _app_sha[65] = {};
  mbedtls_sha256_context _app_ctx;

  unsigned long _next_auto = 30000;   // first unattended check ~30 s after boot
  bool   _checking = false;

  bool   fetchManifest(Manifest& out);
  bool   runCheck();                  // manifest fetch + version compare (no install)
  bool   startDownloadTask();
  static void downloadTask(void* arg);
  static void checkTask(void* arg);
  void   runDownload();
  const char* streamToFlash(class HTTPClient& http, mbedtls_sha256_context& sha);
  void   fail(const char* why);
  static const char* stateName(State s);
};

#endif // ESP32
