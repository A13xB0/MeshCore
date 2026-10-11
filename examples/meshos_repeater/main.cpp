#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MeshOSFull.h"
#ifdef ESP32
#include "WebInterface.h"
#include "Observer.h"
#include "AirLink.h"
#include "SelfUpdate.h"
#include "EspNet.h"
#include <helpers/esp32/SerialBLEInterface.h>
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(board, display);
#endif

#ifdef ETHERNET_ENABLED
  #define ETHERNET_CLI_BANNER "MeshCore Repeater CLI"
  #include <helpers/nrf52/EthernetCLI.h>
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MeshOSFull the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);
#ifdef ESP32
static WebInterface web_iface(the_mesh);
static SerialBLEInterface ble_iface;
static Observer observer(the_mesh);
static AirLink airlink(the_mesh, the_mesh.getNodePrefs(), the_mesh.getPacketManager(), the_mesh.getRTCClock());
static SelfUpdate self_update(the_mesh);
static EspNet esp_net(the_mesh);
static char ble_cmd[180];
static char ble_reply[180];
#endif

void halt() {
  while (1) ;
}

#ifdef ESP32
#include <WiFi.h>
#include <esp_gap_ble_api.h>
static String jbool(bool b) { return b ? "true" : "false"; }

// The /api/status JSON the web UI polls. Built here rather than in
// WebInterface because it reports on every subsystem instance below.
static String buildStatusJson() {
  NodePrefs* p = the_mesh.getNodePrefs();
  char id[20], pub[72];
  mesh::Utils::toHex(id, the_mesh.self_id.pub_key, 3);
  mesh::Utils::toHex(pub, the_mesh.self_id.pub_key, PUB_KEY_SIZE);

  uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
  DateTime dt(now);
  char clk[24];
  snprintf(clk, sizeof(clk), "%02d:%02d - %02d/%02d/%04d",
           dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());

  bool wifi_up = (WiFi.status() == WL_CONNECTED);
  String wifi_ip = wifi_up ? WiFi.localIP().toString() : String("");
  String ap_ip = WiFi.softAPIP().toString();
  const char* transport = the_mesh.transportName();
  bool wifi_mode = the_mesh.isWifiTransport();

  char hdr[640];
  snprintf(hdr, sizeof(hdr),
    "{\"name\":\"%s\",\"id\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\",\"hw\":\"%s\",\"build\":\"%s\","
    "\"role\":\"repeater\",\"uptime\":%lu,\"batt_mv\":%u,\"heap\":{\"free\":%u,\"min\":%u,\"block\":%u},"
    "\"time\":%lu,\"clock\":\"%s\",\"setup\":%s,\"inherited\":%s,\"transport\":\"%s\",\"running\":\"%s\",",
    p->node_name, id, MESHOS_VERSION, MESHOS_BOARD_NAME, MESHOS_HW_NAME, FIRMWARE_BUILD_DATE,
    (unsigned long)(millis()/1000), board.getBattMilliVolts(),
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
    (unsigned long)now, clk, jbool(the_mesh.setupDone()).c_str(), jbool(the_mesh.inheritedPrefs()).c_str(),
    transport, transport);

  char net[400];
  snprintf(net, sizeof(net),
    "\"ble\":{\"on\":%s,\"connected\":false,\"bonds\":%d},"
    "\"wifi\":{\"ssid\":\"%s\",\"up\":%s,\"ip\":\"%s\",\"rssi\":%d},"
    "\"eth\":{\"present\":false,\"up\":false,\"ip\":\"\"},"
    "\"ap\":{\"up\":%s,\"ip\":\"%s\"},",
    jbool(!wifi_mode).c_str(), esp_ble_get_bond_device_num(),
    the_mesh.wifiSsid(), jbool(wifi_up).c_str(), wifi_ip.c_str(), wifi_up ? WiFi.RSSI() : 0,
    jbool(wifi_mode).c_str(), ap_ip.c_str());

  char radio[400];
  snprintf(radio, sizeof(radio),
    "\"pub\":\"%s\",\"web\":%s,\"web_pwd_set\":%s,"
    "\"radio\":{\"freq\":%.3f,\"sf\":%d,\"bw\":%.1f,\"cr\":%d,\"tx\":%d},\"lat\":%.5f,\"lon\":%.5f,"
    "\"air\":{\"tx_ms\":%lu,\"rx_ms\":%lu,\"tx5\":%lu,\"rx5\":%lu,\"s5\":%lu,\"tx24\":%lu,\"rx24\":%lu,\"s24\":%lu},"
    "\"pkts\":{\"tx_flood\":%lu,\"tx_direct\":%lu,\"rx_flood\":%lu,\"rx_direct\":%lu},",
    pub, jbool(wifi_mode).c_str(), jbool(the_mesh.webPwdSet()).c_str(),
    p->freq, p->sf, p->bw, p->cr, p->tx_power_dbm, p->node_lat, p->node_lon,
    the_mesh.getTotalAirTime(), the_mesh.getReceiveAirTime(),
    the_mesh.airTx5(), the_mesh.airRx5(), the_mesh.airSent5(),
    the_mesh.airTx24(), the_mesh.airRx24(), the_mesh.airSent24(),
    the_mesh.getNumSentFlood(), the_mesh.getNumSentDirect(),
    the_mesh.getNumRecvFlood(), the_mesh.getNumRecvDirect());

  String j = String(hdr) + net + radio;
  j += "\"fw\":" + self_update.statusJson() + ",";
  j += "\"mqtt\":" + observer.mqttJson() + ",";
  j += "\"sig\":" + the_mesh.sigJson() + ",";
  j += "\"noise\":" + String(radio_driver.getNoiseFloor()) + ",";
  j += "\"airlink\":{" + airlink.statsJson() + "}}";
  return j;
}

