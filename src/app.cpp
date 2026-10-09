// app.cpp - The window and tray icon around the engine.
//
// The list shows every playback device with a checkbox: ticked devices play the PC's
// sound. The Windows default output plays it anyway (it is what gets copied); every
// other ticked device receives a copy. Closing the window keeps the app running in the
// notification area; "Start with Windows" launches it straight into the tray.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "../res/resource.h"
#include "engine.h"
#include "settings.h"
#include "tone.h"
#include "wasapi_util.h"

using namespace sas;

namespace {

constexpr wchar_t kTitle[] = L"Simple Audio Split";
constexpr UINT WM_APP_DEVICES = WM_APP + 1;    // the set of devices changed
constexpr UINT WM_APP_TRAY = WM_APP + 2;       // notification-area icon events
constexpr UINT WM_APP_TEST_DONE = WM_APP + 3;  // test sound finished; wParam = HRESULT
constexpr UINT WM_APP_LAYOUT = WM_APP + 4;     // re-fit the list columns
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kTrayId = 1;

struct Row {
  std::wstring id;
  std::wstring name;
  bool present = false;    // connected right now
  bool isDefault = false;  // the Windows default output: the source of the sound
  bool checked = false;
  std::wstring status;     // last text shown in the Status column
};

struct App {
  HINSTANCE inst = nullptr;
  HWND dlg = nullptr;
  HWND list = nullptr;
  Engine engine;
  Settings settings;
  std::vector<Row> rows;
  bool populating = false;
  bool testRunning = false;
  bool trayAdded = false;
  HICON iconOn = nullptr;
  HICON iconOff = nullptr;
  HICON iconBig = nullptr;
  UINT taskbarCreatedMsg = 0;
  UINT showMsg = 0;
  std::wstring statusText;
  std::wstring trayTip;
};

App g;

bool isSaved(const std::wstring& id) {
  return std::any_of(g.settings.targets.begin(), g.settings.targets.end(),
                     [&](const SavedDevice& d) { return d.id == id; });
}

void applyConfig() {
  EngineConfig cfg;
  cfg.enabled = g.settings.enabled;
  for (const SavedDevice& d : g.settings.targets) cfg.targetIds.push_back(d.id);
  g.engine.setConfig(cfg);
}

// ---------------------------------------------------------------------------
// Notification-area icon

void updateTray() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g.dlg;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = g.settings.enabled ? g.iconOn : g.iconOff;
  wcsncpy_s(nid.szTip, g.trayTip.c_str(), _TRUNCATE);
  if (!g.trayAdded) {
    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (g.trayAdded) Shell_NotifyIconW(NIM_SETVERSION, &nid);
  } else {
    Shell_NotifyIconW(NIM_MODIFY, &nid);
  }
}

void removeTray() {
  if (!g.trayAdded) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g.dlg;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  g.trayAdded = false;
}

