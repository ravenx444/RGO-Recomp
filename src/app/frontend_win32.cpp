// Native window ownership is independent of the ROM-derived game process.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // Must precede commdlg.h and shellapi.h.

#include <algorithm>
#include <commdlg.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include "jfg/renderer/widescreen.hpp"
#ifdef JFG_RML_UI
#include "native_ui.hpp"
#endif
namespace fs = std::filesystem;
namespace {
constexpr UINT kReady = WM_APP + 20, kHotkey = WM_APP + 21;
#ifdef JFG_RML_UI
constexpr int kMuteShortcut = 0x4a46;
#endif
enum : UINT {
  Play = 100,
  Stop,
  Setup,
  Cancel,
  Rom,
  Runtime,
  Controllers,
  Audio,
  Fullscreen,
  Map,
  Inventory,
  Support,
  Saves,
  Guide,
  Quit,
  Mods
};
struct Process {
  HANDLE handle = nullptr, job = nullptr;
  void release() {
    if (handle)
      CloseHandle(handle);
    if (job)
      CloseHandle(job);
    handle = job = nullptr;
  }
  bool running() const {
    return handle && WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
  }
  ~Process() { release(); }
};
struct App {
  HWND window = nullptr, viewport = nullptr, render = nullptr, status = nullptr,
       rom = nullptr, runtime = nullptr, shield = nullptr;
  bool uiReady = false;
  bool muteShortcutFocus = false, muteShortcutRegistered = false;
  std::vector<HWND> home;
  HMENU menu = nullptr;
  HFONT font = nullptr, titleFont = nullptr;
  HBRUSH background = CreateSolidBrush(RGB(18, 27, 40));
  fs::path profile, helper;
  std::wstring inputReplay, progressOutput;
  bool quickPlay = false;
  Process session, dialog;
  std::vector<std::unique_ptr<Process>> tools;
  std::vector<HWND> liveModals;
  bool playing = false, setting = false, fullscreen = false, closing = false,
       mods = false, pauseInactive = false, manualPause = false,
       widescreen = true;
  bool stopRequested = false, initializing = true;
  ULONGLONG startedAt = 0;
  UINT dpi = 96;
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  std::wstring lastStatus;
} app;
std::wstring wide(const std::string &text) {
  if (text.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring result(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), n);
  return result;
}
std::string utf8(const std::wstring &text) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string result(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), n, nullptr, nullptr);
  return result;
}
std::wstring read(const wchar_t *name) {
  std::ifstream stream(app.profile / name, std::ios::binary | std::ios::ate);
  if (!stream || stream.tellg() > 65536)
    return {};
  stream.seekg(0);
  std::string value{std::istreambuf_iterator<char>(stream), {}};
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
    value.pop_back();
  return wide(value);
}
bool write(const wchar_t *name, const std::wstring &value) {
  const auto path = app.profile / name;
  const auto temporary = fs::path(path.wstring() + L".tmp");
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << utf8(value);
    file.flush();
    if (!file)
      return false;
  }
  return MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
         FALSE;
}
// RGO widescreen: the game follows the same profile file while it runs.
bool readWidescreen() {
  // read() drops the final line break that the settings format requires.
  const auto settings =
      jfg::parse_video_settings(utf8(read(L"video.ini")) + "\n");
  return settings.value_or(jfg::VideoSettings{}).aspect ==
         jfg::AspectMode::widescreen;
}
std::wstring text(HWND control) {
  int n = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(n + 1), L'\0');
  GetWindowTextW(control, value.data(), n + 1);
  value.resize(static_cast<std::size_t>(n));
  return value;
}
std::wstring quote(const std::wstring &value) {
  std::wstring out = L"\"";
  std::size_t slashes = 0;
  for (wchar_t ch : value) {
    if (ch == L'\\') {
      ++slashes;
      continue;
    }
    out.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (ch == L'"')
      out += L'\\';
    out += ch;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}
void status(const std::wstring &value) {
  app.lastStatus = value;
  SetWindowTextW(app.status, value.c_str());
}
bool launch(Process &process, const wchar_t *action, HWND owner) {
  if (process.running())
    return false;
  process.release();
  std::wstring command =
      quote(app.helper.wstring()) + L" --frontend-worker " + action + L" " +
      quote(app.profile.wstring()) + L" " +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(owner));
  if (std::wstring(action) == L"play") {
    if (!app.inputReplay.empty())
      command += L" --input-replay " + quote(app.inputReplay);
    if (!app.progressOutput.empty())
      command += L" --progress-output " + quote(app.progressOutput);
  }
  fs::path executable=app.helper;
#ifdef JFG_RML_UI
  if(std::wstring(action)==L"map" || std::wstring(action)==L"inventory" || std::wstring(action)==L"controllers") {
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);executable=module;
    command=quote(executable.wstring())+L" --live-tool "+action+L" --profile "+quote(app.profile.wstring())+L" --helper "+quote(app.helper.wstring())+L" --owner "+std::to_wstring(reinterpret_cast<std::uintptr_t>(owner));
  }
