#include <Arduino.h>
#include <ArduinoJson.h>
#include <ETH.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <AtemClient.h>
#include "Arbitration.h"
#include "ProgramCoordinator.h"
#include "BoardProfile.h"
#include "Settings.h"
#include "web_assets.h"

namespace {
Settings settings;
datv::Arbitration controller(datv::ArbitrationConfig{});
datv::ProgramCoordinator programCoordinator;
AtemClient atemClient;
WebServer server(80);

struct Event { uint32_t uptimeMs = 0; char message[112]{}; };
std::array<Event, 32> events{};
size_t eventHead = 0, eventCount = 0;
String csrfToken, apSsid;
bool apRunning = false, mdnsRunning = false, lastUplink = false;
bool lastConnected = false, lastPreferredEthernet = false;
IPAddress lastUplinkIp;
uint32_t lastInputPoll = 0, noUplinkSince = 0, rebootAt = 0;
bool rebootScheduled = false;
uint32_t acceptedCommands = 0, applicationErrors = 0;
std::array<bool, datv::kInputCount> previousLocks{};

void logEvent(const String& text) {
  auto& entry = events[eventHead];
  entry.uptimeMs = millis();
  strlcpy(entry.message, text.c_str(), sizeof(entry.message));
  eventHead = (eventHead + 1) % events.size();
  eventCount = min(eventCount + 1, events.size());
  Serial.printf("[%lu] %s\n", static_cast<unsigned long>(entry.uptimeMs), entry.message);
}

bool ethernetReady() { return board::ethernet && ETH.linkUp() && static_cast<uint32_t>(ETH.localIP()) != 0; }
bool wifiReady() { return WiFi.status() == WL_CONNECTED && static_cast<uint32_t>(WiFi.localIP()) != 0; }
bool uplinkReady() { return ethernetReady() || wifiReady(); }
IPAddress uplinkIP() { return ethernetReady() ? ETH.localIP() : WiFi.localIP(); }

void beginAtem();

bool pauseAutomatic(const String& why) {
  const bool abandonUnacked = datv::ProgramCoordinator::mustAbandonOnPause(
      controller.automatic(), atemClient.pendingCommands());
  if (controller.automatic()) logEvent(why);
  controller.setAutomatic(false, millis());
  programCoordinator.reset();
  // A reliable sequence cannot drop one packet and retain later packets. End
  // the whole session to stop old automatic commands being retried after Pause.
  // A datagram already received/applied by the switcher cannot be retracted.
  if (abandonUnacked) {
    beginAtem();
    logEvent("Unacknowledged commands abandoned; reconnecting ATEM");
  }
  return abandonUnacked;
}

void startSetupAp() {
  if (apRunning) return;
  apRunning = WiFi.softAP(apSsid.c_str(), settings.adminPassword.c_str());
  if (apRunning) logEvent(String("Setup AP ") + apSsid + " at " + WiFi.softAPIP().toString());
  else { ++applicationErrors; logEvent("Unable to start setup AP"); }
}

void beginAtem() {
  programCoordinator.sessionLost();
  atemClient.disconnect();
  lastConnected = false;
  if (settings.atemIp.isEmpty() || !uplinkReady()) return;
  IPAddress remote;
  if (remote.fromString(settings.atemIp) && !atemClient.begin(remote)) {
    ++applicationErrors;
    logEvent("ATEM UDP socket could not start; retrying in transport");
  }
}

void maintainNetwork(uint32_t now) {
  const bool ready = uplinkReady();
  const bool preferredEthernet = ethernetReady();
  const IPAddress currentIp = ready ? uplinkIP() : IPAddress();
  const bool identityChanged = ready && lastUplink &&
      (currentIp != lastUplinkIp || preferredEthernet != lastPreferredEthernet);
  if (ready != lastUplink || identityChanged) {
    lastUplink = ready;
    lastUplinkIp = currentIp;
    lastPreferredEthernet = preferredEthernet;
    if (ready) {
      logEvent(String(identityChanged ? "Network changed: " : "Network ready: ") +
               (preferredEthernet ? "Ethernet " : "Wi-Fi ") + currentIp.toString());
      if (!mdnsRunning) {
        mdnsRunning = MDNS.begin(settings.hostname.c_str());
        if (mdnsRunning) MDNS.addService("http", "tcp", 80);
      }
      beginAtem();
    } else {
      noUplinkSince = now;
      atemClient.disconnect();
      programCoordinator.sessionLost();
      logEvent("Network unavailable; ATEM commands stopped");
    }
  }
  // Leave a started maintenance AP available for this boot so applying network
  // settings does not cut off the operator mid-commissioning. WPA2 uses the
  // generated device password, never an open AP or a shared factory password.
  if (!ready && static_cast<uint32_t>(now - noUplinkSince) >= 15000) startSetupAp();
}

void sampleInputs(uint32_t now) {
  if (static_cast<uint32_t>(now - lastInputPoll) < 10) return;
  lastInputPoll = now;
  std::array<bool, datv::kInputCount> raw{};
  for (size_t i = 0; i < raw.size(); ++i) raw[i] = digitalRead(board::alarmPins[i]) == HIGH;
  controller.sample(raw, now);
  for (size_t i = 0; i < raw.size(); ++i) {
    bool locked = controller.inputs()[i].locked;
    if (locked != previousLocks[i]) {
      previousLocks[i] = locked;
      logEvent(settings.labels[i] + (locked ? ": video lock qualified" : ": video lock lost"));
    }
  }
}

datv::ProgramFeedback programFeedback() {
  const auto& state = atemClient.state();
  datv::ProgramFeedback feedback;
  feedback.synchronized = atemClient.connected();
  feedback.valid = state.programValid;
  feedback.program = state.program;
  feedback.generation = state.connectionGeneration;
  feedback.inTransition = state.transitionValid && state.inTransition;
  feedback.inFlight = atemClient.pendingCommands();
  return feedback;
}

void observeAtem(uint32_t now) {
  const bool connected = atemClient.connected();
  if (connected != lastConnected) {
    lastConnected = connected;
    if (connected) logEvent(String("ATEM connected: ") + atemClient.state().product);
    else {
      logEvent(atemClient.state().programValid ? "ATEM feedback gap; waiting for missing packets" :
                                               "ATEM disconnected; awaiting fresh feedback");
    }
  }
  const auto observation = programCoordinator.observe(programFeedback(), controller.automatic(),
                                                      settings.respectExternalControl);
  if (observation.changed) logEvent(String("ATEM confirms Program ") + observation.program);
  if (observation.external) pauseAutomatic("External Program change; automation paused");
  (void)now;
}

void runAutomatic(uint32_t now) {
  if (!controller.automatic()) return;
  const auto decision = programCoordinator.decide(programFeedback(), controller.selection().source, now);
  if (decision.action == datv::ProgramAction::None) return;
  if (decision.action == datv::ProgramAction::RestartSession) {
    logEvent(decision.reason);
    beginAtem();
    return;
  }
  if (decision.action == datv::ProgramAction::PauseUnconfirmed) {
    ++applicationErrors;
    pauseAutomatic(decision.reason);
    return;
  }
  const bool accepted = atemClient.setProgram(decision.source);
  programCoordinator.recordAttempt(decision.source, accepted, now);
  if (accepted) {
    ++acceptedCommands;
    logEvent(String("Auto requests Program ") + decision.source + " (" + controller.selection().reason + ")");
  } else {
    ++applicationErrors;
    logEvent("ATEM rejected automatic request; waiting before retry");
  }
}

void commonHeaders() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("X-Frame-Options", "DENY");
}