// /api/prefs: every editable setting. Drives the Settings tab and the setup
// wizard's radio/network fields.
static String buildPrefsJson() {
  NodePrefs* p = the_mesh.getNodePrefs();
  MeshOSPrefs& mp = the_mesh.meshosPrefs();
  char a[520];
  snprintf(a, sizeof(a),
    "{\"name\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"owner\":\"%s\",\"guest\":\"%s\","
    "\"freq\":%.4f,\"bw\":%.2f,\"sf\":%d,\"cr\":%d,\"tx\":%d,\"rxgain\":%d,\"repeat\":%d,"
    "\"adv_int\":%d,\"fadv_int\":%d,\"fmax\":%d,\"fmax_uns\":%d,\"fmax_adv\":%d,\"loop\":%d,"
    "\"hash_mode\":%d,\"af\":%.3f,\"rxdelay\":%.3f,\"txdelay\":%.3f,\"dtxdelay\":%.3f,",
    p->node_name, p->node_lat, p->node_lon, p->owner_info, p->guest_password,
    p->freq, p->bw, p->sf, p->cr, p->tx_power_dbm, p->rx_boosted_gain, p->disable_fwd ? 0 : 1,
    p->advert_interval, p->flood_advert_interval, p->flood_max, p->flood_max_unscoped,
    p->flood_max_advert, p->loop_detect, p->path_hash_mode,
    p->airtime_factor, p->rx_delay_base, p->tx_delay_factor, p->direct_tx_delay_factor);

  char b[400];
  snprintf(b, sizeof(b),
    "\"int_thresh\":%d,\"cad\":%d,\"agc\":%d,\"multi_acks\":%d,\"adc_mult\":%.4f,"
    "\"transport\":\"%s\",\"ssid\":\"%s\",\"observer\":%d,\"ble_pin\":\"%06lu\",\"hw\":\"%s\","
    "\"fem_rx_ctl\":false,\"fem_rx\":%d,\"fem_tx_ctl\":false,\"fem_tx\":%d,"
    "\"airlink\":%d,\"airlink_pub\":%d,\"airlink_ch\":\"%s\"}",
    p->interference_threshold, p->cad_enabled, p->agc_reset_interval, p->multi_acks, p->adc_multiplier,
    transportName(mp.transport), mp.wifi_ssid, mp.observer, (unsigned long) mp.blePin(), MESHOS_HW_NAME,
    p->radio_fem_rxgain, p->radio_fem_txgain,
    mp.airlink, mp.airlink_pub, mp.airlink_ch);

  return String(a) + b;
}