#endif
  STARTUPINFOW start{};
  start.cb = sizeof(start);
  PROCESS_INFORMATION child{};
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits))) {
    if (job)
      CloseHandle(job);
    status(L"Cannot create a supervised session.");
    return false;
  }
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      app.helper.parent_path().c_str(), &start, &child)) {
    CloseHandle(job);
    status(
        L"The setup helper is missing. Extract the complete launcher package.");
    return false;
  }
  if (!AssignProcessToJobObject(job, child.hProcess)) {
    TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CloseHandle(job);
    status(L"Cannot supervise the session.");
    return false;
  }
  process.handle = child.hProcess;
  process.job = job;
  ResumeThread(child.hThread);
  CloseHandle(child.hThread);
  return true;
}
int scaled(int value) { return MulDiv(value, static_cast<int>(app.dpi), 96); }
void fonts() {
  HFONT oldFont = app.font, oldTitle = app.titleFont;
  app.font =
      CreateFontW(-scaled(18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  app.titleFont =
      CreateFontW(-scaled(38), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  for (HWND control : app.home)
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
  if (!app.home.empty())
    SendMessageW(app.home.front(), WM_SETFONT,
                 reinterpret_cast<WPARAM>(app.titleFont), TRUE);
  if (app.status)
    SendMessageW(app.status, WM_SETFONT, reinterpret_cast<WPARAM>(app.font),
                 TRUE);
  if (oldFont)
    DeleteObject(oldFont);
  if (oldTitle)
    DeleteObject(oldTitle);
}
void layout() {
  RECT area{};
  GetClientRect(app.window, &area);
#ifdef JFG_RML_UI
  const int top = app.fullscreen ? 0 : scaled(24);
  MoveWindow(app.viewport, 0, top, std::max<LONG>(1, area.right),
             std::max<LONG>(1, area.bottom - top), TRUE);
  if (app.render && IsWindow(app.render))
    MoveWindow(app.render, 0, 0, std::max<LONG>(1, area.right),
               std::max<LONG>(1, area.bottom - top), TRUE);
  if (app.shield)
    MoveWindow(app.shield, 0, top, std::max<LONG>(1, area.right),
               std::max<LONG>(1, area.bottom - top), FALSE);
  if (app.uiReady)
    FrontendUiResize();
#else
  const int w = MulDiv(area.right, 96, static_cast<int>(app.dpi));
  const int footer =
      (app.fullscreen && app.render && IsWindow(app.render)) ? 0 : 44;
  ShowWindow(app.status, footer ? SW_SHOW : SW_HIDE);
  MoveWindow(app.status, scaled(20),
             std::max<LONG>(0, area.bottom - scaled(36)),
             std::max<LONG>(1, area.right - scaled(40)), scaled(28), TRUE);
  MoveWindow(app.viewport, 0, 0, std::max<LONG>(1, area.right),
             std::max<LONG>(1, area.bottom - scaled(footer)), TRUE);
  if (app.render && IsWindow(app.render))
    MoveWindow(app.render, 0, 0, std::max<LONG>(1, area.right),
               std::max<LONG>(1, area.bottom - scaled(footer)), TRUE);
  if (app.home.size() >= 10) {
    const int left = 40, width = std::max(250, w - 80);
    auto move = [](HWND control, int x, int y, int width, int height) {
      MoveWindow(control, scaled(x), scaled(y), scaled(width), scaled(height),
                 TRUE);
    };
    move(app.home[0], left, 38, width, 58);
    move(app.home[1], left, 104, width, 48);
    move(app.home[2], left, 178, width, 24);
    move(app.rom, left, 210, std::max(90, width - 120), 30);
    move(app.home[4], w - 150, 208, 110, 34);
    move(app.home[5], left, 264, width, 24);
    move(app.runtime, left, 296, std::max(90, width - 120), 30);
    move(app.home[7], w - 150, 294, 110, 34);
    move(app.home[8], left, 366, 190, 46);
    move(app.home[9], left + 210, 366, 190, 46);
  }
#endif
}

void showHome(bool show) {
  for (HWND control : app.home) {
#ifdef JFG_RML_UI
    ShowWindow(control, SW_HIDE);
#else
    ShowWindow(control, show ? SW_SHOW : SW_HIDE);
#endif
    EnableWindow(control, !app.session.running());
  }
  ShowWindow(app.viewport, show ? SW_HIDE : SW_SHOW);
  EnableMenuItem(app.menu, Play,
                 MF_BYCOMMAND |
                     (app.session.running() ? MF_GRAYED : MF_ENABLED));
  EnableMenuItem(app.menu, Stop,
                 MF_BYCOMMAND | (app.playing ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(app.menu, Setup,
                 MF_BYCOMMAND |
                     (app.session.running() ? MF_GRAYED : MF_ENABLED));
  DrawMenuBar(app.window);
  layout();
}
void fullscreen() {
#ifdef JFG_RML_UI
  if (app.uiReady && FrontendUiCapturing() && !FrontendUiModalOpen())
    FrontendUiPanel("");
#endif
  if (!app.fullscreen) {
    GetWindowPlacement(app.window, &app.placement);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTONEAREST),
                    &monitor);
    app.fullscreen = true;
    SetMenu(app.window, nullptr);
    SetWindowLongPtrW(app.window, GWL_STYLE,
                      WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
    SetWindowPos(app.window, nullptr, monitor.rcMonitor.left,
                 monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED);
  } else {
    app.fullscreen = false;
#ifndef JFG_RML_UI
    SetMenu(app.window, app.menu);
#endif
    SetWindowLongPtrW(app.window, GWL_STYLE,
                      WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
    SetWindowPlacement(app.window, &app.placement);
    SetWindowPos(app.window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  CheckMenuItem(app.menu, Fullscreen,
                MF_BYCOMMAND | (app.fullscreen ? MF_CHECKED : MF_UNCHECKED));
  layout();
}
void pick(bool rom) {
  std::vector<wchar_t> path(32768);
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = app.window;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrFilter = rom ? L"N64 ROM\0*.z64;*.n64;*.v64\0All files\0*.*\0"
                           : L"Native game build\0jfg-native-boot.exe\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&dialog)) {
    SetWindowTextW(rom ? app.rom : app.runtime, path.data());
    write(rom ? L"frontend-rom.txt" : L"frontend-runtime.txt", path.data());
  }
}
void capture(bool enabled) {
  app.setting = enabled;
  if (enabled)
    SetPropW(app.window, L"JfgSettingsOpen", reinterpret_cast<HANDLE>(1));
  else {
    RemovePropW(app.window, L"JfgSettingsOpen");
    if (app.render && IsWindow(app.render))
      SetFocus(app.render);
  }
}
void settings(const wchar_t *action) {
  if (app.dialog.running())
    return;
  capture(true);
  if (!launch(app.dialog, action, app.window))
    capture(false);
}
void popup() {
#ifdef JFG_RML_UI
  FrontendUiPanel(FrontendUiCapturing() ? "" : "game");
  return;
#endif
  capture(true);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, Controllers, L"Controllers");
  AppendMenuW(menu, MF_STRING, Audio, L"Audio");
  AppendMenuW(menu, MF_STRING, Fullscreen, L"Toggle fullscreen");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, Stop, L"Stop game / return home");
  POINT p{40, 40};
  ClientToScreen(app.window, &p);
  UINT command = static_cast<UINT>(TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN, p.x, p.y, 0, app.window, nullptr));
  DestroyMenu(menu);
  capture(false);
  if (command)
    PostMessageW(app.window, WM_COMMAND, command, 0);
}
void persist() {
  write(L"frontend-rom.txt", text(app.rom));
  write(L"frontend-runtime.txt", text(app.runtime));
  write(L"frontend-mods.txt", app.mods ? L"1" : L"0");
  if (!app.fullscreen)
    GetWindowPlacement(app.window, &app.placement);
  const RECT &r = app.placement.rcNormalPosition;
  write(L"frontend-window.txt", std::to_wstring(r.left) + L" " +
                                    std::to_wstring(r.top) + L" " +
                                    std::to_wstring(r.right - r.left) + L" " +
                                    std::to_wstring(r.bottom - r.top) + L" " +
                                    (app.fullscreen ? L"1" : L"0"));
}
void stop() {
  app.manualPause = false;
  RemovePropW(app.window, L"JfgFrontendPause");
#ifdef JFG_RML_UI
  FrontendUiPanel("");
#endif
  app.stopRequested = true;
  if (app.render && IsWindow(app.render)) {
    PostMessageW(app.render, WM_CLOSE, 0, 0);
    status(L"Stopping game and flushing saves...");
  } else if (app.session.running())
    status(L"Cancelling game startup...");
}
HWND control(const wchar_t *cls, const wchar_t *label, DWORD style, int id) {
  HWND h = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0,
                           cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 100,
                           28, app.window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
  SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
  return h;
}
void menus() {
  app.menu = CreateMenu();
  auto add =
      [&](const wchar_t *name,
          std::initializer_list<std::pair<UINT, const wchar_t *>> items) {
        HMENU m = CreatePopupMenu();
        for (auto [id, label] : items)
          AppendMenuW(m, id ? MF_STRING : MF_SEPARATOR, id, label);
        AppendMenuW(app.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(m), name);
      };
  add(L"&Game", {{Play, L"&Play"},
                 {Stop, L"&Stop / return home"},
                 {Setup, L"Set up and &build"},
                 {Cancel, L"Cancel setup"},
                 {0, nullptr},
                 {Quit, L"&Quit"}});
  add(L"&Controllers",
      {{Controllers, L"Player 1-4 assignments and mappings"}});
  add(L"&Video", {{Fullscreen, L"&Fullscreen\tF11"}});
  add(L"&Audio", {{Audio, L"&Volume and mute"}});
  add(L"&Tools", {{Map, L"Live map"},
                  {Inventory, L"Live inventory"},
                  {Support, L"Support report"},
                  {Saves, L"Open saves"}});
  add(L"&Help", {{Guide, L"Setup guide"}});
#ifndef JFG_RML_UI
  SetMenu(app.window, app.menu);
#endif
}
#ifdef JFG_RML_UI
void uiAction(const std::string &a) {
  UINT command = 0;
  if (a == "pause" || a == "resume") {
    if (app.render && IsWindow(app.render)) app.manualPause = a == "pause";
    return;
  }
  if (a == "pause-inactive") {
    if (write(L"frontend-pause-inactive.txt", app.pauseInactive ? L"0" : L"1"))
      app.pauseInactive = !app.pauseInactive;
    else status(L"Cannot save the pause preference.");
    return;
  }
  if (a == "aspect-original" || a == "aspect-widescreen") {
    const jfg::VideoSettings settings{a == "aspect-widescreen"
                                          ? jfg::AspectMode::widescreen
                                          : jfg::AspectMode::original};
    if (write(L"video.ini", wide(jfg::encode_video_settings(settings))))
      app.widescreen = settings.aspect == jfg::AspectMode::widescreen;
    else status(L"Cannot save the video settings.");
    return;
  }
  if (a == "play") {
    if (text(app.rom).empty()) {
      pick(true);
      if (text(app.rom).empty())
        return;
    }
    std::error_code runtimeError;
    if (!fs::is_regular_file(fs::path(text(app.runtime)), runtimeError)) {
      FrontendUiPanel("setup");
      return;
    }
    command = Play;
  } else if (a == "stop")
    command = Stop;
  else if (a == "setup-start")
    command = Setup;
  else if (a == "cancel-setup")
    command = Cancel;
  else if (a == "rom")
    command = Rom;
  else if (a == "fullscreen")
    command = Fullscreen;
  else if (a == "controllers")
    command = Controllers;
  else if (a == "map")
    command = Map;
  else if (a == "inventory")
    command = Inventory;
  else if (a == "saves")
    command = Saves;
  else if (a == "guide")
    command = Guide;
  else if (a == "project" || a == "known-issues" || a == "notices") {
    const wchar_t *url = a == "project" ? L"https://github.com/TK22-26/JetForceGemini-Recomp"
        : a == "known-issues" ? L"https://github.com/TK22-26/JetForceGemini-Recomp/blob/main/docs/known-issues.md"
        : L"https://github.com/TK22-26/JetForceGemini-Recomp/blob/main/THIRD_PARTY_NOTICES.md";
    ShellExecuteW(app.window, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
  }
  else if (a == "mods")
    command = Mods;
  else if (a == "quit")
    command = Quit;
  else if (a == "sessions" || a == "export" || a == "assets" || a == "shortcut") {
    auto tool = std::make_unique<Process>();
    if (launch(*tool, a == "assets" ? L"assets" : a == "sessions" ? L"sessions" : a == "shortcut" ? L"shortcut" : L"export", app.window))
      app.tools.push_back(std::move(tool));
  } else if (a == "size720" || a == "size1080") {
    if (app.fullscreen)
      fullscreen();
    RECT r{0, 0, a == "size720" ? 1280 : 1920, a == "size720" ? 744 : 1104};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, app.dpi);
    SetWindowPos(app.window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
  }
  if (command)
    PostMessageW(app.window, WM_COMMAND, command, 0);
}
void syncUiCapture() {
  // Register only while this launcher is foreground. Windows delivers the
  // chord even when its hosted game process has keyboard focus, without
  // passing M through to gameplay or repeating while the keys are held.
  const bool shortcutFocus = GetForegroundWindow() == app.window;
  if (shortcutFocus != app.muteShortcutFocus) {
    app.muteShortcutFocus = shortcutFocus;
    if (app.muteShortcutRegistered) UnregisterHotKey(app.window, kMuteShortcut);
    app.muteShortcutRegistered = shortcutFocus &&
        RegisterHotKey(app.window, kMuteShortcut, MOD_CONTROL | MOD_NOREPEAT, 'M') != 0;
    if (shortcutFocus && !app.muteShortcutRegistered)
      status(L"Ctrl+M is already in use. Mute is available in the Audio menu.");
  }
  bool enabled = FrontendUiCapturing() || app.dialog.running();
  if (enabled != app.setting) {
    capture(enabled);
    ShowWindow(app.shield, enabled ? SW_SHOW : SW_HIDE);
    if (enabled) {
      SetWindowPos(app.shield, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      SetFocus(app.shield);
    }
  }
  // Manual pause, focus pause and modal capture are independent reasons. Regaining focus
  // must not resume a game while a settings dialog is still open.
  const bool inactive = GetForegroundWindow() != app.window || IsIconic(app.window);
  app.liveModals.erase(std::remove_if(app.liveModals.begin(),app.liveModals.end(),[](HWND w){return !IsWindow(w)||!GetPropW(w,L"JfgLiveModal");}),app.liveModals.end());
  if (app.manualPause || FrontendUiModalOpen() || app.dialog.running() || !app.liveModals.empty() || (app.pauseInactive && inactive))
    SetPropW(app.window, L"JfgFrontendPause", reinterpret_cast<HANDLE>(1));
  else
    RemovePropW(app.window, L"JfgFrontendPause");
}
LRESULT CALLBACK shieldProcedure(HWND window, UINT m, WPARAM w, LPARAM l) {
  if ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_KEYDOWN ||
      m == WM_KEYUP || m == WM_CHAR || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) {
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL &&
        m != WM_MOUSEHWHEEL) {
      POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
      MapWindowPoints(window, app.window, &p, 1);
      l = MAKELPARAM(p.x, p.y);
    }
    SendMessageW(app.window, m, w, l);
    return 0;
  }
  if (m == WM_ERASEBKGND)
    return 1;
  return DefWindowProcW(window, m, w, l);
}
#endif
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam,
                           LPARAM lparam) {
  if(message==WM_APP+22) {HWND tool=reinterpret_cast<HWND>(wparam);if(IsWindow(tool)&&GetPropW(tool,L"JfgLiveModal")&&std::find(app.liveModals.begin(),app.liveModals.end(),tool)==app.liveModals.end())app.liveModals.push_back(tool);return 0;}
  if(message==WM_APP+23) {PostMessageW(window,WM_COMMAND,Inventory,0);return 0;}
#ifdef JFG_RML_UI
  if (app.uiReady &&
      ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
       message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
       message == WM_SYSKEYDOWN || message == WM_SYSKEYUP || message == WM_ACTIVATEAPP ||
       message == WM_SETFOCUS || message == WM_KILLFOCUS)) {
    FrontendUiMessage(message, wparam, lparam);
    syncUiCapture();
    // Do not let DefWindowProc enter its system-menu loop for our menu keys.
    if ((message == WM_KEYDOWN || message == WM_KEYUP) && wparam == VK_F10) return 0;
    if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) &&
        (wparam == 'G' || wparam == 'C' || wparam == 'V' || wparam == 'A' || wparam == 'T' || wparam == 'H')) return 0;
  }