void jsonError(int status, const String& error) {
  DynamicJsonDocument doc(512);
  doc["ok"] = false;
  doc["error"] = error;
  String body;
  serializeJson(doc, body);
  commonHeaders();
  server.send(status, "application/json", body);
}

bool authenticate() {
  if (server.authenticate("admin", settings.adminPassword.c_str())) return true;
  commonHeaders();
  server.requestAuthentication(BASIC_AUTH, "DATV ATEM controller");
  return false;
}

bool writeRequest(DynamicJsonDocument& doc) {
  if (!authenticate()) return false;
  if (server.header("X-CSRF-Token") != csrfToken) { jsonError(403, "Missing or stale control token; reload the page"); return false; }
  if (!server.header("Content-Type").startsWith("application/json")) { jsonError(415, "Use application/json"); return false; }
  if (!server.hasArg("plain") || server.arg("plain").length() > 8192) { jsonError(413, "JSON body missing or too large"); return false; }
  if (deserializeJson(doc, server.arg("plain")) || !doc.is<JsonObject>()) { jsonError(400, "Invalid JSON object"); return false; }
  return true;
}

void jsonOk(const char* message) {
  DynamicJsonDocument doc(512);
  doc["ok"] = true;
  doc["message"] = message;
  String body;
  serializeJson(doc, body);
  commonHeaders();
  server.send(200, "application/json", body);
}