void showTrayBalloon(const wchar_t* title, const wchar_t* text) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g.dlg;
  nid.uID = kTrayId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  nid.hBalloonIcon = g.iconBig;
  wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(nid.szInfo, text, _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------
// Status

std::wstring targetStatus(const Row& row, const EngineStatus& st) {
  if (!row.present) return row.checked ? L"Not connected" : L"";
  if (row.isDefault) return L"Default output";
  if (!row.checked) return L"";
  if (!g.settings.enabled) return L"Off";
  const auto it = std::find_if(st.targets.begin(), st.targets.end(),
                               [&](const TargetStatus& t) { return t.id == row.id; });
  if (it == st.targets.end()) return L"Starting…";
  switch (it->state) {
    case TargetState::Playing: {
      wchar_t buf[64];
      swprintf_s(buf, L"Playing · %d ms behind", static_cast<int>(std::lround(it->latencyMs)));
      return buf;
    }
    case TargetState::Starting: return L"Starting…";
    case TargetState::IsSource: return L"Default output";
    case TargetState::Unavailable: return L"Not connected";
    case TargetState::Error: return describeError(it->error);
    case TargetState::Idle: return L"Waiting for the default output";
  }
  return L"";
}

std::wstring summary(const EngineStatus& st) {
  if (!g.settings.enabled) return L"Mirroring is off.";
  const auto ticked = std::count_if(g.rows.begin(), g.rows.end(), [](const Row& r) { return r.checked && !r.isDefault; });
  if (ticked == 0) return L"Tick a device to copy the sound to it.";
  if (!st.capturing && st.sourceError) return L"Can't capture the default output: " + describeError(st.sourceError);
  const auto playing = std::count_if(st.targets.begin(), st.targets.end(),
                                     [](const TargetStatus& t) { return t.state == TargetState::Playing; });
  if (playing == 0) return L"Waiting for the ticked devices…";
  return L"Copying the default output to " + std::to_wstring(playing) + (playing == 1 ? L" device." : L" devices.");
}

void updateStatus() {
  const EngineStatus st = g.engine.status();
  if (IsWindowVisible(g.dlg)) {
    for (size_t i = 0; i < g.rows.size(); ++i) {
      std::wstring text = targetStatus(g.rows[i], st);
      if (text != g.rows[i].status) {
        g.rows[i].status = std::move(text);
        ListView_SetItemText(g.list, static_cast<int>(i), 1, g.rows[i].status.data());
      }
    }
  }
  std::wstring text = summary(st);
  if (text != g.statusText) {
    g.statusText = text;
    SetDlgItemTextW(g.dlg, IDC_STATUS, text.c_str());
  }
  std::wstring tip = std::wstring(kTitle) + L"\n" + text;
  if (tip != g.trayTip || !g.trayAdded) {
    g.trayTip = std::move(tip);
    updateTray();
  }
}

// ---------------------------------------------------------------------------
// Device list

int selectedRow() { return ListView_GetNextItem(g.list, -1, LVNI_SELECTED); }

void updateTestButton() {
  const int sel = selectedRow();
  const bool ok = !g.testRunning && sel >= 0 && sel < static_cast<int>(g.rows.size()) && g.rows[sel].present;
  EnableWindow(GetDlgItem(g.dlg, IDC_TEST), ok);
}

void layoutColumns() {
  RECT rc;
  GetClientRect(g.list, &rc);
  const int statusWidth = MulDiv(150, GetDpiForWindow(g.list), 96);
  ListView_SetColumnWidth(g.list, 1, statusWidth);
  ListView_SetColumnWidth(g.list, 0, std::max<int>(60, rc.right - statusWidth));
}

void rebuildRows() {
  std::vector<DeviceInfo> devices;
  ComPtr<IMMDeviceEnumerator> en;
  if (SUCCEEDED(createEnumerator(en))) listOutputDevices(en.Get(), devices);
  auto present = [&](const std::wstring& id) {
    return std::any_of(devices.begin(), devices.end(), [&](const DeviceInfo& d) { return d.id == id; });
  };

  // A device plugged into another USB port comes back with a new ID but the same
  // name: carry the user's tick over to it. Also keep saved names current.
  bool changed = false;
  for (SavedDevice& t : g.settings.targets) {
    for (const DeviceInfo& d : devices) {
      if (d.id == t.id && d.name != t.name) {
        t.name = d.name;
        changed = true;
      }
    }
    if (present(t.id)) continue;
    for (const DeviceInfo& d : devices) {
      if (d.name == t.name && !isSaved(d.id)) {
        t.id = d.id;
        changed = true;
        break;
      }
    }
  }
  if (changed) {
    saveSettings(g.settings);
    applyConfig();
  }

  std::vector<Row> rows;
  for (const DeviceInfo& d : devices) {
    Row r;
    r.id = d.id;
    r.name = d.name;
    r.present = true;
    r.isDefault = d.isDefault;
    r.checked = isSaved(d.id);
    rows.push_back(std::move(r));
  }
  std::stable_partition(rows.begin(), rows.end(), [](const Row& r) { return r.isDefault; });  // the source on top
  for (const SavedDevice& t : g.settings.targets) {
    if (present(t.id)) continue;
    Row r;
    r.id = t.id;
    r.name = t.name.empty() ? L"Unknown device" : t.name;
    r.checked = true;
    rows.push_back(std::move(r));
  }

  const int sel = selectedRow();
  const std::wstring selectedId = sel >= 0 && sel < static_cast<int>(g.rows.size()) ? g.rows[sel].id : L"";
  g.rows = std::move(rows);
  g.populating = true;
  SendMessageW(g.list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(g.list);
  int newSel = -1;
  for (size_t i = 0; i < g.rows.size(); ++i) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(i);
    item.pszText = g.rows[i].name.data();
    ListView_InsertItem(g.list, &item);
    ListView_SetCheckState(g.list, static_cast<int>(i), g.rows[i].checked);
    if (g.rows[i].id == selectedId) newSel = static_cast<int>(i);
  }
  if (newSel < 0 && !g.rows.empty()) newSel = 0;
  if (newSel >= 0) ListView_SetItemState(g.list, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g.list, nullptr, TRUE);
  g.populating = false;
  layoutColumns();
  updateTestButton();
  updateStatus();
}

void onCheckChanged(int index, bool checked) {
  if (index < 0 || index >= static_cast<int>(g.rows.size())) return;
  Row& row = g.rows[index];
  row.checked = checked;
  auto& targets = g.settings.targets;
  targets.erase(std::remove_if(targets.begin(), targets.end(), [&](const SavedDevice& d) { return d.id == row.id; }),
                targets.end());
  if (checked) targets.push_back({row.id, row.name});
  saveSettings(g.settings);
  applyConfig();
  if (!row.present && !checked) PostMessageW(g.dlg, WM_APP_DEVICES, 0, 0);  // drop the row of an unplugged device
  updateStatus();
}

// ---------------------------------------------------------------------------
// Actions

void setEnabled(bool enabled) {
  g.settings.enabled = enabled;
  saveSettings(g.settings);
  applyConfig();
  CheckDlgButton(g.dlg, IDC_ENABLED, enabled ? BST_CHECKED : BST_UNCHECKED);
  g.trayTip.clear();  // force the icon to refresh
  updateStatus();
}

void showMainWindow() {
  if (IsIconic(g.dlg)) ShowWindow(g.dlg, SW_RESTORE);
  ShowWindow(g.dlg, SW_SHOW);
  SetForegroundWindow(g.dlg);
  rebuildRows();
}

void hideToTray() {
  ShowWindow(g.dlg, SW_HIDE);
  if (!g.settings.trayHintShown && g.trayAdded) {
    showTrayBalloon(L"Still running", L"The sound keeps being copied in the background. To get back here, "
                                      L"click the tray icon or start the app again. Use Exit to stop it.");
    g.settings.trayHintShown = true;
    saveSettings(g.settings);
  }
}

void startTestSound() {
  const int sel = selectedRow();
  if (g.testRunning || sel < 0 || sel >= static_cast<int>(g.rows.size()) || !g.rows[sel].present) return;
  g.testRunning = true;
  updateTestButton();
  const std::wstring id = g.rows[sel].id;
  const HWND dlg = g.dlg;
  std::thread([id, dlg] {
    const HRESULT hr = playTestSound(id);
    PostMessageW(dlg, WM_APP_TEST_DONE, static_cast<WPARAM>(hr), 0);
  }).detach();
}

void showTrayMenu(int x, int y) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, IDM_OPEN, L"&Open Simple Audio Split");
  AppendMenuW(menu, MF_STRING | (g.settings.enabled ? MF_CHECKED : MF_UNCHECKED), IDM_ENABLED, L"&Mirroring on");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_EXIT, L"E&xit");
  SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
  SetForegroundWindow(g.dlg);  // so the menu closes when clicking elsewhere
  const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, x, y, g.dlg, nullptr);
  PostMessageW(g.dlg, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Dialog

void initDialog(HWND dlg) {
  g.dlg = dlg;
  g.list = GetDlgItem(dlg, IDC_DEVICES);
  SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g.iconBig));
  SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g.iconOn));

  ListView_SetExtendedListViewStyle(g.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  LVCOLUMNW col{};
  col.mask = LVCF_TEXT | LVCF_WIDTH;
  col.cx = 200;
  col.pszText = const_cast<wchar_t*>(L"Device");
  ListView_InsertColumn(g.list, 0, &col);
  col.pszText = const_cast<wchar_t*>(L"Status");
  ListView_InsertColumn(g.list, 1, &col);

  CheckDlgButton(dlg, IDC_ENABLED, g.settings.enabled ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(dlg, IDC_STARTUP, autostartEnabled() ? BST_CHECKED : BST_UNCHECKED);

  g.engine.start(dlg, WM_APP_DEVICES);
  applyConfig();
  rebuildRows();
  SetTimer(dlg, kStatusTimer, 500, nullptr);
}

INT_PTR handleNotify(HWND dlg, const NMHDR* hdr) {
  if (hdr->idFrom != IDC_DEVICES) return FALSE;
  switch (hdr->code) {
    case LVN_ITEMCHANGED: {
      const auto* nm = reinterpret_cast<const NMLISTVIEW*>(hdr);
      if (g.populating || !(nm->uChanged & LVIF_STATE)) return FALSE;
      const UINT oldCheck = nm->uOldState & LVIS_STATEIMAGEMASK;
      const UINT newCheck = nm->uNewState & LVIS_STATEIMAGEMASK;
      if (oldCheck != newCheck && oldCheck != 0 && newCheck != 0)
        onCheckChanged(nm->iItem, newCheck == INDEXTOSTATEIMAGEMASK(2));
      if ((nm->uOldState ^ nm->uNewState) & LVIS_SELECTED) updateTestButton();
      return FALSE;
    }
    case NM_CUSTOMDRAW: {  // grey out devices that are ticked but not connected
      auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(hdr));
      LRESULT result = CDRF_DODEFAULT;
      if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
        result = CDRF_NOTIFYITEMDRAW;
      } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const size_t i = cd->nmcd.dwItemSpec;
        if (i < g.rows.size() && !g.rows[i].present) cd->clrText = GetSysColor(COLOR_GRAYTEXT);
      }
      SetWindowLongPtrW(dlg, DWLP_MSGRESULT, result);
      return TRUE;
    }
  }
  return FALSE;
}