#endif
  switch (message) {
  case WM_CREATE: {
    app.window = window;
    SetPropW(window, L"JfgFrontend", reinterpret_cast<HANDLE>(1));
    app.dpi = GetDpiForWindow(window);
    fonts();
    menus();
    app.viewport =
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 1,
                        1, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    app.home.push_back(control(L"STATIC", L"JET FORCE GEMINI", 0, 0));
    SendMessageW(app.home.back(), WM_SETFONT,
                 reinterpret_cast<WPARAM>(app.titleFont), TRUE);
    app.home.push_back(
        control(L"STATIC",
                L"Your game. Your controllers. Ready to play.\nChoose a ROM "
                L"once; your settings and saves stay here.",
                0, 0));
    app.home.push_back(control(L"STATIC", L"GAME ROM", 0, 0));
    app.rom = control(L"EDIT", read(L"frontend-rom.txt").c_str(),
                      ES_AUTOHSCROLL | WS_TABSTOP, 0);
    app.home.push_back(app.rom);
    app.home.push_back(
        control(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, Rom));
    app.home.push_back(control(L"STATIC", L"LOCAL GAME BUILD", 0, 0));
    app.runtime = control(L"EDIT", read(L"frontend-runtime.txt").c_str(),
                          ES_AUTOHSCROLL | WS_TABSTOP, 0);
    app.home.push_back(app.runtime);
    app.home.push_back(
        control(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, Runtime));
    app.home.push_back(
        control(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, Play));
    app.home.push_back(control(L"BUTTON", L"Set up and build",
                               BS_PUSHBUTTON | WS_TABSTOP, Setup));
    app.status = control(L"STATIC", L"Preparing your profile...", SS_LEFT, 0);
    app.mods = read(L"frontend-mods.txt") == L"1";
    app.pauseInactive = read(L"frontend-pause-inactive.txt") == L"1";
    app.widescreen = readWidescreen();
    CheckMenuItem(app.menu, Mods,
                  MF_BYCOMMAND | (app.mods ? MF_CHECKED : MF_UNCHECKED));
#ifdef JFG_RML_UI
    ShowWindow(app.status, SW_HIDE);
    WNDCLASSW shieldClass{};
    shieldClass.lpfnWndProc = shieldProcedure;
    shieldClass.hInstance = GetModuleHandleW(nullptr);
    shieldClass.lpszClassName = L"JfgUiShield";
    shieldClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&shieldClass);
    app.shield =
        CreateWindowExW(0, shieldClass.lpszClassName, L"", WS_CHILD, 0, 0, 1, 1,
                        window, nullptr, shieldClass.hInstance, nullptr);
    if (!FrontendUiInit(window, app.profile, uiAction))
      return -1;
    app.uiReady = true;
    SetTimer(window, 2, 16, nullptr);
#endif
    launch(app.session, L"init", window);
    SetTimer(window, 1, 200, nullptr);
    showHome(true);
    return 0;
  }
  case WM_DPICHANGED: {
    app.dpi = HIWORD(wparam);
    fonts();
    const auto *suggested = reinterpret_cast<RECT *>(lparam);
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    return 0;
  }
  case WM_SIZE:
    layout();
    return 0;
  case WM_GETMINMAXINFO: {
    auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
    info->ptMinTrackSize = {scaled(700), scaled(540)};
    return 0;
  }
  case WM_CTLCOLORSTATIC: {
    HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, RGB(225, 234, 245));
    SetBkColor(dc, RGB(18, 27, 40));
    return reinterpret_cast<LRESULT>(app.background);
  }
  case WM_ERASEBKGND: {
    RECT r{};
    GetClientRect(window, &r);
    FillRect(reinterpret_cast<HDC>(wparam), &r, app.background);
    return 1;
  }
  case WM_ENTERMENULOOP:
    capture(true);
    return 0;
  case WM_EXITMENULOOP:
    if (!app.dialog.running())
      capture(false);
    return 0;
  case WM_KEYDOWN:
    if (wparam == VK_F11) {
      fullscreen();
      return 0;
    }
    if (wparam == VK_ESCAPE) {
      popup();
      return 0;
    }
    break;
