#include "SelfUpdate.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include "MiniJson.h"

// The manifest's version strings have no leading 'v'; FIRMWARE_VERSION does ("v1.17.1").
static const char* runningVersion() {
  return FIRMWARE_VERSION[0] == 'v' ? FIRMWARE_VERSION + 1 : FIRMWARE_VERSION;
}

// Any version other than the running one counts as an update.
static bool isDifferentVersion(const char* version) {
  return strcmp(version, runningVersion()) != 0;
}

// lower-case hex, as sha256 sums are usually written
static void toHexStr(const uint8_t* src, int len, char* dst) {
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (int i = 0; i < len; i++) {
    dst[i * 2]     = HEX_DIGITS[src[i] >> 4];
    dst[i * 2 + 1] = HEX_DIGITS[src[i] & 0x0F];
  }
  dst[len * 2] = 0;
}

static const uint32_t DOWNLOAD_TASK_STACK = 12288;   // TLS + Update need room
static const uint32_t CHECK_TASK_STACK    = 10240;
static const uint32_t STALL_TIMEOUT_MS    = 15000;
static const unsigned long AUTO_CHECK_INTERVAL_MS = 8UL * 3600000UL;   // ~3 times a day

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
  StrHelper::strncpy(_err, why, sizeof(_err));
  _state = FAILED;
  Serial.printf("FW: FAILED: %s\n", why);
}

// --------------------------------------------------------------------------
// Manifest fetch + parse
// --------------------------------------------------------------------------
bool SelfUpdate::fetchManifest(Manifest& out) {
  if (!WiFi.isConnected()) { strcpy(_err, "no network"); return false; }

  WiFiClientSecure client;
  client.setInsecure();             // certificate is not checked; the image sha256 is
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
  if (!minijson::getString(e, "version", out.version, sizeof(out.version))) {
    strcpy(_err, "manifest: entry has no version");
    return false;
  }
  if (!minijson::getString(e, "url", out.url, sizeof(out.url))
      || !minijson::getString(e, "sha256", out.sha256, sizeof(out.sha256))) {
    strcpy(_err, "manifest: entry incomplete");
    return false;
  }
  out.size = minijson::getUint(e, "size");
  minijson::getString(e, "notes", out.notes, sizeof(out.notes));
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
  BaseType_t ok = xTaskCreate(&SelfUpdate::downloadTask, "fw_dl", DOWNLOAD_TASK_STACK, this, 1, &h);
  if (ok != pdPASS) { fail("could not start the update task"); return false; }
  return true;
}

void SelfUpdate::downloadTask(void* arg) {
  SelfUpdate* self = (SelfUpdate*) arg;
  self->runDownload();
  Serial.printf("FW: %s task used %lu of %lu bytes of stack\n", "fw_dl",
                (unsigned long)(DOWNLOAD_TASK_STACK - uxTaskGetStackHighWaterMark(NULL)), (unsigned long) DOWNLOAD_TASK_STACK);
  vTaskDelete(NULL);
}

// Downloads _download_url into the OTA slot, checks its sha256 against
// _want_sha and reboots into it. Runs on its own task; progress is in _done.
void SelfUpdate::runDownload() {
  Serial.printf("FW: downloading %s\n", _download_url);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, _download_url)) { fail("download: begin failed"); return; }
  int code = http.GET();
  if (code != 200) {
    http.end();
    char why[32];
    snprintf(why, sizeof(why), "download: HTTP %d", code);
    fail(why);
    return;
  }

  int len = http.getSize();   // -1 if the server did not say
  if (_total == 0 && len > 0) _total = len;
  if (_total == 0) { http.end(); fail("empty download"); return; }
  if (len > 0 && (uint32_t)len != _total) { http.end(); fail("download size differs from the manifest"); return; }
  if (!Update.begin(_total)) { http.end(); fail("Update.begin failed"); return; }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  const char* error = streamToFlash(http, sha);
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (error) {
    Update.abort();
    fail(error);
    return;
  }

  _state = VERIFYING;
  char got[65];
  toHexStr(digest, sizeof(digest), got);
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

