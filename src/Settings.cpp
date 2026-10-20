#include "Settings.h"
#include "BoardProfile.h"
#include <Preferences.h>
#include <cstring>

namespace {
String randomPassword() {
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  String value;
  value.reserve(20);
  for (int i = 0; i < 20; ++i) value += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  return value;
}
bool integerIn(JsonVariantConst value, uint32_t low, uint32_t high) {
  return value.is<uint32_t>() && value.as<uint32_t>() >= low && value.as<uint32_t>() <= high;
}
bool textIn(JsonVariantConst value, size_t low, size_t high) {
  if (!value.is<const char*>()) return false;
  const char* p = value.as<const char*>();
  const size_t len = strlen(p);
  if (len < low || len > high) return false;
  for (size_t i = 0; i < len; ++i) if (static_cast<unsigned char>(p[i]) < 32) return false;
  return true;
}
}  // namespace

Settings defaultSettings() {
  Settings out;
  out.adminPassword = randomPassword();
  for (size_t i = 0; i < datv::kInputCount; ++i) {
    out.labels[i] = String("Receiver ") + (i + 1);
    out.arbitration.inputs[i].source = i + 1;
    out.arbitration.inputs[i].priority = i + 1;
    out.arbitration.inputs[i].activeHigh = board::activeHigh;
  }
  return out;
}

const char* policyName(datv::MultiPolicy policy) {
  switch (policy) {
    case datv::MultiPolicy::Priority: return "priority";
    case datv::MultiPolicy::Conflict: return "conflict";
    default: return "round_robin";
  }
}

void settingsToJson(const Settings& settings, JsonObject out, bool withSecrets) {
  out["atemIp"] = settings.atemIp;
  out["hostname"] = settings.hostname;
  out["wifiSsid"] = settings.wifiSsid;
  out["wifiPasswordSet"] = !settings.wifiPassword.isEmpty();
  if (withSecrets) {
    out["version"] = 1;
    out["wifiPassword"] = settings.wifiPassword;
    out["adminPassword"] = settings.adminPassword;
  }
  const auto& a = settings.arbitration;
  out["acquireMs"] = a.acquireMs;
  out["releaseMs"] = a.releaseMs;
  out["switchIntervalSec"] = a.switchIntervalMs / 1000;
  out["multiPolicy"] = policyName(a.policy);
  out["conflictSource"] = a.conflictSource;
  out["fallbackSource"] = a.fallbackSource;
  out["respectExternalControl"] = settings.respectExternalControl;
  auto inputs = out.createNestedArray("inputs");
  for (size_t i = 0; i < datv::kInputCount; ++i) {
    auto input = inputs.createNestedObject();
    input["label"] = settings.labels[i];
    input["enabled"] = a.inputs[i].enabled;
    input["source"] = a.inputs[i].source;
    input["activeHigh"] = a.inputs[i].activeHigh;
    input["priority"] = a.inputs[i].priority;
  }
}