INT_PTR CALLBACK dialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == g.showMsg && msg != 0) {
    showMainWindow();
    return TRUE;
  }
  if (msg == g.taskbarCreatedMsg && msg != 0) {  // Explorer restarted: the icon is gone
    g.trayAdded = false;
    updateTray();
    return TRUE;
  }
  switch (msg) {
    case WM_INITDIALOG:
      initDialog(dlg);
      return TRUE;
    case WM_TIMER:
      if (wParam == kStatusTimer) updateStatus();
      return TRUE;
    case WM_APP_DEVICES:
      rebuildRows();
      return TRUE;
    case WM_APP_TEST_DONE: {
      g.testRunning = false;
      updateTestButton();
      const HRESULT hr = static_cast<HRESULT>(wParam);
      if (FAILED(hr)) {
        const std::wstring text = L"Couldn't play the test sound: " + describeError(hr);
        MessageBoxW(dlg, text.c_str(), kTitle, MB_OK | MB_ICONWARNING);
      }
      return TRUE;
    }
    case WM_APP_LAYOUT:
      layoutColumns();
      return TRUE;
    case WM_DPICHANGED:
      PostMessageW(dlg, WM_APP_LAYOUT, 0, 0);  // after the dialog has rescaled itself
      return FALSE;
    case WM_NOTIFY:
      return handleNotify(dlg, reinterpret_cast<const NMHDR*>(lParam));
    case WM_APP_TRAY:
      switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
          showMainWindow();
          break;
        case WM_CONTEXTMENU:
          showTrayMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
          break;
      }
      return TRUE;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDC_TEST:
          startTestSound();
          return TRUE;
        case IDC_ENABLED:
          setEnabled(IsDlgButtonChecked(dlg, IDC_ENABLED) == BST_CHECKED);
          return TRUE;
        case IDC_STARTUP: {
          const bool want = IsDlgButtonChecked(dlg, IDC_STARTUP) == BST_CHECKED;
          if (!setAutostart(want)) {
            MessageBoxW(dlg, L"Couldn't change the startup setting.", kTitle, MB_OK | MB_ICONWARNING);
            CheckDlgButton(dlg, IDC_STARTUP, autostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
          }
          return TRUE;
        }
        case IDCANCEL:  // Close button, Esc and the title bar's X
          hideToTray();
          return TRUE;
        case IDM_OPEN:
          showMainWindow();
          return TRUE;
        case IDM_ENABLED:
          setEnabled(!g.settings.enabled);
          return TRUE;
        case IDM_EXIT:
          DestroyWindow(dlg);
          return TRUE;
      }
      return FALSE;
    case WM_DESTROY:
      KillTimer(dlg, kStatusTimer);
      removeTray();
      PostQuitMessage(0);
      return TRUE;
  }
  return FALSE;
}