// Copies the HTTP body into the OTA slot, hashing as it goes. Returns NULL on
// success or a description of what went wrong.
const char* SelfUpdate::streamToFlash(HTTPClient& http, mbedtls_sha256_context& sha) {
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  uint32_t last_progress = millis();

  while (http.connected() && _done < _total) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - last_progress > STALL_TIMEOUT_MS) return "download stalled";
      delay(5);
      continue;
    }
    int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n <= 0) continue;
    if (Update.write(buf, n) != (size_t)n) return "flash write failed";
    mbedtls_sha256_update_ret(&sha, buf, n);
    _done += n;
    last_progress = millis();
  }

  if (_done != _total) {
    static char incomplete[64];
    snprintf(incomplete, sizeof(incomplete), "download incomplete (%lu of %lu bytes)",
             (unsigned long)_done, (unsigned long)_total);
    return incomplete;
  }
  return NULL;
}

// --------------------------------------------------------------------------
// CLI
// --------------------------------------------------------------------------
bool SelfUpdate::handleCommand(char* command, char* reply) {
  // ----- app-streamed path -----
  if (memcmp(command, "fw.begin ", 9) == 0) {
    if (_manual_upload) { strcpy(reply, "Err - a manual upload (/update) is in progress"); return true; }
    if (_state == DOWNLOADING || _app_active) { strcpy(reply, "an update is already in progress"); return true; }
    char sha[65];
    unsigned long size = 0;
    if (sscanf(&command[9], "%lu %64s", &size, sha) != 2 || size == 0 || strlen(sha) != 64) {
      strcpy(reply, "usage: fw.begin <size> <sha256>");
      return true;
    }
    if (!Update.begin(size)) { strcpy(reply, "no updater"); return true; }
    mbedtls_sha256_init(&_app_ctx);
    mbedtls_sha256_starts_ret(&_app_ctx, 0);
    _app_active = true;
    _app_total = size;
    _app_done = 0;
    StrHelper::strncpy(_app_sha, sha, sizeof(_app_sha));
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
    Update.abort();
    mbedtls_sha256_free(&_app_ctx);
    _app_active = false;
    strcpy(reply, "update from the app abandoned");
    return true;
  }
  if (strcmp(command, "fw.commit") == 0) {
    if (!_app_active) { strcpy(reply, "no update in progress"); return true; }
    _app_active = false;
    uint8_t digest[32];
    char got[65];
    mbedtls_sha256_finish_ret(&_app_ctx, digest);
    mbedtls_sha256_free(&_app_ctx);
    toHexStr(digest, sizeof(digest), got);
    if (strcasecmp(got, _app_sha) != 0) {
      Update.abort();
      Serial.printf("FW: sha256 %s != announced %s\n", got, _app_sha);
      strcpy(reply, "bad size or sha256");
      return true;
    }
    if (!Update.end(true)) { strcpy(reply, "no update in progress"); return true; }
    Serial.printf("FW: installed %lu bytes from the app, sha256 ok - rebooting\n", (unsigned long)_app_total);
    strcpy(reply, "ok - verified, rebooting into the new firmware");
    delay(400);
    ESP.restart();
    return true;
  }

  // ----- site self-update path -----
  if (strcmp(command, "fw.check") == 0) {
    _state = CHECKING;
    if (runCheck()) {
      strcpy(reply, _avail ? "OK - checking for updates" : "up to date");
    } else {
      _state = IDLE;
      strcpy(reply, "manifest fetch failed");
    }
    return true;
  }
  if (memcmp(command, "fw.update", 9) == 0) {
    if (_manual_upload) { strcpy(reply, "Err - a manual upload (/update) is in progress"); return true; }
    if (_state == DOWNLOADING) { strcpy(reply, "an update is already in progress"); return true; }
    bool force = strstr(command, "force") != NULL;
    if (!_latest.ok) { strcpy(reply, "Err - run fw.check first"); return true; }
    if (!isDifferentVersion(_latest.version) && !force) {
      sprintf(reply, "Err - already up to date (site has v%s, running v%s); 'fw.update force' reinstalls",
              _latest.version, runningVersion());
      return true;
    }
    StrHelper::strncpy(_download_url, _latest.url, sizeof(_download_url));
    StrHelper::strncpy(_want_sha, _latest.sha256, sizeof(_want_sha));
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
    _custom_manifest = strcmp(arg, "default") != 0;
    StrHelper::strncpy(_manifest, _custom_manifest ? arg : FW_MANIFEST_DEFAULT, sizeof(_manifest));
    sprintf(reply, "OK - manifest %s", _manifest);
    return true;
  }
  if (memcmp(command, "fw.install", 10) == 0) {
    if (_state == DOWNLOADING) { strcpy(reply, "an update is already in progress"); return true; }
    char url[160], sha[65];
    unsigned long size = 0;
    int got = sscanf(&command[10], "%159s %64s %lu", url, sha, &size);
    if (got < 2 || strlen(sha) != 64) { strcpy(reply, "Err - usage: fw.install <url> <sha256> [size]"); return true; }
    StrHelper::strncpy(_download_url, url, sizeof(_download_url));
    StrHelper::strncpy(_want_sha, sha, sizeof(_want_sha));
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
    Update.abort();
    mbedtls_sha256_free(&_app_ctx);
    _app_active = false;
    if (reply) strcpy(reply, "flash write failed");
    return false;
  }
  mbedtls_sha256_update_ret(&_app_ctx, data, len);
  _app_done += len;
  return true;
}