bool settingsFromJson(JsonObjectConst in, Settings& next, String& error, bool loading) {
  if (in.isNull()) { error = "Expected a JSON object"; return false; }
  if (!textIn(in["atemIp"], 0, 15)) { error = "ATEM IP must be a numeric IPv4 address or empty"; return false; }
  const String ipText = in["atemIp"].as<String>();
  IPAddress ip;
  if (!ipText.isEmpty() && (!ip.fromString(ipText) || ip[0] == 0 || ip[0] == 127 || ip[0] >= 224 || ip == IPAddress(255,255,255,255))) {
    error = "ATEM IP must be a unicast IPv4 address"; return false;
  }
  if (!textIn(in["hostname"], 1, 31)) { error = "Hostname must contain 1–31 letters, digits or hyphens"; return false; }
  String host = in["hostname"].as<String>();
  for (size_t i = 0; i < host.length(); ++i) {
    char c = host[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == '-' && i > 0 && i + 1 < host.length()))) {
      error = "Invalid hostname"; return false;
    }
  }
  if (!textIn(in["wifiSsid"], 0, 32)) { error = "Wi-Fi SSID must be at most 32 bytes"; return false; }
  for (const char* key : {"wifiPassword", "adminPassword"}) {
    if (!in[key].isNull() && !textIn(in[key], 0, 63)) { error = String(key) + " must be a string of at most 63 bytes"; return false; }
  }
  String wifi = in["wifiPassword"].isNull() ? String() : in["wifiPassword"].as<String>();
  String admin = in["adminPassword"].isNull() ? String() : in["adminPassword"].as<String>();
  if (!in["clearWifiPassword"].isNull() && !in["clearWifiPassword"].is<bool>()) {
    error = "clearWifiPassword must be boolean"; return false;
  }
  const bool clearWifi = in["clearWifiPassword"] | false;
  if (clearWifi && !wifi.isEmpty()) { error = "Choose either a new Wi-Fi password or clear the saved password"; return false; }
  if (!wifi.isEmpty() && wifi.length() < 8) { error = "Wi-Fi password must contain 8–63 characters"; return false; }
  if ((!admin.isEmpty() || loading) && admin.length() < 12) { error = "Admin password must contain 12–63 characters"; return false; }
  if (!integerIn(in["acquireMs"], 0, 60000) || !integerIn(in["releaseMs"], 0, 60000)) {
    error = "Qualification times must be integers from 0 to 60000 ms"; return false;
  }
  if (!integerIn(in["switchIntervalSec"], 1, 86400)) { error = "Switch interval must be 1–86400 seconds"; return false; }
  if (!integerIn(in["fallbackSource"], 0, 6) || !integerIn(in["conflictSource"], 0, 6)) {
    error = "Fallback and conflict outputs must be Black (0) or input 1–6"; return false;
  }
  if (!in["respectExternalControl"].is<bool>()) { error = "respectExternalControl must be boolean"; return false; }
  String policy = in["multiPolicy"].as<String>();
  if (policy != "round_robin" && policy != "priority" && policy != "conflict") {
    error = "Unknown multiple-lock policy"; return false;
  }
  JsonArrayConst inputs = in["inputs"].as<JsonArrayConst>();
  if (inputs.size() != datv::kInputCount) { error = "Exactly six input configurations are required"; return false; }
  uint8_t used = 0;
  for (size_t i = 0; i < datv::kInputCount; ++i) {
    auto input = inputs[i].as<JsonObjectConst>();
    if (!textIn(input["label"], 1, 32) || !integerIn(input["source"], 1, 6) ||
        !integerIn(input["priority"], 1, 6) || !input["enabled"].is<bool>() || !input["activeHigh"].is<bool>()) {
      error = String("Invalid configuration for alarm ") + (i + 1); return false;
    }
    auto& rule = next.arbitration.inputs[i];
    rule.enabled = input["enabled"].as<bool>();
    rule.source = input["source"].as<uint16_t>();
    rule.priority = input["priority"].as<uint8_t>();
    rule.activeHigh = input["activeHigh"].as<bool>();
    if (rule.enabled && (used & (1 << (rule.source - 1)))) { error = "Enabled alarms must map to distinct ATEM inputs"; return false; }
    if (rule.enabled) used |= 1 << (rule.source - 1);
    next.labels[i] = input["label"].as<String>();
  }
  next.atemIp = ipText;
  next.hostname = host;
  next.wifiSsid = in["wifiSsid"].as<String>();
  if (loading || clearWifi || !wifi.isEmpty()) next.wifiPassword = wifi;
  if (!admin.isEmpty()) next.adminPassword = admin;
  next.arbitration.acquireMs = in["acquireMs"].as<uint32_t>();
  next.arbitration.releaseMs = in["releaseMs"].as<uint32_t>();
  next.arbitration.switchIntervalMs = in["switchIntervalSec"].as<uint32_t>() * 1000;
  next.arbitration.fallbackSource = in["fallbackSource"].as<uint16_t>();
  next.arbitration.conflictSource = in["conflictSource"].as<uint16_t>();
  next.arbitration.policy = policy == "priority" ? datv::MultiPolicy::Priority :
                            policy == "conflict" ? datv::MultiPolicy::Conflict : datv::MultiPolicy::RoundRobin;
  next.respectExternalControl = in["respectExternalControl"].as<bool>();
  return true;
}

bool saveSettings(const Settings& settings) {
  DynamicJsonDocument doc(8192);
  settingsToJson(settings, doc.to<JsonObject>(), true);
  if (doc.overflowed()) return false;
  String serialized;
  serializeJson(doc, serialized);
  Preferences store;
  if (!store.begin("datv-atem", false)) return false;
  bool ok = store.putString("settings", serialized) > 0;
  store.end();
  return ok;
}

bool loadSettings(Settings& settings) {
  Preferences store;
  if (!store.begin("datv-atem", true)) return false;
  String serialized = store.getString("settings", "");
  store.end();
  if (serialized.isEmpty() || serialized.length() > 8192) return false;
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, serialized) || doc["version"] != 1) return false;
  Settings next = settings;
  String error;
  if (!settingsFromJson(doc.as<JsonObjectConst>(), next, error, true)) return false;
  settings = next;
  return true;
}