#ifdef JFG_RML_UI
  case WM_HOTKEY:
    if (wparam == kMuteShortcut && GetForegroundWindow() == app.window) {
      FrontendUiToggleMute();
      return 0;
    }
    break;
#endif
  case kHotkey:
    if (wparam == VK_F11)
      fullscreen();
    else
      popup();
    return 0;
  case kReady: {
    HWND render = reinterpret_cast<HWND>(wparam);
    if (GetParent(render) != app.viewport)
      return 0;
    app.render = render;
    app.playing = true;
    showHome(false);
    if (app.stopRequested)
      stop();
    else
      SetFocus(render);
    return 0;
  }
  case WM_SETFOCUS:
    if (app.render && IsWindow(app.render) && !app.setting)
      SetFocus(app.render);
    return 0;
  case WM_TIMER: {
#ifdef JFG_RML_UI
    if (wparam == 2) {
      syncUiCapture();
      FrontendUiFrame({app.render && IsWindow(app.render),
                       app.session.running(), app.fullscreen, app.mods,
                       text(app.rom), text(app.runtime), app.lastStatus, app.pauseInactive,
                       app.widescreen});
      return 0;
    }
#endif
    std::erase_if(app.tools, [](const auto &tool) { return !tool->running(); });
    const auto messageText = read(L"frontend-status.txt");
    if (!messageText.empty() && messageText != app.lastStatus)
      status(messageText);
    if (app.stopRequested && app.session.running() && !app.render &&
        GetTickCount64() - app.startedAt > 15000) {
      // No render window means the guest has not entered play; cancel only this
      // launch tree.
      TerminateJobObject(app.session.job, 1);
      write(L"frontend-status.txt", L"Game startup cancelled. You can retry.");
    }
    if (app.session.handle && !app.session.running()) {
      DWORD exitCode = 1;
      GetExitCodeProcess(app.session.handle, &exitCode);
      const bool startAfterInit = app.initializing && app.quickPlay && exitCode == 0;
      app.quickPlay = false;
      app.session.release();
      app.render = nullptr;
      app.playing = false;
      app.manualPause = false;
      app.stopRequested = false;
      app.initializing = false;
      showHome(true);
      SetWindowTextW(app.rom, read(L"frontend-rom.txt").c_str());
      SetWindowTextW(app.runtime, read(L"frontend-runtime.txt").c_str());
      if (app.closing)
        DestroyWindow(window);
      else if (startAfterInit)
        PostMessageW(window, WM_COMMAND, Play, 0);
    }
    if (app.dialog.handle && !app.dialog.running()) {
      app.dialog.release();
      capture(false);
    }
    return 0;
  }
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case Rom:
      pick(true);
      break;
    case Runtime:
      pick(false);
      break;
    case Play:
    case Setup:
      if (app.session.running() || app.dialog.running())
        break;
      if (LOWORD(wparam) == Setup &&
          MessageBoxW(
              window,
              L"Setup downloads source and missing build tools. The first "
              L"setup may download several GB and require administrator "
              L"approval or a restart. Your ROM stays on this PC. Continue?",
              L"Set up game", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        break;
      persist();
      app.playing = LOWORD(wparam) == Play;
      app.manualPause = false;
      app.stopRequested = false;
      app.startedAt = GetTickCount64();
      if (launch(app.session, app.playing ? L"play" : L"setup",
                 app.playing ? app.viewport : window))
        status(app.playing ? L"Starting game..." : L"Starting setup...");
      else
        app.playing = false;
      showHome(true);
      break;
    case Stop:
      stop();
      break;
    case Cancel:
      if (app.session.running() && !app.playing &&
          MessageBoxW(
              window,
              L"Cancel setup? Completed downloads are retained for retry.",
              L"Cancel setup", MB_OKCANCEL) == IDOK) {
        TerminateJobObject(app.session.job, 1);
        status(L"Setup cancelled.");
        write(L"frontend-status.txt", L"Setup cancelled. You can retry.");
      }
      break;
    case Fullscreen:
      fullscreen();
      break;
    case Controllers:
      settings(L"controllers");
      break;
    case Audio:
      settings(L"audio");
      break;
    case Support:
      settings(L"support");
      break;
    case Map:
    case Inventory: {
      auto tool = std::make_unique<Process>();
      if (launch(*tool, LOWORD(wparam) == Map ? L"map" : L"inventory", window))
        app.tools.push_back(std::move(tool));
      break;
    }
    case Saves:
      ShellExecuteW(window, L"open", app.profile.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
      break;
    case Guide:
      ShellExecuteW(window, L"open",
                    L"https://github.com/TK22-26/JetForceGemini-Recomp/blob/"
                    L"main/docs/getting-started.md",
                    nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case Mods:
      if (!app.session.running()) {
        app.mods = !app.mods;
        CheckMenuItem(app.menu, Mods,
                      MF_BYCOMMAND | (app.mods ? MF_CHECKED : MF_UNCHECKED));
        persist();
      } else
        status(L"Stop the game before changing testing mods.");
      break;
    case Quit:
      PostMessageW(window, WM_CLOSE, 0, 0);
      break;
    }
    return 0;
  case WM_CLOSE:
    if (!app.initializing)
      persist();
    if (app.session.running()) {
      if (app.initializing) {
        app.closing = true;
      } else if (app.playing) {
        app.closing = true;
        stop();
      } else
        MessageBoxW(window, L"Finish or cancel setup before closing.",
                    L"Jet Force Gemini", MB_OK);
    } else
      DestroyWindow(window);
    return 0;
  case WM_DESTROY:
#ifdef JFG_RML_UI
    if (app.muteShortcutRegistered) UnregisterHotKey(window, kMuteShortcut);
#endif
#ifdef JFG_RML_UI
    KillTimer(window, 2);
    app.uiReady = false;
    FrontendUiShutdown();
#endif
    KillTimer(window, 1);
    RemovePropW(window, L"JfgFrontendPause");
    RemovePropW(window, L"JfgFrontend");
    RemovePropW(window, L"JfgSettingsOpen");
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
#ifdef JFG_RML_UI
  int liveResult=FrontendLiveToolEntry(instance,show);if(liveResult>=0)return liveResult;
#endif
  HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\JFGUnifiedFrontend");
  if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (singleton)
      CloseHandle(singleton);
    MessageBoxW(nullptr, L"The JFG frontend is already open.",
                L"Jet Force Gemini", MB_OK);
    return 0;
  }
  auto dpi = reinterpret_cast<BOOL(WINAPI *)(HANDLE)>(GetProcAddress(
      GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (dpi)
    dpi(reinterpret_cast<HANDLE>(-4)); // same DPI context as hosted renderer
  wchar_t module[32768]{};
  GetModuleFileNameW(nullptr, module, 32768);
#ifdef JFG_RML_UI
  SetEnvironmentVariableW(L"JFG_LAUNCHER_EXE",module);
#endif
  app.helper = fs::path(module).parent_path() / L"JFG-Setup.exe";
  PWSTR local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
    return 1;
  app.profile = fs::path(local) / L"JFGRecomp" / L"profiles" / L"default";
  CoTaskMemFree(local);
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool validArguments = argv != nullptr;
  for (int i = 1; i < argc && validArguments; ++i) {
    const std::wstring option = argv[i];
    if (option == L"--play" && !app.quickPlay)
      app.quickPlay = true;
    else if (option == L"--profile" && i + 1 < argc)
      app.profile = fs::absolute(fs::path(argv[++i]));
    else if (option == L"--input-replay" && i + 1 < argc && app.inputReplay.empty())
      app.inputReplay = fs::absolute(fs::path(argv[++i])).wstring();
    else if (option == L"--progress-output" && i + 1 < argc && app.progressOutput.empty())
      app.progressOutput = fs::absolute(fs::path(argv[++i])).wstring();
    else
      validArguments = false;
  }
  LocalFree(argv);
  if (!validArguments || (!app.quickPlay && (!app.inputReplay.empty() || !app.progressOutput.empty()))) {
    MessageBoxW(nullptr,
                L"Use JFG-Launcher.exe [--play] [--profile <folder>]. "
                L"Diagnostic --input-replay <file> and --progress-output <file> require --play.",
                L"Jet Force Gemini", MB_OK | MB_ICONERROR);
    return 2;
  }
  HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
  if (resource) {
    HGLOBAL loaded = LoadResource(instance, resource);
    DWORD size = SizeofResource(instance, resource);
    const auto *bytes = static_cast<const char *>(LockResource(loaded));
    if (!bytes || size == 0 || size > 4 * 1024 * 1024)
      return 1;
    std::uint64_t hash = 1469598103934665603ULL;
    for (DWORD i = 0; i < size; ++i) {
      hash ^= static_cast<unsigned char>(bytes[i]);
      hash *= 1099511628211ULL;
    }
    fs::path helperFolder =
        app.profile / L"frontend-helper" / std::to_wstring(hash);
    std::error_code helperError;
    fs::create_directories(helperFolder, helperError);
    if (helperError)
      return 1;
    app.helper = helperFolder / L"JFG-Setup.exe";
    std::ifstream existing(app.helper, std::ios::binary | std::ios::ate);
    std::string previous;
    if (existing && existing.tellg() == static_cast<std::streamoff>(size)) {
      existing.seekg(0);
      previous.assign(std::istreambuf_iterator<char>(existing), {});
    }
    existing.close();
    if (previous.size() != size ||
        !std::equal(previous.begin(), previous.end(), bytes)) {
      const fs::path tmp = helperFolder / L"JFG-Setup.tmp";
      std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
      output.write(bytes, size);
      output.close();
      if (!output ||
          !MoveFileExW(tmp.c_str(), app.helper.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 1;
    }
  }
  std::error_code error;
  fs::create_directories(app.profile, error);
  if (error) {
    MessageBoxW(nullptr, L"Cannot open the profile directory.",
                L"Jet Force Gemini", MB_OK | MB_ICONERROR);
    return 1;
  }
  WNDCLASSW wc{};
  wc.lpfnWndProc = procedure;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"JfgFrontend";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = app.background;
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(
      0, wc.lpszClassName, L"Jet Force Gemini",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      MulDiv(1280, static_cast<int>(GetDpiForSystem()), 96),
      MulDiv(760, static_cast<int>(GetDpiForSystem()), 96), nullptr, nullptr,
      instance, nullptr);
  if (!window)
    return 1;
  int x = 0, y = 0, w = 0, h = 0, full = 0;
  if (swscanf_s(read(L"frontend-window.txt").c_str(), L"%d %d %d %d %d", &x, &y,
                &w, &h, &full) == 5 &&
      w >= 700 && h >= 540) {
    RECT r{x, y, x + w, y + h};
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL))
      SetWindowPos(window, nullptr, x, y, w, h, SWP_NOZORDER);
  }
  ShowWindow(window, show);
  UpdateWindow(window);
  if (full)
    fullscreen();
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_F11) {
      fullscreen();
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
      popup();
      continue;
    }
#ifdef JFG_RML_UI
    {
#else
    if (!IsDialogMessageW(window, &msg)) {
#endif
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  app.dialog.release();
  app.session.release();
  DeleteObject(app.font);
  DeleteObject(app.titleFont);
  DeleteObject(app.background);
  DestroyMenu(app.menu);
  ReleaseMutex(singleton);
  CloseHandle(singleton);
  return 0;
}
