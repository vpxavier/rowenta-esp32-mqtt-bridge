/*
  ============================================================
  Rowenta Pur Air Connect XL - Complete Firmware
  ============================================================
  Author: Xavier Hang
  ============================================================
  Combines:
    - State reading via UART (replaces the BroadLink module)
    - Button-press simulation (transistors)
    - WiFi + MQTT configuration portal (first-time setup)
    - MQTT client with automatic Home Assistant discovery

  REQUIRED LIBRARY: PubSubClient3 (fork maintained by hmueller01, based on PubSubClient by Nick O'Leary)
  Tools -> Manage Libraries -> search "PubSubClient3"
  (Compatible API: same #include <PubSubClient.h>, same functions)

  FULL WIRING:
    UART (BroadLink module disconnected):
      Rowenta GND    -> ESP32 GND
      Green wire TXD -> GPIO16 (ESP32 receives)
      Red wire RXD   -> GPIO4  (ESP32 transmits)
      Yellow wire +5V -> ESP32 VCC (power)

    Transistors (E->common GND):
      POWER : Base->2k resistor->GPIO18, Collector->POWER spring
      LIGHT : Base->2k resistor->GPIO19, Collector->LIGHT spring
      MODE  : Base->2k resistor->GPIO23, Collector->MODE spring
      TIMER : Base->2k resistor->GPIO5,  Collector->TIMER spring

  Serial command "reset_wifi": clears the config and restarts the portal.
  ============================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

// ---------- Pins ----------
#define PIN_RX 16
#define PIN_TX 4
#define BAUD_MCU 9600
#define FRAME_TIMEOUT_MS 50
#define HEARTBEAT_INTERVAL_MS 10000
#define WDT_TIMEOUT_S 15

#define PIN_POWER 18
#define PIN_LIGHT 19
#define PIN_MODE  23
#define PIN_TIMER 5
#define PRESS_DURATION_MS 250
#define DELAY_BETWEEN_PRESSES_MS 400

// ---------- MQTT Topics ----------
#define TOPIC_AVAIL       "rowenta/purificateur/availability"
#define TOPIC_POWER_STATE "rowenta/purificateur/power/state"
#define TOPIC_POWER_SET   "rowenta/purificateur/power/set"
#define TOPIC_MODE_STATE  "rowenta/purificateur/mode/state"
#define TOPIC_MODE_SET    "rowenta/purificateur/mode/set"
#define TOPIC_LIGHT_STATE "rowenta/purificateur/light/state"
#define TOPIC_LIGHT_SET   "rowenta/purificateur/light/set"
#define TOPIC_TIMER_SET   "rowenta/purificateur/timer/set"
#define TOPIC_AIRQ_STATE  "rowenta/purificateur/qualite_air/state"
#define TOPIC_AIRQ_LEVEL_STATE "rowenta/purificateur/qualite_air_niveau/state"
#define AIRQ_THRESHOLD_DEGRADED 81
#define AIRQ_THRESHOLD_BAD 171

const char* DEVICE_ID = "rowenta_pu6080f0";

// ---------- Global objects ----------
HardwareSerial mcuSerial(2);
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
WiFiClient espClient;
PubSubClient mqtt(espClient);

IPAddress apIP(192, 168, 4, 1);
bool portalMode = false;

struct Config {
  String wifiSsid, wifiPass, mqttHost, mqttUser, mqttPass, otaPass, uiLang;
  int mqttPort;
} config;

// ---------- Known device state (updated via UART) ----------
bool stateKnown = false;
bool statePower = false;
int  stateModeIdx = 0;
int  stateLightIdx = 0;
uint8_t stateAirQuality = 0;

const char* MODES[] = {"Silencieux", "Boost", "Jour", "Nuit"};
const uint8_t MODES_B1[] = {0x01, 0x04, 0x03, 0x02};
const uint8_t MODES_B2[] = {0x01, 0x05, 0x02, 0x01};
const int NUM_MODES = 4;

const char* LIGHTS[] = {"Max", "Faible", "Eteint"};
const int NUM_LIGHTS = 3;

const char* AIR_LEVELS[] = {"Bonne", "Degradee", "Mauvaise"};

// ============================================================
// UI translation (display only — internal MODES/LIGHTS/AIR_LEVELS
// values used for MQTT / matching stay in French)
// ============================================================
String T(const char* fr, const char* en) {
  return (config.uiLang == "en") ? String(en) : String(fr);
}

const char* MODE_LABELS_FR[] = {"Silencieux", "Boost", "Jour", "Nuit"};
const char* MODE_LABELS_EN[] = {"Silent", "Boost", "Day", "Night"};
const char* LIGHT_LABELS_FR[] = {"Max", "Faible", "Eteint"};
const char* LIGHT_LABELS_EN[] = {"Max", "Low", "Off"};
const char* AIR_LEVELS_FR[] = {"Bonne", "Degradee", "Mauvaise"};
const char* AIR_LEVELS_EN[] = {"Good", "Degraded", "Bad"};

const char* modeLabel(int i) { return config.uiLang == "en" ? MODE_LABELS_EN[i] : MODE_LABELS_FR[i]; }
const char* lightLabel(int i) { return config.uiLang == "en" ? LIGHT_LABELS_EN[i] : LIGHT_LABELS_FR[i]; }
const char* airLevelLabel(int i) { return config.uiLang == "en" ? AIR_LEVELS_EN[i] : AIR_LEVELS_FR[i]; }

uint8_t buf[256];
int bufLen = 0;
unsigned long lastByteTime = 0;
uint8_t heartbeatSeq = 0;
unsigned long lastHeartbeat = 0;
unsigned long bootTime = 0;
bool bootFlagCleared = false;
#define DOUBLE_RESET_WINDOW_MS 3000

// ============================================================
// Configuration storage
// ============================================================

// Loads config from flash and checks the double-reset flag in a single NVS access
bool loadConfigAndCheckReset() {
  prefs.begin("rowenta_cfg", false);
  config.wifiSsid = prefs.getString("wifi_ssid", "");
  config.wifiPass = prefs.getString("wifi_pass", "");
  config.mqttHost = prefs.getString("mqtt_host", "");
  config.mqttPort = prefs.getInt("mqtt_port", 1883);
  config.mqttUser = prefs.getString("mqtt_user", "");
  config.mqttPass = prefs.getString("mqtt_pass", "");
  config.otaPass  = prefs.getString("ota_pass", "rowenta_ota");
  config.uiLang   = prefs.getString("ui_lang", "fr");
  bool doubleReset = prefs.getBool("boot_pend", false);
  prefs.putBool("boot_pend", true);
  prefs.end();
  return doubleReset;
}

void saveConfig() {
  prefs.begin("rowenta_cfg", false);
  prefs.putString("wifi_ssid", config.wifiSsid);
  prefs.putString("wifi_pass", config.wifiPass);
  prefs.putString("mqtt_host", config.mqttHost);
  prefs.putInt("mqtt_port", config.mqttPort);
  prefs.putString("mqtt_user", config.mqttUser);
  prefs.putString("mqtt_pass", config.mqttPass);
  prefs.putString("ota_pass", config.otaPass);
  prefs.putString("ui_lang", config.uiLang);
  prefs.end();
}

void clearConfig() {
  prefs.begin("rowenta_cfg", false);
  prefs.clear();
  prefs.end();
}

// ============================================================
// Double-reset detection: resets the OTA password
// ============================================================

void confirmStableBoot() {
  prefs.begin("rowenta_cfg", false);
  prefs.putBool("boot_pend", false);
  prefs.end();
}

// ============================================================
// WiFi/MQTT configuration portal
// ============================================================

String generateHomePage() {
  int networkCount = WiFi.scanNetworks();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Configuration Rowenta</title>";
  html += "<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 12px;}";
  html += "label{display:block;margin-top:14px;font-weight:bold;}";
  html += "input,select{width:100%;padding:8px;margin-top:4px;box-sizing:border-box;}";
  html += "button{margin-top:20px;width:100%;padding:12px;background:#2b7de9;color:white;border:none;border-radius:4px;font-size:16px;}";
  html += "h2{margin-top:28px;}</style></head><body>";
  html += "<h1>Configuration Rowenta</h1><form method='POST' action='/save'>";
  html += "<h2>Reseau WiFi</h2>";
  html += "<label>Reseau detecte</label><select onchange=\"document.getElementById('ssid').value=this.value\">";
  html += "<option value=''>-- choisir --</option>";
  for (int i = 0; i < networkCount; i++) {
    html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  html += "</select>";
  html += "<label>Nom du reseau (SSID)</label><input type='text' id='ssid' name='ssid' required>";
  html += "<label>Mot de passe WiFi</label><input type='password' name='wifi_pass'>";
  html += "<h2>Broker MQTT</h2>";
  html += "<label>Adresse (IP ou nom d'hote)</label><input type='text' name='mqtt_host' required>";
  html += "<label>Port</label><input type='number' name='mqtt_port' value='1883' required>";
  html += "<label>Utilisateur</label><input type='text' name='mqtt_user'>";
  html += "<label>Mot de passe MQTT</label><input type='password' name='mqtt_pass'>";
  html += "<button type='submit'>Enregistrer et redemarrer</button></form></body></html>";
  return html;
}

void handleRoot() { server.send(200, "text/html", generateHomePage()); }

void handleSave() {
  config.wifiSsid = server.arg("ssid");
  config.wifiPass = server.arg("wifi_pass");
  config.mqttHost = server.arg("mqtt_host");
  config.mqttPort = server.arg("mqtt_port").toInt();
  config.mqttUser = server.arg("mqtt_user");
  config.mqttPass = server.arg("mqtt_pass");
  saveConfig();
  server.send(200, "text/html", "<h1>Enregistre</h1><p>Redemarrage...</p>");
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ============================================================
// OTA settings page (reachable at the device's normal IP)
// ============================================================

String generateOtaPage(const char* message = "") {
  int networkCount = WiFi.scanNetworks();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Parametres Rowenta</title>";
  html += "<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 12px;}";
  html += "label{display:block;margin-top:14px;font-weight:bold;}";
  html += "input,select{width:100%;padding:8px;margin-top:4px;box-sizing:border-box;}";
  html += "button{margin-top:20px;width:100%;padding:12px;background:#2b7de9;color:white;border:none;border-radius:4px;font-size:16px;}";
  html += ".msg{background:#d4edda;padding:10px;border-radius:4px;margin-bottom:10px;}";
  html += "h2{margin-top:28px;}hr{margin-top:30px;border:none;border-top:1px solid #ddd;}";
  html += ".langsw{text-align:right;margin-bottom:10px;font-size:13px;}";
  html += ".langsw a{color:#888;text-decoration:none;margin-left:8px;}";
  html += ".langsw a.active{color:#222;font-weight:700;text-decoration:underline;}";
  html += "</style></head><body>";
  html += "<div class='langsw'><a href='/lang?set=fr'" + String(config.uiLang != "en" ? " class='active'" : "") + ">FR</a>";
  html += "<a href='/lang?set=en'" + String(config.uiLang == "en" ? " class='active'" : "") + ">EN</a></div>";
  html += "<h1>" + T("Parametres Rowenta", "Rowenta Settings") + "</h1>";
  if (strlen(message) > 0) html += "<p class='msg'>" + String(message) + "</p>";

  html += "<h2>" + T("Reseau WiFi", "WiFi network") + "</h2>";
  html += "<p style='color:#888;font-size:13px;'>" + T("Reseau actuel : ", "Current network: ") + config.wifiSsid + "</p>";
  html += "<form method='POST' action='/wifi'>";
  html += "<label>" + T("Reseau detecte", "Detected network") + "</label><select onchange=\"document.getElementById('ssid').value=this.value\">";
  html += "<option value=''>-- " + T("choisir", "select") + " --</option>";
  for (int i = 0; i < networkCount; i++) {
    html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  html += "</select>";
  html += "<label>" + T("Nom du reseau (SSID)", "Network name (SSID)") + "</label><input type='text' id='ssid' name='ssid' required>";
  html += "<label>" + T("Mot de passe WiFi", "WiFi password") + "</label><input type='password' name='wifi_pass'>";
  html += "<label>" + T("Mot de passe OTA actuel (requis)", "Current OTA password (required)") + "</label><input type='password' name='current_pass' required>";
  html += "<button type='submit'>" + T("Changer de reseau et redemarrer", "Change network and restart") + "</button></form>";

  html += "<hr><h2>" + T("Mot de passe OTA", "OTA password") + "</h2>";
  html += "<form method='POST' action='/ota'>";
  html += "<label>" + T("Mot de passe OTA actuel (requis)", "Current OTA password (required)") + "</label><input type='password' name='current_pass' required>";
  html += "<label>" + T("Nouveau mot de passe OTA", "New OTA password") + "</label><input type='password' name='ota_pass' minlength='8' required>";
  html += "<label>" + T("Confirmer", "Confirm") + "</label><input type='password' name='ota_pass2' minlength='8' required>";
  html += "<button type='submit'>" + T("Enregistrer et redemarrer", "Save and restart") + "</button></form>";

  html += "<p style='margin-top:30px;color:#888;font-size:13px;'>" + T("IP actuelle : ", "Current IP: ") + WiFi.localIP().toString() + "</p>";
  html += "<a href='/' style='color:#888;font-size:13px;'>&larr; " + T("Retour a l'accueil", "Back to home") + "</a>";
  html += "</body></html>";
  return html;
}

// ============================================================
// Control page: shows current state and lets the user send commands
// ============================================================

String generateControlPage(const char* message = "") {
  const char* MODE_ICONS[] = {"&#128564;", "&#128640;", "&#9728;&#65039;", "&#127769;"};
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Purificateur Rowenta</title>";
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;max-width:420px;margin:0 auto;padding:20px 16px;";
  html += "background:linear-gradient(160deg,#123256,#020509 85%);min-height:100vh;color:#222;}";
  html += "h1{font-size:22px;margin:0 0 18px;color:#f0f4fa;}";
  html += ".card{background:white;border-radius:14px;padding:18px;margin-bottom:16px;box-shadow:0 4px 14px rgba(0,0,0,0.25);}";
  html += ".row{display:flex;justify-content:space-between;align-items:center;padding:9px 0;border-bottom:1px solid #f0f0f0;}";
  html += ".row:last-child{border-bottom:none;}";
  html += ".row .k{color:#888;font-size:14px;}";
  html += ".row .v{font-weight:600;font-size:14px;}";
  html += ".badge{padding:3px 10px;border-radius:20px;font-size:13px;font-weight:600;}";
  html += ".badge.good{background:#d9f2e3;color:#1e8449;}";
  html += ".badge.degraded{background:#fdecd2;color:#b9770e;}";
  html += ".badge.bad{background:#fadbd8;color:#c0392b;}";
  html += "label.title{display:block;font-size:13px;font-weight:600;color:#666;margin:14px 0 6px;text-transform:uppercase;letter-spacing:.03em;}";
  html += ".segmented{display:flex;border-radius:10px;overflow:hidden;border:1px solid #e0e0e0;}";
  html += ".segmented input{display:none;}";
  html += ".segmented label{flex:1;text-align:center;padding:11px 4px;background:#f7f8fa;cursor:pointer;font-size:14px;color:#555;border-right:1px solid #e0e0e0;transition:.15s;}";
  html += ".segmented label:last-child{border-right:none;}";
  html += ".segmented input:checked + label{background:#2b7de9;color:white;font-weight:600;}";
  html += ".segmented.icons label{font-size:22px;padding:10px 4px 6px;line-height:1.1;}";
  html += ".segmented.icons label .lbl{display:block;font-size:11px;margin-top:3px;font-weight:600;}";
  html += "button.ghost{width:100%;padding:12px;margin-top:6px;border:1px solid #e0e0e0;border-radius:10px;font-size:14px;color:#555;background:white;cursor:pointer;}";
  html += ".msg{background:#d9f2e3;color:#1e8449;padding:10px 14px;border-radius:10px;margin-bottom:14px;font-size:14px;}";
  html += "a.link{display:block;text-align:center;margin-top:18px;color:#9fb3cc;font-size:13px;text-decoration:none;}";
  html += ".switch{position:relative;display:inline-block;width:52px;height:30px;flex-shrink:0;}";
  html += ".switch input{opacity:0;width:0;height:0;}";
  html += ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#c0392b;transition:.2s;border-radius:30px;}";
  html += ".slider:before{position:absolute;content:'';height:24px;width:24px;left:3px;bottom:3px;background:white;transition:.2s;border-radius:50%;box-shadow:0 1px 3px rgba(0,0,0,.3);}";
  html += ".switch input:checked + .slider{background:#27ae60;}";
  html += ".switch input:checked + .slider:before{transform:translateX(22px);}";
  html += ".langsw{text-align:right;margin-bottom:6px;}";
  html += ".langsw a{color:#9fb3cc;font-size:12px;text-decoration:none;margin-left:8px;}";
  html += ".langsw a.active{color:white;font-weight:700;text-decoration:underline;}";
  html += "</style></head><body>";
  html += "<div class='langsw'><a href='/lang?set=fr'" + String(config.uiLang != "en" ? " class='active'" : "") + ">FR</a>";
  html += "<a href='/lang?set=en'" + String(config.uiLang == "en" ? " class='active'" : "") + ">EN</a></div>";
  html += "<h1>" + T("Purificateur Rowenta", "Rowenta Purifier") + "</h1>";
  if (strlen(message) > 0) html += "<p class='msg'>" + String(message) + "</p>";

  html += "<div class='card'>";
  if (!stateKnown) {
    html += "<div class='row' id='unknownRow'><span class='k'>" + T("Etat", "State") + "</span><span class='v'>" +
            T("Inconnu (en attente de donnees UART...)", "Unknown (waiting for UART data...)") + "</span></div>";
  } else {
    html += "<div class='row'><span class='k'>" + T("Marche", "Power") + "</span>";
    html += "<form id='pwrform' method='POST' action='/cmd/power' style='margin:0;'>";
    html += "<input type='hidden' name='value' id='pwrval' value='" + String(statePower ? "ON" : "OFF") + "'>";
    html += "<label class='switch'><input type='checkbox' id='pwrCheckbox'" + String(statePower ? " checked" : "") +
            " onchange=\"document.getElementById('pwrval').value=this.checked?'ON':'OFF';this.form.submit();\">";
    html += "<span class='slider'></span></label></form></div>";

    int level = 0;
    if (stateAirQuality >= AIRQ_THRESHOLD_BAD) level = 2;
    else if (stateAirQuality >= AIRQ_THRESHOLD_DEGRADED) level = 1;
    const char* levelClass[] = {"good", "degraded", "bad"};
    html += "<div class='row'><span class='k'>" + T("Qualite air", "Air quality") + "</span><span class='badge " + String(levelClass[level]) + "' id='airqBadge'>" +
            String(airLevelLabel(level)) + " (" + String(stateAirQuality) + ")</span></div>";
  }
  html += "</div>";

  if (stateKnown) {
    html += "<div class='card'>";

    html += "<label class='title'>" + T("Mode", "Mode") + "</label>";
    html += "<form method='POST' action='/cmd/mode' class='segmented icons' id='modeform'>";
    for (int i = 0; i < NUM_MODES; i++) {
      String id = "mode_" + String(i);
      html += "<input type='radio' id='" + id + "' name='value' value='" + String(MODES[i]) + "'" +
              (i == stateModeIdx ? " checked" : "") + " onchange='this.form.submit()'>";
      html += "<label for='" + id + "'>" + String(MODE_ICONS[i]) + "<span class='lbl'>" + String(modeLabel(i)) + "</span></label>";
    }
    html += "</form>";

    html += "<label class='title'>" + T("Intensite LED", "LED brightness") + "</label>";
    html += "<form method='POST' action='/cmd/light' class='segmented' id='lightform'>";
    for (int i = 0; i < NUM_LIGHTS; i++) {
      String id = "light_" + String(i);
      html += "<input type='radio' id='" + id + "' name='value' value='" + String(LIGHTS[i]) + "'" +
              (i == stateLightIdx ? " checked" : "") + " onchange='this.form.submit()'>";
      html += "<label for='" + id + "'>" + String(lightLabel(i)) + "</label>";
    }
    html += "</form>";

    html += "<form method='POST' action='/cmd/timer'>";
    html += "<button type='submit' class='ghost'>" + T("Timer suivant", "Next timer") + "</button></form>";

    html += "</div>";
  }

  html += "<a class='link' href='/settings'>" + T("Parametres (WiFi / OTA)", "Settings (WiFi / OTA)") + "</a>";

  html += "<script>";
  html += "let pendingClick=false;";
  html += "document.querySelectorAll('.segmented input, .switch input').forEach(el=>{";
  html += "el.addEventListener('change',()=>{pendingClick=true;});";
  html += "});";
  html += "function refreshState(){";
  html += "if(pendingClick)return;";
  html += "fetch('/state').then(r=>r.json()).then(d=>{";
  html += "if(!d.known){ if(document.getElementById('modeform')) location.reload(); return; }";
  html += "if(!document.getElementById('pwrCheckbox')){ location.reload(); return; }";
  html += "document.getElementById('pwrCheckbox').checked = (d.power==='ON');";
  html += "document.getElementById('pwrval').value = d.power;";
  html += "let badge=document.getElementById('airqBadge');";
  html += "badge.textContent = d.levelDisplay + ' (' + d.airq + ')';";
  html += "badge.className = 'badge ' + d.levelClass;";
  html += "let mr=document.querySelector('#modeform input[value=\"'+d.mode+'\"]');";
  html += "if(mr) mr.checked=true;";
  html += "let lr=document.querySelector('#lightform input[value=\"'+d.light+'\"]');";
  html += "if(lr) lr.checked=true;";
  html += "}).catch(()=>{});";
  html += "}";
  html += "setInterval(refreshState,2500);";
  html += "</script>";

  html += "</body></html>";
  return html;
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleControlGet() { server.send(200, "text/html", generateControlPage()); }

void handleState() {
  if (!stateKnown) {
    server.send(200, "application/json", "{\"known\":false}");
    return;
  }
  int level = 0;
  if (stateAirQuality >= AIRQ_THRESHOLD_BAD) level = 2;
  else if (stateAirQuality >= AIRQ_THRESHOLD_DEGRADED) level = 1;
  const char* levelClass[] = {"good", "degraded", "bad"};
  String json = "{\"known\":true,\"power\":\"" + String(statePower ? "ON" : "OFF") + "\",";
  json += "\"mode\":\"" + String(MODES[stateModeIdx]) + "\",";
  json += "\"light\":\"" + String(LIGHTS[stateLightIdx]) + "\",";
  json += "\"airq\":" + String(stateAirQuality) + ",";
  json += "\"level\":\"" + String(AIR_LEVELS[level]) + "\",";
  json += "\"levelDisplay\":\"" + String(airLevelLabel(level)) + "\",";
  json += "\"levelClass\":\"" + String(levelClass[level]) + "\"}";
  server.send(200, "application/json", json);
}

void handleLang() {
  String v = server.arg("set");
  if (v == "en" || v == "fr") {
    config.uiLang = v;
    saveConfig();
  }
  String ref = server.header("Referer");
  server.sendHeader("Location", ref.length() > 0 ? ref : "/", true);
  server.send(302, "text/plain", "");
}

void handleCmdPower() {
  if (stateKnown) setPower(server.arg("value") == "ON");
  redirectHome();
}

void handleCmdMode() {
  String v = server.arg("value");
  for (int i = 0; i < NUM_MODES; i++) if (v == MODES[i]) setMode(i);
  redirectHome();
}

void handleCmdLight() {
  String v = server.arg("value");
  for (int i = 0; i < NUM_LIGHTS; i++) if (v == LIGHTS[i]) setLight(i);
  redirectHome();
}

void handleCmdTimer() {
  simulatePress(PIN_TIMER, "TIMER");
  redirectHome();
}

void handleOtaGet() { server.send(200, "text/html", generateOtaPage()); }

void handleWifiPost() {
  String currentPass = server.arg("current_pass");
  if (currentPass != config.otaPass) {
    server.send(200, "text/html", generateOtaPage(T("Erreur : mot de passe OTA actuel incorrect.", "Error: current OTA password is incorrect.").c_str()));
    return;
  }
  String newSsid = server.arg("ssid");
  String newPass = server.arg("wifi_pass");
  if (newSsid.length() == 0) {
    server.send(200, "text/html", generateOtaPage(T("Erreur : le nom du reseau ne peut pas etre vide.", "Error: network name cannot be empty.").c_str()));
    return;
  }
  config.wifiSsid = newSsid;
  config.wifiPass = newPass;
  saveConfig();
  server.send(200, "text/html", "<h1>" + T("Reseau WiFi mis a jour", "WiFi network updated") + "</h1><p>" +
              T("Redemarrage, reconnexion a ", "Restarting, reconnecting to ") + newSsid + "...</p>");
  delay(1500);
  ESP.restart();
}

void handleOtaPost() {
  String currentPass = server.arg("current_pass");
  if (currentPass != config.otaPass) {
    server.send(200, "text/html", generateOtaPage(T("Erreur : mot de passe OTA actuel incorrect.", "Error: current OTA password is incorrect.").c_str()));
    return;
  }
  String p1 = server.arg("ota_pass");
  String p2 = server.arg("ota_pass2");
  if (p1.length() < 8) {
    server.send(200, "text/html", generateOtaPage(T("Erreur : minimum 8 caracteres.", "Error: minimum 8 characters.").c_str()));
    return;
  }
  if (p1 != p2) {
    server.send(200, "text/html", generateOtaPage(T("Erreur : les deux mots de passe ne correspondent pas.", "Error: the two passwords do not match.").c_str()));
    return;
  }
  config.otaPass = p1;
  saveConfig();
  server.send(200, "text/html", "<h1>" + T("Mot de passe OTA mis a jour", "OTA password updated") + "</h1><p>" + T("Redemarrage...", "Restarting...") + "</p>");
  delay(1500);
  ESP.restart();
}

void startPortal() {
  portalMode = true;
  Serial.println("=== Portail de configuration actif ===");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("Rowenta-Setup");
  Serial.print("Connectez-vous a 'Rowenta-Setup', puis http://192.168.4.1 - IP: ");
  Serial.println(WiFi.softAPIP());
  dnsServer.start(53, "*", apIP);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();
}

bool tryWifiConnect() {
  if (config.wifiSsid.length() == 0) return false;
  Serial.printf("Connexion WiFi a %s...\n", config.wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) { delay(500); Serial.print("."); attempts++; }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connecte, IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Echec connexion WiFi.");
  return false;
}

// ============================================================
// UART: checksum, send, read
// ============================================================

uint8_t calculateChecksum(uint8_t* data, int len) {
  uint16_t s = 0;
  for (int i = 0; i < len; i++) s += data[i];
  while (s > 0xFF) s = (s & 0xFF) + (s >> 8);
  return (uint8_t)s;
}

void sendAck(uint8_t seq) {
  uint8_t t[9] = {0xFF,0xFF,0x00,0x05,0x06,seq,0x00,0x00,0x00};
  t[8] = calculateChecksum(t, 8);
  mcuSerial.write(t, 9);
}

void sendHeartbeat(uint8_t seq) {
  uint8_t t[11] = {0xFF,0xFF,0x00,0x07,0x0D,seq,0x00,0x00,0x00,0x00,0x00};
  t[10] = calculateChecksum(t, 10);
  mcuSerial.write(t, 11);
}

void publishStateIfChanged();

void parseFrame(uint8_t* b, int len) {
  if (len < 6 || b[0] != 0xFF || b[1] != 0xFF) return;
  uint8_t type = b[4];
  uint8_t seq = b[5];

  if (type == 0x05 && len >= 45) {
    bool running = b[10] != 0;
    uint8_t m1 = b[14], m2 = b[15];
    uint8_t brightness = b[40];

    int modeIdx = -1;
    for (int i = 0; i < NUM_MODES; i++) {
      if (MODES_B1[i] == m1 && MODES_B2[i] == m2) { modeIdx = i; break; }
    }
    // "Day" sub-state with degraded air quality (purple LED): (03,03)
    // still recognized as part of Day mode (index 2)
    if (modeIdx < 0 && m1 == 0x03 && m2 == 0x03) modeIdx = 2;
    // "Night" sub-state with degraded air quality: (02,02)
    // still recognized as part of Night mode (index 3)
    if (modeIdx < 0 && m1 == 0x02 && m2 == 0x02) modeIdx = 3;
    int lightIdx = (brightness <= 2) ? brightness : 0;

    statePower = running;
    if (modeIdx >= 0) stateModeIdx = modeIdx;
    stateLightIdx = lightIdx;
    stateAirQuality = b[12];
    stateKnown = true;

    Serial.printf("[ETAT] marche:%s mode:%s lumiere:%s  C1=%d\n",
                  running ? "ON" : "OFF",
                  modeIdx >= 0 ? MODES[modeIdx] : "inconnu",
                  LIGHTS[lightIdx], b[12]);
    if (modeIdx < 0) {
      Serial.printf("       (mode inconnu -> m1=%02X m2=%02X)\n", m1, m2);
    }

    sendAck(seq);
    publishStateIfChanged();
  }
}

// ============================================================
// Button-press simulation
// ============================================================

void simulatePress(int pin, const char* name) {
  Serial.printf("  -> Simulation appui : %s\n", name);
  digitalWrite(pin, HIGH);
  delay(PRESS_DURATION_MS);
  digitalWrite(pin, LOW);
}

void setPower(bool turnOn) {
  if (!stateKnown) return;
  if (statePower != turnOn) simulatePress(PIN_POWER, "POWER");
  statePower = turnOn;
  publishStateIfChanged();
}

void setMode(int targetIndex) {
  if (!stateKnown || targetIndex < 0) return;
  int pressCount = (targetIndex - stateModeIdx + NUM_MODES) % NUM_MODES;
  for (int i = 0; i < pressCount; i++) {
    simulatePress(PIN_MODE, "MODE");
    delay(DELAY_BETWEEN_PRESSES_MS);
  }
  stateModeIdx = targetIndex;
  publishStateIfChanged();
}

void setLight(int targetIndex) {
  if (!stateKnown || targetIndex < 0) return;
  int pressCount = (targetIndex - stateLightIdx + NUM_LIGHTS) % NUM_LIGHTS;
  for (int i = 0; i < pressCount; i++) {
    simulatePress(PIN_LIGHT, "LIGHT");
    delay(DELAY_BETWEEN_PRESSES_MS);
  }
  stateLightIdx = targetIndex;
  publishStateIfChanged();
}

// ============================================================
// MQTT: Home Assistant discovery, publishing, commands
// ============================================================

String deviceBlock() {
  return "\"device\":{\"identifiers\":[\"" + String(DEVICE_ID) +
         "\"],\"name\":\"Purificateur Rowenta\",\"manufacturer\":\"Rowenta\",\"model\":\"PU6080F0\"}";
}

void publishDiscovery() {
  String p;

  // Switch (power on/off)
  p = "{\"name\":\"Purificateur\",\"unique_id\":\"" + String(DEVICE_ID) + "_power\","
      "\"command_topic\":\"" TOPIC_POWER_SET "\",\"state_topic\":\"" TOPIC_POWER_STATE "\","
      "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
      "\"availability_topic\":\"" TOPIC_AVAIL "\"," + deviceBlock() + "}";
  mqtt.publish(("homeassistant/switch/" + String(DEVICE_ID) + "/power/config").c_str(), p.c_str(), true);

  // Select (mode)
  p = "{\"name\":\"Mode\",\"unique_id\":\"" + String(DEVICE_ID) + "_mode\","
      "\"command_topic\":\"" TOPIC_MODE_SET "\",\"state_topic\":\"" TOPIC_MODE_STATE "\","
      "\"options\":[\"Silencieux\",\"Boost\",\"Jour\",\"Nuit\"],"
      "\"availability_topic\":\"" TOPIC_AVAIL "\"," + deviceBlock() + "}";
  mqtt.publish(("homeassistant/select/" + String(DEVICE_ID) + "/mode/config").c_str(), p.c_str(), true);

  // Select (LED brightness)
  p = "{\"name\":\"Intensite LED\",\"unique_id\":\"" + String(DEVICE_ID) + "_light\","
      "\"command_topic\":\"" TOPIC_LIGHT_SET "\",\"state_topic\":\"" TOPIC_LIGHT_STATE "\","
      "\"options\":[\"Max\",\"Faible\",\"Eteint\"],"
      "\"availability_topic\":\"" TOPIC_AVAIL "\"," + deviceBlock() + "}";
  mqtt.publish(("homeassistant/select/" + String(DEVICE_ID) + "/light/config").c_str(), p.c_str(), true);

  // Button (timer)
  p = "{\"name\":\"Timer suivant\",\"unique_id\":\"" + String(DEVICE_ID) + "_timer\","
      "\"command_topic\":\"" TOPIC_TIMER_SET "\","
      "\"availability_topic\":\"" TOPIC_AVAIL "\"," + deviceBlock() + "}";
  mqtt.publish(("homeassistant/button/" + String(DEVICE_ID) + "/timer/config").c_str(), p.c_str(), true);

  // Sensor (air quality, raw value)
  p = "{\"name\":\"Qualite air (brut)\",\"unique_id\":\"" + String(DEVICE_ID) + "_airq\","
      "\"state_topic\":\"" TOPIC_AIRQ_STATE "\","
      "\"availability_topic\":\"" TOPIC_AVAIL "\"," + deviceBlock() + "}";
  mqtt.publish(("homeassistant/sensor/" + String(DEVICE_ID) + "/airq/config").c_str(), p.c_str(), true);

  // Sensor (air quality, text level Good/Degraded/Bad)
  p = "{\"name\":\"Qualite air\",\"unique_id\":\"" + String(DEVICE_ID) + "_airq_niveau\","
      "\"state_topic\":\"" TOPIC_AIRQ_LEVEL_STATE "\","
      "\"availability_topic\":\"" TOPIC_AVAIL "\"," + deviceBlock() + "}";
  mqtt.publish(("homeassistant/sensor/" + String(DEVICE_ID) + "/airq_niveau/config").c_str(), p.c_str(), true);

  Serial.println("Decouverte Home Assistant publiee.");
}

bool lastPowerPublished = false, firstPublishDone = false;
int lastModePublished = -1, lastLightPublished = -1;
int lastAirqPublished = -1;
int lastLevelPublished = -1;

void publishStateIfChanged() {
  if (!mqtt.connected()) return;
  if (!firstPublishDone || statePower != lastPowerPublished) {
    mqtt.publish(TOPIC_POWER_STATE, statePower ? "ON" : "OFF", true);
    lastPowerPublished = statePower;
  }
  if (!firstPublishDone || stateModeIdx != lastModePublished) {
    mqtt.publish(TOPIC_MODE_STATE, MODES[stateModeIdx], true);
    lastModePublished = stateModeIdx;
  }
  if (!firstPublishDone || stateLightIdx != lastLightPublished) {
    mqtt.publish(TOPIC_LIGHT_STATE, LIGHTS[stateLightIdx], true);
    lastLightPublished = stateLightIdx;
  }
  if (!firstPublishDone || stateAirQuality != lastAirqPublished) {
    mqtt.publish(TOPIC_AIRQ_STATE, String(stateAirQuality).c_str(), true);
    lastAirqPublished = stateAirQuality;
  }
  int level = 0;
  if (stateAirQuality >= AIRQ_THRESHOLD_BAD) level = 2;
  else if (stateAirQuality >= AIRQ_THRESHOLD_DEGRADED) level = 1;
  if (!firstPublishDone || level != lastLevelPublished) {
    mqtt.publish(TOPIC_AIRQ_LEVEL_STATE, AIR_LEVELS[level], true);
    lastLevelPublished = level;
  }
  firstPublishDone = true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String t = String(topic);

  Serial.printf("[MQTT] %s = %s\n", topic, msg.c_str());

  if (t == TOPIC_POWER_SET) {
    setPower(msg == "ON");
  } else if (t == TOPIC_MODE_SET) {
    for (int i = 0; i < NUM_MODES; i++) if (msg == MODES[i]) setMode(i);
  } else if (t == TOPIC_LIGHT_SET) {
    for (int i = 0; i < NUM_LIGHTS; i++) if (msg == LIGHTS[i]) setLight(i);
  } else if (t == TOPIC_TIMER_SET) {
    simulatePress(PIN_TIMER, "TIMER");
  }
}

unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
#define WIFI_RECONNECT_INTERVAL_MS 15000

// Non-blocking WiFi watchdog: retries a reconnect periodically if the link drops
void monitorWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttempt < WIFI_RECONNECT_INTERVAL_MS) return;
  lastWifiAttempt = millis();
  Serial.println("WiFi deconnecte, tentative de reconnexion...");
  WiFi.reconnect();
}

void ensureMqttConnection() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttempt < 5000) return;
  lastMqttAttempt = millis();

  Serial.println("Connexion MQTT...");
  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = mqtt.connect(clientId.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str(),
                          TOPIC_AVAIL, 0, true, "offline");
  if (ok) {
    Serial.println("MQTT connecte.");
    mqtt.publish(TOPIC_AVAIL, "online", true);
    mqtt.subscribe(TOPIC_POWER_SET);
    mqtt.subscribe(TOPIC_MODE_SET);
    mqtt.subscribe(TOPIC_LIGHT_SET);
    mqtt.subscribe(TOPIC_TIMER_SET);
    publishDiscovery();
    firstPublishDone = false;
    publishStateIfChanged();
  } else {
    Serial.printf("Echec MQTT, code=%d\n", mqtt.state());
  }
}

// ============================================================
// Setup / Loop
// ============================================================

void setup() {
  Serial.begin(115200);
  while (!Serial) { }

  mcuSerial.begin(BAUD_MCU, SERIAL_8N1, PIN_RX, PIN_TX);

  pinMode(PIN_POWER, OUTPUT); digitalWrite(PIN_POWER, LOW);
  pinMode(PIN_LIGHT, OUTPUT); digitalWrite(PIN_LIGHT, LOW);
  pinMode(PIN_MODE, OUTPUT);  digitalWrite(PIN_MODE, LOW);
  pinMode(PIN_TIMER, OUTPUT); digitalWrite(PIN_TIMER, LOW);

  Serial.println("\n=== Rowenta - Firmware complet - demarrage ===");
  Serial.println("    par Xavier Hang");

  bootTime = millis();
  if (loadConfigAndCheckReset()) {
    Serial.println("!! Double-reset detecte : reinitialisation du mot de passe OTA !!");
    config.otaPass = "rowenta_ota";
    saveConfig();
  }

  if (!tryWifiConnect()) {
    startPortal();
  } else {
    mqtt.setServer(config.mqttHost.c_str(), config.mqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(1024);

    // Settings page also reachable at the device's normal IP (not just the AP portal)
    server.on("/", handleControlGet);
    server.on("/state", HTTP_GET, handleState);
    server.on("/lang", HTTP_GET, handleLang);
    server.on("/settings", HTTP_GET, handleOtaGet);
    server.on("/ota", HTTP_POST, handleOtaPost);
    server.on("/wifi", HTTP_POST, handleWifiPost);
    server.on("/cmd/power", HTTP_POST, handleCmdPower);
    server.on("/cmd/mode", HTTP_POST, handleCmdMode);
    server.on("/cmd/light", HTTP_POST, handleCmdLight);
    server.on("/cmd/timer", HTTP_POST, handleCmdTimer);
    server.begin();
    Serial.println("Page de parametres disponible sur http://" + WiFi.localIP().toString());

    ArduinoOTA.setHostname(DEVICE_ID);
    ArduinoOTA.setPassword(config.otaPass.c_str());
    ArduinoOTA.onStart([]() { Serial.println("OTA : debut mise a jour"); });
    ArduinoOTA.onEnd([]() { Serial.println("\nOTA : terminee, redemarrage"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { esp_task_wdt_reset(); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA erreur [%u]\n", error); });
    ArduinoOTA.begin();
    Serial.println("OTA pret (hostname: " + String(DEVICE_ID) + ")");
  }

  lastHeartbeat = millis();

  // Hardware watchdog: reboots automatically if the main loop stops feeding it
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(NULL);
  Serial.printf("Watchdog actif (timeout %ds)\n", WDT_TIMEOUT_S);
}

void loop() {
  esp_task_wdt_reset();

  // Clears the double-reset flag once the boot has been stable for a few seconds
  if (!bootFlagCleared && millis() - bootTime > DOUBLE_RESET_WINDOW_MS) {
    confirmStableBoot();
    bootFlagCleared = true;
  }

  if (portalMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    ensureMqttConnection();
    mqtt.loop();
    ArduinoOTA.handle();
    server.handleClient();
  } else {
    monitorWifi();
  }

  while (mcuSerial.available()) {
    if (bufLen < (int)sizeof(buf)) buf[bufLen++] = mcuSerial.read();
    else mcuSerial.read();
    lastByteTime = millis();
  }
  if (bufLen > 0 && millis() - lastByteTime > FRAME_TIMEOUT_MS) {
    parseFrame(buf, bufLen);
    bufLen = 0;
  }

  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat(heartbeatSeq++);
    lastHeartbeat = millis();
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim(); cmd.toLowerCase();
    if (cmd == "reset_wifi") {
      clearConfig();
      delay(300);
      ESP.restart();
    } else if (cmd == "power") {
      simulatePress(PIN_POWER, "POWER");
    } else if (cmd == "light") {
      simulatePress(PIN_LIGHT, "LIGHT");
    } else if (cmd == "mode") {
      simulatePress(PIN_MODE, "MODE");
    } else if (cmd == "timer") {
      simulatePress(PIN_TIMER, "TIMER");
    }
  }
}