void sendStatus() {
  if (!authenticate()) return;
  DynamicJsonDocument doc(16384);
  doc["csrfToken"] = csrfToken;
  doc["uptimeMs"] = static_cast<uint64_t>(esp_timer_get_time() / 1000);
  doc["mode"] = controller.automatic() ? "auto" : "manual";
  doc["board"] = board::name;
  auto network = doc.createNestedObject("network");
  network["link"] = uplinkReady();
  network["ip"] = uplinkReady() ? uplinkIP().toString() : String();
  network["interface"] = ethernetReady() ? "Ethernet" : wifiReady() ? "Wi-Fi" : "Setup AP";
  network["apSsid"] = apRunning ? apSsid : String();
  network["apIp"] = apRunning ? WiFi.softAPIP().toString() : String();
  auto switcher = doc.createNestedObject("atem");
  const auto& state = atemClient.state();
  const auto& stats = atemClient.stats();
  switcher["configured"] = !settings.atemIp.isEmpty();
  switcher["connected"] = atemClient.connected();
  switcher["status"] = atemClient.statusText();
  switcher["error"] = atemClient.errorText();
  if (atemClient.connected() && state.programValid) switcher["program"] = state.program;
  else switcher["program"] = nullptr;
  if (atemClient.connected() && state.previewValid) switcher["preview"] = state.preview;
  else switcher["preview"] = nullptr;
  switcher["inTransition"] = atemClient.connected() && state.transitionValid && state.inTransition;
  if (atemClient.connected() && state.fadeToBlackValid) switcher["fadeToBlack"] = state.fadeToBlack;
  else switcher["fadeToBlack"] = nullptr;
  switcher["product"] = state.product;
  switcher["version"] = state.version;
  switcher["lastRxMs"] = state.lastRxMs;
  switcher["commandsSent"] = acceptedCommands;
  switcher["errors"] = applicationErrors + stats.malformedPackets + stats.sendErrors + stats.droppedCommands;
  switcher["pendingCommands"] = atemClient.pendingCommands();
  switcher["retransmissions"] = stats.retransmissions;
  auto automatic = doc.createNestedObject("automatic");
  const auto& selection = controller.selection();
  if (selection.source == datv::kNoSource) automatic["desiredSource"] = nullptr;
  else automatic["desiredSource"] = selection.source;
  automatic["reason"] = selection.reason;
  automatic["eligibleCount"] = selection.eligibleCount;
  automatic["remainingMs"] = selection.remainingMs;
  auto inputs = doc.createNestedArray("inputs");
  for (size_t i = 0; i < datv::kInputCount; ++i) {
    auto input = inputs.createNestedObject();
    const auto& rule = settings.arbitration.inputs[i];
    const auto& sampled = controller.inputs()[i];
    input["index"] = i;
    input["gpio"] = board::alarmPins[i];
    input["label"] = settings.labels[i];
    input["source"] = rule.source;
    input["enabled"] = rule.enabled;
    input["activeHigh"] = rule.activeHigh;
    input["priority"] = rule.priority;
    input["rawHigh"] = sampled.rawHigh;
    input["locked"] = sampled.locked;
    input["qualifying"] = sampled.qualifying;
    input["remainingMs"] = sampled.remainingMs;
  }
  auto eventJson = doc.createNestedArray("events");
  for (size_t n = 0; n < eventCount; ++n) {
    const auto& entry = events[(eventHead + events.size() - n - 1) % events.size()];
    auto event = eventJson.createNestedObject();
    event["uptimeMs"] = entry.uptimeMs;
    event["message"] = entry.message;
  }
  settingsToJson(settings, doc.createNestedObject("config"));
  if (doc.overflowed()) { jsonError(500, "Status buffer exhausted"); return; }
  String body;
  serializeJson(doc, body);
  commonHeaders();
  server.send(200, "application/json", body);
}

