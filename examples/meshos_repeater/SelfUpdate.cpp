#include "SelfUpdate.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

// ---- tiny JSON field readers (good enough for the flat manifest entries) ----
// Find `"key"` after `from`, then the start of its value. Returns NULL if absent.
static const char* jsonValue(const char* obj, const char* key) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(obj, pat);
  if (!p) return NULL;
  p = strchr(p + strlen(pat), ':');
  if (!p) return NULL;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}
static bool jsonStr(const char* obj, const char* key, char* out, size_t outsz) {
  const char* p = jsonValue(obj, key);
  if (!p || *p != '"') return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n < outsz - 1) out[n++] = *p++;
  out[n] = 0;
  return true;
}
static uint32_t jsonNum(const char* obj, const char* key) {
  const char* p = jsonValue(obj, key);
  return p ? (uint32_t) strtoul(p, NULL, 10) : 0;
}

static void toHexStr(const uint8_t* src, int len, char* dst) {
  static const char* H = "0123456789abcdef";
  for (int i = 0; i < len; i++) { dst[i*2] = H[src[i] >> 4]; dst[i*2+1] = H[src[i] & 0xf]; }
  dst[len*2] = 0;
}

const char* SelfUpdate::stateName(State s) {
  switch (s) {
    case IDLE:        return "idle";
    case CHECKING:    return "checking";
    case AVAILABLE:   return "available";
    case DOWNLOADING: return "downloading";
    case VERIFYING:   return "verifying";
    case INSTALLED:   return "installed";
    default:          return "failed";
  }
}

void SelfUpdate::fail(const char* why) {
  strncpy(_err, why, sizeof(_err) - 1);
  _err[sizeof(_err)-1] = 0;
  _state = FAILED_;
  Serial.printf("FW: FAILED: %s\n", why);
}

// --------------------------------------------------------------------------
// Manifest fetch + parse
// --------------------------------------------------------------------------
bool SelfUpdate::fetchManifest(Manifest& out) {
  if (!WiFi.isConnected()) { strcpy(_err, "no network"); return false; }

  WiFiClientSecure client;
  client.setInsecure();             // the reconstruction does not pin the cert
  HTTPClient http;
  Serial.printf("FW: checking %s\n", _manifest);
  if (!http.begin(client, _manifest)) { strcpy(_err, "manifest: begin failed"); return false; }
  int code = http.GET();
  if (code != 200) {
    snprintf(_err, sizeof(_err), "manifest: HTTP %d", code);
    Serial.printf("manifest: HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  if (body.length() == 0) { strcpy(_err, "manifest: empty reply"); return false; }

  // locate this board's object: "rpt_heltec_v4":{ ... }
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\"", FW_BOARD_KEY);
  int at = body.indexOf(pat);
  if (at < 0) { snprintf(_err, sizeof(_err), "no %s entry in the manifest yet", FW_BOARD_KEY); return false; }
  int brace = body.indexOf('{', at);
  int close = body.indexOf('}', brace);
  if (brace < 0 || close < 0) { strcpy(_err, "manifest: bad entry"); return false; }
  String entry = body.substring(brace, close + 1);
  const char* e = entry.c_str();

  out = Manifest();
  if (!jsonStr(e, "version", out.version, sizeof(out.version))) { strcpy(_err, "manifest: entry has no version"); return false; }
  if (!jsonStr(e, "url", out.url, sizeof(out.url)) || !jsonStr(e, "sha256", out.sha256, sizeof(out.sha256))) {
    strcpy(_err, "manifest: entry incomplete"); return false;
  }
  out.size = jsonNum(e, "size");
  jsonStr(e, "notes", out.notes, sizeof(out.notes));
  out.ok = true;
  return true;
}

// --------------------------------------------------------------------------
// Background download + install (site / direct-install paths)
// --------------------------------------------------------------------------
bool SelfUpdate::startDownloadTask() {
  _state = DOWNLOADING;
  _done = 0;
  TaskHandle_t h = NULL;
  // generous stack: TLS + Update need room
  BaseType_t ok = xTaskCreate(&SelfUpdate::downloadTask, "fw_dl", 12288, this, 1, &h);
  if (ok != pdPASS) { fail("could not start the update task"); return false; }
  return true;
}

void SelfUpdate::downloadTask(void* arg) {
  SelfUpdate* self = (SelfUpdate*) arg;
  self->runDownload();
  Serial.printf("FW: %s task used %lu of %lu bytes of stack\n", "fw_dl",
                (unsigned long)(12288 - uxTaskGetStackHighWaterMark(NULL)), 12288UL);
  vTaskDelete(NULL);
}

void SelfUpdate::runDownload() {
  Serial.printf("FW: downloading %s\n", _key);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, _key)) { fail("download: begin failed"); return; }
  int code = http.GET();
  if (code != 200) { snprintf(_err, sizeof(_err), "download: HTTP %d", code); Serial.println(_err); _state = FAILED_; http.end(); return; }

  int len = http.getSize();
  if (_total == 0 && len > 0) _total = len;
  if (_total == 0) { http.end(); fail("empty download"); return; }
  if (len > 0 && (uint32_t)len != _total) { http.end(); fail("download size differs from the manifest"); return; }

  if (!Update.begin(_total)) { http.end(); fail("Update.begin failed"); return; }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  uint32_t last_progress = millis();
  while (http.connected() && _done < _total) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - last_progress > 15000) { Update.abort(); mbedtls_sha256_free(&ctx); http.end(); fail("download stalled"); return; }
      delay(5);
      continue;
    }
    int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n <= 0) continue;
    if (Update.write(buf, n) != (size_t)n) { Update.abort(); mbedtls_sha256_free(&ctx); http.end(); fail("flash write failed"); return; }
    mbedtls_sha256_update_ret(&ctx, buf, n);
    _done += n;
    last_progress = millis();
  }
  http.end();

  if (_done != _total) {
    Update.abort(); mbedtls_sha256_free(&ctx);
    snprintf(_err, sizeof(_err), "download incomplete (%lu of %lu bytes)", (unsigned long)_done, (unsigned long)_total);
    Serial.println(_err); _state = FAILED_; return;
  }

  _state = VERIFYING;
  uint8_t digest[32]; char got[65];
  mbedtls_sha256_finish_ret(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  toHexStr(digest, 32, got);

  if (strcasecmp(got, _want_sha) != 0) {
    Update.abort();
    Serial.printf("FW: sha256 %s != manifest %s\n", got, _want_sha);
    fail("sha256 mismatch");
    return;
  }
  if (!Update.end(true)) { fail("Update.end failed"); return; }

  _state = INSTALLED;
  Serial.printf("FW: installed %lu bytes, sha256 ok - rebooting\n", (unsigned long)_total);
  delay(400);
  ESP.restart();
}

