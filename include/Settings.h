#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "Arbitration.h"

struct Settings {
  String atemIp;
  String hostname = "datv-atem";
  String wifiSsid;
  String wifiPassword;
  String adminPassword;
  std::array<String, datv::kInputCount> labels;
  datv::ArbitrationConfig arbitration;
  bool respectExternalControl = true;
};

Settings defaultSettings();
const char* policyName(datv::MultiPolicy policy);
void settingsToJson(const Settings& settings, JsonObject out, bool withSecrets = false);
bool settingsFromJson(JsonObjectConst in, Settings& next, String& error, bool loading = false);
bool loadSettings(Settings& settings);
bool saveSettings(const Settings& settings);
