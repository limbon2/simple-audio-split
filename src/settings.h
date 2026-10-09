// settings.h - What the user picked, persisted in %APPDATA%\SimpleAudioSplit\settings.ini,
// and the "Start with Windows" entry.
#pragma once

#include <string>
#include <vector>

namespace sas {

struct SavedDevice {
  std::wstring id;
  std::wstring name;  // lets a device be recognised again after it moves to another USB port
};

struct Settings {
  bool enabled = true;
  bool trayHintShown = false;
  std::vector<SavedDevice> targets;
};

Settings loadSettings();
void saveSettings(const Settings& settings);

bool autostartEnabled();
bool setAutostart(bool enable);

}  // namespace sas
