#include "settings.h"

#include <windows.h>
#include <shlobj.h>

namespace sas {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"SimpleAudioSplit";

std::wstring exePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n < path.size()) {
      path.resize(n);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

std::wstring autostartCommand() { return L"\"" + exePath() + L"\" --tray"; }

std::wstring settingsPath() {
  std::wstring dir;
  PWSTR appData = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
  CoTaskMemFree(appData);
  dir += L"\\SimpleAudioSplit";
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring path = dir + L"\\settings.ini";
  // The profile API only writes Unicode into a file that already starts with a
  // UTF-16 byte order mark; without it, non-English device names would be mangled.
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    const unsigned char bom[] = {0xFF, 0xFE};
    DWORD written = 0;
    WriteFile(f, bom, sizeof bom, &written, nullptr);
    CloseHandle(f);
  }
  return path;
}

std::wstring readString(const std::wstring& path, const wchar_t* section, const std::wstring& key) {
  std::wstring buf(512, L'\0');
  for (;;) {
    const DWORD n = GetPrivateProfileStringW(section, key.c_str(), L"", buf.data(), static_cast<DWORD>(buf.size()),
                                             path.c_str());
    if (n + 1 < buf.size()) {
      buf.resize(n);
      return buf;
    }
    buf.resize(buf.size() * 2);
  }
}

}  // namespace

Settings loadSettings() {
  const std::wstring path = settingsPath();
  Settings s;
  s.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, path.c_str()) != 0;
  s.trayHintShown = GetPrivateProfileIntW(L"General", L"TrayHintShown", 0, path.c_str()) != 0;
  const UINT count = GetPrivateProfileIntW(L"Targets", L"Count", 0, path.c_str());
  for (UINT i = 0; i < count && i < 64; ++i) {
    SavedDevice d;
    d.id = readString(path, L"Targets", L"Id" + std::to_wstring(i));
    d.name = readString(path, L"Targets", L"Name" + std::to_wstring(i));
    if (!d.id.empty()) s.targets.push_back(std::move(d));
  }
  return s;
}

void saveSettings(const Settings& s) {
  const std::wstring path = settingsPath();
  const wchar_t* file = path.c_str();
  WritePrivateProfileStringW(L"General", L"Enabled", s.enabled ? L"1" : L"0", file);
  WritePrivateProfileStringW(L"General", L"TrayHintShown", s.trayHintShown ? L"1" : L"0", file);
  WritePrivateProfileStringW(L"Targets", nullptr, nullptr, file);  // drop the old list
  WritePrivateProfileStringW(L"Targets", L"Count", std::to_wstring(s.targets.size()).c_str(), file);
  for (size_t i = 0; i < s.targets.size(); ++i) {
    WritePrivateProfileStringW(L"Targets", (L"Id" + std::to_wstring(i)).c_str(), s.targets[i].id.c_str(), file);
    WritePrivateProfileStringW(L"Targets", (L"Name" + std::to_wstring(i)).c_str(), s.targets[i].name.c_str(), file);
  }
}

bool autostartEnabled() {
  wchar_t buf[2048];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
    return false;
  return _wcsicmp(buf, autostartCommand().c_str()) == 0;  // an entry for a moved copy doesn't count
}

bool setAutostart(bool enable) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS)
    return false;
  LSTATUS st;
  if (enable) {
    const std::wstring cmd = autostartCommand();
    st = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    st = RegDeleteValueW(key, kRunValue);
    if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return st == ERROR_SUCCESS;
}

}  // namespace sas