// --------------------------------------------------------------------------
// CLI
// --------------------------------------------------------------------------
bool SelfUpdate::handleCommand(char* command, char* reply) {
  // ----- app-streamed path -----
  if (memcmp(command, "fw.begin ", 9) == 0) {
    if (_manual_upload) { strcpy(reply, "Err - a manual upload (/update) is in progress"); return true; }
    if (_state == DOWNLOADING || _app_active) { strcpy(reply, "an update is already in progress"); return true; }
    char sha[65]; unsigned long size = 0;
    if (sscanf(&command[9], "%lu %64s", &size, sha) != 2 || size == 0 || strlen(sha) != 64) {
      strcpy(reply, "usage: fw.begin <size> <sha256>"); return true;
    }
    if (!Update.begin(size)) { strcpy(reply, "no updater"); return true; }
    mbedtls_sha256_init(&_app_ctx);
    mbedtls_sha256_starts_ret(&_app_ctx, 0);
    _app_active = true; _app_total = size; _app_done = 0;
    strncpy(_app_sha, sha, sizeof(_app_sha)); _app_sha[64] = 0;
    Serial.printf("FW: receiving %lu bytes from the app\n", (unsigned long)size);
    strcpy(reply, "ok");
    return true;
  }
  if (strcmp(command, "fw.ack") == 0) {
    if (!_app_active) { strcpy(reply, "no update in progress"); return true; }
    sprintf(reply, "ok %lu/%lu", (unsigned long)_app_done, (unsigned long)_app_total);
    return true;
  }
  if (strcmp(command, "fw.abort") == 0) {
    if (!_app_active) { strcpy(reply, "no update in progress"); return true; }
    Update.abort(); mbedtls_sha256_free(&_app_ctx); _app_active = false;
    strcpy(reply, "update from the app abandoned");
    return true;
  }
  if (strcmp(command, "fw.commit") == 0) {
    if (!_app_active) { strcpy(reply, "no update in progress"); return true; }
    uint8_t digest[32]; char got[65];
    mbedtls_sha256_finish_ret(&_app_ctx, digest); mbedtls_sha256_free(&_app_ctx);
    toHexStr(digest, 32, got);
    if (strcasecmp(got, _app_sha) != 0) {
      Update.abort(); _app_active = false;
      Serial.printf("FW: sha256 %s != announced %s\n", got, _app_sha);
      strcpy(reply, "bad size or sha256"); return true;
    }
    if (!Update.end(true)) { _app_active = false; strcpy(reply, "no update in progress"); return true; }
    _app_active = false;
    Serial.printf("FW: installed %lu bytes from the app, sha256 ok - rebooting\n", (unsigned long)_app_total);
    strcpy(reply, "ok - verified, rebooting into the new firmware");
    delay(400); ESP.restart();
    return true;
  }

  // ----- site self-update path -----
  if (strcmp(command, "fw.check") == 0) {
    _state = CHECKING; _check_failed = false; _err[0] = 0;
    if (fetchManifest(_latest)) {
      _checked = (long)(_mesh.getRTCClock() ? _mesh.getRTCClock()->getCurrentTime() : 0);
      bool newer = strcmp(_latest.version, FIRMWARE_VERSION + 1) != 0;  // FIRMWARE_VERSION is "v1.17.1"
      _avail = newer;
      _state = newer ? AVAILABLE : IDLE;
      Serial.printf("FW: running %s, site has %s (%lu bytes) -> %s\n",
                    FIRMWARE_VERSION, _latest.version, (unsigned long)_latest.size, newer ? "UPDATE AVAILABLE" : "up to date");
      strcpy(reply, newer ? "OK - checking for updates" : "up to date");
    } else {
      _check_failed = true; _state = IDLE;
      Serial.printf("FW: check failed: %s\n", _err);
      strcpy(reply, "manifest fetch failed");
    }
    return true;
  }
  if (memcmp(command, "fw.update", 9) == 0) {
    if (_manual_upload) { strcpy(reply, "Err - a manual upload (/update) is in progress"); return true; }
    if (_state == DOWNLOADING) { strcpy(reply, "an update is already in progress"); return true; }
    bool force = strstr(command, "force") != NULL;
    if (!_latest.ok) { strcpy(reply, "Err - run fw.check first"); return true; }
    bool newer = strcmp(_latest.version, FIRMWARE_VERSION + 1) != 0;
    if (!newer && !force) {
      sprintf(reply, "Err - already up to date (site has v%s, running v%s); 'fw.update force' reinstalls",
              _latest.version, FIRMWARE_VERSION + 1);
      return true;
    }
    strncpy(_key, _latest.url, sizeof(_key)); _key[sizeof(_key)-1] = 0;
    strncpy(_want_sha, _latest.sha256, sizeof(_want_sha)); _want_sha[64] = 0;
    _total = _latest.size;
    startDownloadTask();
    strcpy(reply, "OK - checking for updates");
    return true;
  }
  if (strcmp(command, "fw.status") == 0) {
    strncpy(reply, statusJson().c_str(), 159); reply[159] = 0;
    return true;
  }
  if (memcmp(command, "fw.manifest", 11) == 0) {
    const char* arg = command[11] == ' ' ? &command[12] : "";
    if (!*arg) { strcpy(reply, "Err - usage: fw.manifest <url>|default"); return true; }
    if (strcmp(arg, "default") == 0) { strcpy(_manifest, FW_MANIFEST_DEFAULT); _custom_manifest = false; }
    else { strncpy(_manifest, arg, sizeof(_manifest)-1); _manifest[sizeof(_manifest)-1] = 0; _custom_manifest = true; }
    sprintf(reply, "OK - manifest %s", _manifest);
    return true;
  }
  if (memcmp(command, "fw.install", 10) == 0) {
    if (_state == DOWNLOADING) { strcpy(reply, "an update is already in progress"); return true; }
    char url[160], sha[65]; unsigned long size = 0;
    int got = sscanf(&command[10], "%159s %64s %lu", url, sha, &size);
    if (got < 2 || strlen(sha) != 64) { strcpy(reply, "Err - usage: fw.install <url> <sha256> [size]"); return true; }
    strncpy(_key, url, sizeof(_key)); _key[sizeof(_key)-1] = 0;
    strncpy(_want_sha, sha, sizeof(_want_sha)); _want_sha[64] = 0;
    _total = (got == 3) ? (uint32_t)size : 0;
    startDownloadTask();
    strcpy(reply, "OK - checking for updates");
    return true;
  }
  if (memcmp(command, "fw.", 3) == 0) {
    strcpy(reply, "Err - fw.check | fw.update [force] | fw.status | fw.manifest <url>|default | fw.install <url> <sha256> [size]");
    return true;
  }
  return false;
}