void setMode() {
  DynamicJsonDocument doc(512);
  if (!writeRequest(doc)) return;
  const String mode = doc["mode"].as<String>();
  if (mode != "auto" && mode != "manual") { jsonError(400, "Mode must be auto or manual"); return; }
  if (mode == "auto") {
    if (!atemClient.connected()) { jsonError(409, "Connect to the ATEM and receive feedback before resuming Auto"); return; }
    if (!controller.automatic()) {
      if (atemClient.pendingCommands() > 0) { jsonError(409, "Wait for pending manual commands before resuming Auto"); return; }
      programCoordinator.reset();
      controller.setAutomatic(true, millis());
      logEvent("Operator resumed automatic switching");
    }
  } else pauseAutomatic("Operator paused automatic switching");
  jsonOk(mode == "auto" ? "Automatic switching resumed" : "Automatic switching paused");
}

void manualAction() {
  DynamicJsonDocument doc(512);
  if (!writeRequest(doc)) return;
  String action = doc["action"].as<String>();
  if (action != "program" && action != "preview" && action != "cut" && action != "auto" && action != "ftb") {
    jsonError(400, "Unknown ATEM action"); return;
  }
  uint16_t source = 0;
  if (action == "program" || action == "preview") {
    if (!doc["source"].is<uint16_t>() || doc["source"].as<uint16_t>() > 6) { jsonError(400, "Source must be Black (0) or input 1–6"); return; }
    source = doc["source"].as<uint16_t>();
  }
  const bool reconnecting = action != "preview" && pauseAutomatic("Manual on-air control; automation paused");
  if (!atemClient.connected()) {
    jsonError(409, reconnecting ? "Auto paused; previous unacknowledged commands abandoned. ATEM reconnecting: retry after feedback returns." : "ATEM is disconnected; command was not queued");
    return;
  }
  bool accepted = action == "program" ? atemClient.setProgram(source) :
                  action == "preview" ? atemClient.setPreview(source) :
                  action == "cut" ? atemClient.cut() :
                  action == "auto" ? atemClient.autoTransition() : atemClient.fadeToBlack();
  if (!accepted) { ++applicationErrors; jsonError(503, "ATEM command window is busy; command was not queued"); return; }
  ++acceptedCommands;
  logEvent(String("Manual ATEM action: ") + action + ((action == "program" || action == "preview") ? String(" ") + source : String()));
  jsonOk("Command accepted; on-air indication updates from ATEM feedback");
}