// MeshOS commands that need the subsystem instances held here (AirLink,
// self-update, WiFi) rather than MeshOSFull. Returns true if it handled `cmd`.
static bool handleMeshOSCli(char* cmd, char* reply) {
  if (strcmp(cmd, "airlink.status") == 0 || strcmp(cmd, "airlink.log") == 0) {
    strcpy(reply, airlink.statusLine().c_str());
    return true;
  }
  if (self_update.handleCommand(cmd, reply)) return true;

  bool wifi_up = (WiFi.status() == WL_CONNECTED);
  if (strcmp(cmd, "get ip") == 0) {
    if (wifi_up) sprintf(reply, "> %s", WiFi.localIP().toString().c_str());
    else strcpy(reply, "> not connected");
    return true;
  }
  if (strcmp(cmd, "get net") == 0) {
    String wifi = wifi_up ? "up " + WiFi.localIP().toString() : String("down");
    snprintf(reply, 160, "> transport %s, wifi %s, ap %s",
             the_mesh.transportName(), wifi.c_str(), WiFi.softAPIP().toString().c_str());
    return true;
  }
  return false;
}
#endif

static char command[160];
#ifdef ETHERNET_ENABLED
static char ethernet_command[160];
#endif

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;
#ifdef ETHERNET_ENABLED
  ethernet_command[0] = 0;
#endif

  sensors.begin();

  the_mesh.begin(fs);
  the_mesh.beginFull(fs);
#ifdef ESP32
  if (the_mesh.isWifiTransport()) {
    web_iface.setStatusProvider(buildStatusJson);
    web_iface.setPrefsProvider(buildPrefsJson);
    web_iface.setCmdHook(handleMeshOSCli);
    web_iface.begin("MeshOS Repeater", "");
    esp_net.beginSTA();   // join the saved WiFi (station) for observer/OTA/NTP
  } else {
    // Default BLE transport: the MeshOS app connects and sends CLI commands.
    ble_iface.begin("MeshCore-", the_mesh.getNodePrefs()->node_name, the_mesh.getBlePin());
    ble_iface.enable();
  }
  // AirLink bridge (phones <-> LoRa). Works in both Bluetooth and WiFi mode, on
  // boards with PSRAM; shares the one BLE stack with the CLI interface above.
  if (the_mesh.airlinkEnabled()) {
    airlink.begin();
    the_mesh.setAirLink(&airlink);
  }
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#ifdef ETHERNET_ENABLED
  ethernet_start_task();
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

void loop() {
  // Handle Serial CLI
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    reply[0] = 0;
#ifdef ESP32
    if (!handleMeshOSCli(command, reply))
#endif
#ifdef ETHERNET_ENABLED
    if (!ethernet_handle_command(command, reply)) {
      the_mesh.handleCommand(0, command, reply);
    }
#else
    the_mesh.handleCommand(0, command, reply);
#endif
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#ifdef ETHERNET_ENABLED
  ethernet_loop_maintain();
  if (ethernet_read_line(ethernet_command, sizeof(ethernet_command))) {
    char reply[160];
    reply[0] = 0;
    if (!ethernet_handle_command(ethernet_command, reply)) {
      the_mesh.handleCommand(0, ethernet_command, reply);
    }
    ethernet_send_reply(reply);
    ethernet_command[0] = 0;
  }
#endif

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_) && !defined(DISPLAY_CLASS)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

#ifdef ESP32
  if (ble_iface.isEnabled()) {
    ble_iface.loop();
    int len = ble_iface.checkRecvFrame((uint8_t*) ble_cmd);
    if (len > 0) {
      ble_cmd[len] = 0;
      ble_reply[0] = 0;
      if (!handleMeshOSCli(ble_cmd, ble_reply))
        the_mesh.handleCommand(0, ble_cmd, ble_reply);
      if (ble_reply[0]) ble_iface.writeFrame((const uint8_t*) ble_reply, strlen(ble_reply));
    }
  }
#endif
  the_mesh.loop();
  the_mesh.meshosTick();
#ifdef ESP32
  esp_net.loop();
  observer.loop();
  airlink.loop();
  self_update.loop();
#endif
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