HICON loadIcon(int id, int size) {
  return static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
  g.inst = inst;
  g.showMsg = RegisterWindowMessageW(L"SimpleAudioSplit.Show");
  g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

  // One instance only: a second launch just brings the first one's window up.
  HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\SimpleAudioSplit.Instance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Our message boxes share the dialog's class and title; only the main window reacts.
    for (HWND other = nullptr; (other = FindWindowExW(nullptr, other, L"#32770", kTitle)) != nullptr;) {
      DWORD pid = 0;
      GetWindowThreadProcessId(other, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(other, g.showMsg, 0, 0);
    }
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);

  const int smallIcon = GetSystemMetrics(SM_CXSMICON);
  g.iconOn = loadIcon(IDI_APP, smallIcon);
  g.iconOff = loadIcon(IDI_APP_OFF, smallIcon);
  g.iconBig = loadIcon(IDI_APP, GetSystemMetrics(SM_CXICON));
  g.settings = loadSettings();

  if (!CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialogProc, 0)) return 1;
  const bool startInTray = cmdLine && wcsstr(cmdLine, L"--tray");
  if (!startInTray) showMainWindow();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(g.dlg, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  g.engine.stop();
  DestroyIcon(g.iconOn);
  DestroyIcon(g.iconOff);
  DestroyIcon(g.iconBig);
  CoUninitialize();
  if (instanceMutex) CloseHandle(instanceMutex);
  return 0;
}