void setConfig() {
  DynamicJsonDocument doc(8192);
  if (!writeRequest(doc)) return;
  Settings next = settings;
  String error;
  if (!settingsFromJson(doc.as<JsonObjectConst>(), next, error)) { jsonError(400, error); return; }
  const bool networkChanged = next.hostname != settings.hostname || next.wifiSsid != settings.wifiSsid || next.wifiPassword != settings.wifiPassword;
  const bool passwordChanged = next.adminPassword != settings.adminPassword;
  const bool addressChanged = next.atemIp != settings.atemIp;
  if (!saveSettings(next)) { ++applicationErrors; jsonError(500, "Settings could not be saved"); return; }
  pauseAutomatic("Settings saved; automation paused for verification");
  settings = next;
  controller.configure(settings.arbitration, millis());
  previousLocks = {};
  if (addressChanged) beginAtem();
  // Updating polarity never changes a pin to OUTPUT; field wiring remains read-only.
  for (size_t i = 0; i < datv::kInputCount; ++i) {
    uint8_t pin = board::alarmPins[i];
    pinMode(pin, pin >= 34 ? INPUT : settings.arbitration.inputs[i].activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  }
  logEvent("Configuration applied");
  DynamicJsonDocument response(512);
  response["ok"] = true;
  response["rebootRequired"] = networkChanged || passwordChanged;
  response["reauthRequired"] = passwordChanged;
  response["message"] = networkChanged || passwordChanged ? "Saved. Reboot to apply network/setup AP settings. Auto is paused." : "Saved and applied. Auto is paused.";
  String body;
  serializeJson(response, body);
  commonHeaders();
  server.send(200, "application/json", body);
}

void serveAsset(const char* mime, const uint8_t* bytes, size_t size) {
  if (!authenticate()) return;
  commonHeaders();
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Content-Security-Policy", "default-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
  server.send_P(200, mime, reinterpret_cast<const char*>(bytes), size);
}

void configureWeb() {
  const char* headers[] = {"X-CSRF-Token", "Content-Type"};
  server.collectHeaders(headers, 2);
  server.on("/", HTTP_GET, [] { serveAsset("text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN); });
  server.on("/app.css", HTTP_GET, [] { serveAsset("text/css; charset=utf-8", APP_CSS_GZ, APP_CSS_GZ_LEN); });
  server.on("/app.js", HTTP_GET, [] { serveAsset("application/javascript", APP_JS_GZ, APP_JS_GZ_LEN); });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/mode", HTTP_POST, setMode);
  server.on("/api/action", HTTP_POST, manualAction);
  server.on("/api/config", HTTP_POST, setConfig);
  server.on("/api/reboot", HTTP_POST, [] {
    DynamicJsonDocument doc(256);
    if (!writeRequest(doc)) return;
    pauseAutomatic("Operator requested controller restart");
    rebootScheduled = true;
    rebootAt = millis();
    jsonOk("Restart scheduled; automation will boot paused");
  });
  server.onNotFound([] { if (authenticate()) jsonError(404, "Not found"); });
  server.begin();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  // Start RF before esp_random() generates credentials and the control token.
  // AP mode is enabled only by softAP() after its password has been supplied.
  WiFi.mode(WIFI_STA);
  settings = defaultSettings();
  if (!loadSettings(settings)) {
    if (!saveSettings(settings)) logEvent("Settings storage unavailable; credentials will change after restart");
    logEvent("Factory configuration created");
  }
  controller.configure(settings.arbitration, millis());
  for (size_t i = 0; i < datv::kInputCount; ++i) {
    uint8_t pin = board::alarmPins[i];
    pinMode(pin, pin >= 34 ? INPUT : settings.arbitration.inputs[i].activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  }
  char suffix[7];
  uint8_t baseMac[6] = {};
  esp_efuse_mac_get_default(baseMac);
  snprintf(suffix, sizeof(suffix), "%02X%02X%02X", baseMac[3], baseMac[4], baseMac[5]);
  apSsid = String("DATV-ATEM-") + suffix;
  char token[33];
  snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  csrfToken = token;
  Serial.printf("\nDATV ATEM controller\nBoard: %s\nWeb username: admin\nDevice password: %s\nSetup AP: %s\n", board::name, settings.adminPassword.c_str(), apSsid.c_str());
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.setSleep(false);
  if (!settings.wifiSsid.isEmpty()) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  }
#if defined(DATV_BOARD_POE_ISO)
  if (!ETH.begin(0, 12, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO17_OUT)) logEvent("Ethernet initialization failed");
  ETH.setHostname(settings.hostname.c_str());
#endif
  if (!board::ethernet && settings.wifiSsid.isEmpty()) startSetupAp();
  noUplinkSince = millis();
  configureWeb();
  logEvent("Ready. Automatic switching is paused until explicitly resumed.");
}

void loop() {
  const uint32_t now = millis();
  maintainNetwork(now);
  atemClient.tick();
  sampleInputs(now);
  observeAtem(now);
  runAutomatic(now);
  server.handleClient();
  if (rebootScheduled && static_cast<uint32_t>(millis() - rebootAt) >= 500) ESP.restart();
  delay(1);
}