// Write one app-streamed chunk (called from the companion data path).
bool SelfUpdate::writeAppChunk(const uint8_t* data, size_t len, char* reply) {
  if (!_app_active) { if (reply) strcpy(reply, "no update in progress"); return false; }
  if (_app_done + len > _app_total) len = _app_total - _app_done;
  if (len == 0) return true;
  if (Update.write((uint8_t*)data, len) != len) {
    Update.abort(); mbedtls_sha256_free(&_app_ctx); _app_active = false;
    if (reply) strcpy(reply, "flash write failed");
    return false;
  }
  mbedtls_sha256_update_ret(&_app_ctx, data, len);
  _app_done += len;
  return true;
}

String SelfUpdate::statusJson() {
  int pct = (_total > 0) ? (int)((uint64_t)_done * 100 / _total) : 0;
  char buf[360];
  snprintf(buf, sizeof(buf),
    "{\"cur\":\"%s\",\"key\":\"%s\",\"state\":\"%s\",\"pct\":%d,\"done\":%lu,\"total\":%lu,"
    "\"avail\":%s,\"checked\":%ld,\"check_failed\":%s,\"latest\":\"%s\",\"size\":%lu,"
    "\"notes\":\"%s\",\"err\":\"%s\",\"custom\":%s}",
    FIRMWARE_VERSION + 1, FW_BOARD_KEY, stateName(_state), pct,
    (unsigned long)_done, (unsigned long)_total,
    _avail ? "true" : "false", _checked, _check_failed ? "true" : "false",
    _latest.ok ? _latest.version : "", (unsigned long)_latest.size,
    _latest.ok ? _latest.notes : "", _err, _custom_manifest ? "true" : "false");
  return String(buf);
}

#endif // ESP32