// --------------------------------------------------------------------------
// Unattended periodic check (a few times a day) - availability only
// --------------------------------------------------------------------------
void SelfUpdate::loop() {
  if (_manual_upload || _state == DOWNLOADING || _app_active || _checking) return;
  if (!WiFi.isConnected()) return;
  if ((long)(millis() - _next_auto) < 0) return;
  _next_auto = millis() + AUTO_CHECK_INTERVAL_MS;
  _checking = true;
  if (xTaskCreate(&SelfUpdate::checkTask, "fw_chk", CHECK_TASK_STACK, this, 1, NULL) != pdPASS) _checking = false;
}

void SelfUpdate::checkTask(void* arg) {
  SelfUpdate* self = (SelfUpdate*) arg;
  self->runCheck();
  self->_checking = false;
  vTaskDelete(NULL);
}

// Fetches the manifest and records whether an update is available. Never
// installs anything. Returns false if the manifest could not be fetched.
bool SelfUpdate::runCheck() {
  _check_failed = false;
  _err[0] = 0;
  if (!fetchManifest(_latest)) {
    _check_failed = true;
    Serial.printf("FW: check failed: %s\n", _err);
    return false;
  }
  _checked = (long)(_mesh.getRTCClock() ? _mesh.getRTCClock()->getCurrentTime() : 0);
  _avail = isDifferentVersion(_latest.version);
  if (_state == IDLE || _state == AVAILABLE || _state == CHECKING) _state = _avail ? AVAILABLE : IDLE;
  Serial.printf("FW: running %s, site has %s (%lu bytes) -> %s\n",
                FIRMWARE_VERSION, _latest.version, (unsigned long)_latest.size,
                _avail ? "UPDATE AVAILABLE" : "up to date");
  return true;
}

String SelfUpdate::statusJson() {
  int pct = (_total > 0) ? (int)((uint64_t)_done * 100 / _total) : 0;
  char buf[360];
  snprintf(buf, sizeof(buf),
    "{\"cur\":\"%s\",\"key\":\"%s\",\"state\":\"%s\",\"pct\":%d,\"done\":%lu,\"total\":%lu,"
    "\"avail\":%s,\"checked\":%ld,\"check_failed\":%s,\"latest\":\"%s\",\"size\":%lu,"
    "\"notes\":\"%s\",\"err\":\"%s\",\"custom\":%s}",
    runningVersion(), FW_BOARD_KEY, stateName(_state), pct,
    (unsigned long)_done, (unsigned long)_total,
    _avail ? "true" : "false", _checked, _check_failed ? "true" : "false",
    _latest.ok ? _latest.version : "", (unsigned long)_latest.size,
    _latest.ok ? _latest.notes : "", _err, _custom_manifest ? "true" : "false");
  return String(buf);
}

#endif // ESP32
