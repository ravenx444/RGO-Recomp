// Native RmlUi design review harness. Game/process integration remains
// separate.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi_Backend.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <commdlg.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <string>
#include <windows.h>
#include <xinput.h>
namespace fs = std::filesystem;
static std::string u8(const std::wstring &s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                              nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}
static std::string escape(std::string s) {
  std::string out;
  for (char c : s) {
    if (c == '&')
      out += "&amp;";
    else if (c == '<')
      out += "&lt;";
    else if (c == '>')
      out += "&gt;";
    else if (c == '"')
      out += "&quot;";
    else
      out += c;
  }
  return out;
}
struct LogSystem : Rml::SystemInterface {
  Rml::SystemInterface *platform;
  std::ofstream file;
  explicit LogSystem(Rml::SystemInterface *p)
      : platform(p), file("rmlui-preview.log", std::ios::trunc) {}
  double GetElapsedTime() override { return platform->GetElapsedTime(); }
  void SetMouseCursor(const Rml::String &s) override {
    platform->SetMouseCursor(s);
  }
  void SetClipboardText(const Rml::String &s) override {
    platform->SetClipboardText(s);
  }
  void GetClipboardText(Rml::String &s) override {
    platform->GetClipboardText(s);
  }
  bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
    file << int(type) << ": " << message << "\n";
    file.flush();
    return true;
  }
};
struct Ui : Rml::EventListener {
  Rml::ElementDocument *doc = nullptr;
  fs::path profile;
  int volume = 100, player = 0;
  bool muted = false;
  double toastUntil = 0;
  std::array<bool, 4> connected{};
  std::array<WORD, 4> buttons{};
  std::array<bool, 4> activated{};
  std::deque<std::string> notices;
  bool reducedMotion = false;
  std::string current;
  Rml::Element *el(const char *id) { return doc->GetElementById(id); }
  void label(const char *id, const std::string &s) {
    el(id)->SetInnerRML(escape(s));
  }
  std::string read(const char *file) {
    std::ifstream f(profile / file, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  void status(const std::string &s) { label("status", s); }
  void toast(const std::string &s) {
    if (toastUntil > Rml::GetSystemInterface()->GetElapsedTime()) {
      if (notices.empty() || notices.back() != s)
        notices.push_back(s);
      return;
    }
    label("toast-text", s);
    el("toast")->SetClass("visible", false);
    doc->GetContext()->Update();
    el("toast")->SetClass("visible", true);
    toastUntil = Rml::GetSystemInterface()->GetElapsedTime() + 4.62;
  }
  void ports() {
    for (DWORD i = 0; i < 4; i++) {
      XINPUT_STATE s{};
      bool on = XInputGetState(i, &s) == ERROR_SUCCESS;
      const WORD pressed =
          on ? static_cast<WORD>(s.Gamepad.wButtons & ~buttons[i]) : 0;
      if (on != connected[i]) {
        connected[i] = on;
        activated[i] = false;
        el(("p" + std::to_string(i + 1)).c_str())->SetClass("connected", on);
        toast("Controller " + std::to_string(i + 1) +
              (on ? " connected" : " disconnected"));
      } else if (pressed && !activated[i]) {
        activated[i] = true;
        toast("Controller " + std::to_string(i + 1) + " active");
      }
      buttons[i] = on ? s.Gamepad.wButtons : 0;
    }
    if (toastUntil) {
      double elapsed = Rml::GetSystemInterface()->GetElapsedTime() -
                       (toastUntil - 4.62),
             y = 0, opacity = 1;
      if (elapsed < 0.4) {
        double t = std::clamp(elapsed / 0.4, 0.0, 1.0), q = t - 1;
        double eased = 1 + 2.70158 * q * q * q + 1.70158 * q * q;
        y = 96 * (1 - eased);
        opacity = t;
      } else if (elapsed > 4.4) {
        double t = std::clamp((elapsed - 4.4) / 0.22, 0.0, 1.0);
        y = 120 * t * t;
        opacity = 1 - t;
      }
      el("toast")->SetProperty(
          "transform",
          "translate(-50%, " + std::to_string(reducedMotion ? 0 : y) + "dp)");
      el("toast")->SetProperty("opacity", std::to_string(opacity));
    }
    if (toastUntil &&
        Rml::GetSystemInterface()->GetElapsedTime() > toastUntil) {
      el("toast")->SetClass("visible", false);
      toastUntil = 0;
      if (!notices.empty()) {
        auto n = notices.front();
        notices.pop_front();
        toast(n);
      }
    }
  }
  void loadAudio() {
    auto text = read("audio.ini");
    int v = 100, m = 0;
    if (sscanf_s(text.c_str(), "version=1\nvolume=%d\nmuted=%d", &v, &m) == 2 &&
        v >= 0 && v <= 100 && (m == 0 || m == 1)) {
      volume = v;
      muted = m != 0;
    }
  }
  void saveAudio() {
    std::error_code ec;
    fs::create_directories(profile, ec);
    if (ec) {
      status("Cannot save audio settings.");
      return;
    }
    const auto tmp = profile / "audio.ini.ui.tmp", dest = profile / "audio.ini";
    {
      std::ofstream f(tmp, std::ios::binary);
      f << "version=1\nvolume=" << volume << "\nmuted=" << (muted ? 1 : 0)
        << "\n";
      f.flush();
      if (!f) {
        status("Cannot save audio settings.");
        return;
      }
    }
    if (!MoveFileExW(tmp.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      status("Cannot save audio settings.");
  }
  std::string button(const std::string &action, const std::string &text,
                     bool disabled = false) {
    return "<button data-action=\"" + action + "\"" +
           (disabled ? " disabled=\"disabled\"" : "") + ">" + escape(text) +
           "</button>";
  }
  void modal(const std::string &page) {
    current = page;
    el("modal")->SetProperty("display", "block");
    std::string html,
        note = "Design preview. Game integration is still in progress.";
    label("dialog-title", page == "controllers" ? "CONTROLLER MAPPING"
                          : page == "setup"     ? "SET UP AND BUILD"
                          : page == "support"   ? "SUPPORT REPORT"
                          : page == "video"     ? "VIDEO"
                          : page == "audio"     ? "AUDIO"
                          : page == "game"      ? "GAME"
                                                : "JFG RECOMP");
    el("dialog")->SetProperty("width", page == "audio" ? "480dp" : "760dp");
    if (page == "audio") {
      html = "<div class=\"row\"><div class=\"grow\">Master volume<div "
             "class=\"muted\">Music and effects</div></div><span "
             "class=\"title-font\" style=\"font-size:36dp;color:#ffb23e\">" +
             (muted ? std::string("MUTED") : std::to_string(volume) + "%") +
             "</span></div>";
      html += "<div class=\"row\">" + button("down", "-") +
              "<div class=\"grow\"><div "
              "style=\"height:6dp;background:#222b40\"><div "
              "style=\"height:6dp;background:#ffb23e;width:" +
              std::to_string(volume) + "%\"/></div></div>" + button("up", "+") +
              "</div>";
      html += "<div class=\"row\"><div class=\"grow\">Mute<div "
              "class=\"muted\">Volume is remembered when muted.</div></div>" +
              button("mute", muted ? "ON" : "OFF") + "</div>";
      note = "Applied immediately to the selected preview profile.";
    } else if (page == "video") {
      for (const auto &name : {"Window mode", "Resolution", "Aspect ratio",
                               "Frame rate", "Anti-aliasing", "VSync"}) {
        html += "<div class=\"row\"><span class=\"grow\">" + std::string(name) +
                "</span>";
        if (std::string(name) == "Window mode")
          html += button("fullscreen", "Toggle fullscreen");
        else
          html += button("unavailable", "Original / runtime default", true);
        html += "</div>";
      }
      note = "Renderer controls remain disabled until connected and validated.";
    } else if (page == "controllers") {
      html = "<div class=\"tabs\">";
      for (int i = 0; i < 4; i++)
        html += button("player" + std::to_string(i),
                       "PLAYER " + std::to_string(i + 1) +
                           (connected[i] ? " - connected" : " - no pad"));
      html += "</div><div class=\"row\"><span "
              "class=\"grow\">Device</span>Controller " +
              std::to_string(player + 1) + "</div>";
      const char *names[] = {
          "A",        "B",          "Z / fire",   "R",          "L",
          "Start",    "C up",       "C down",     "C left",     "C right",
          "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
      for (const char *name : names)
        html += "<div class=\"row\"><span class=\"grow\">" + std::string(name) +
                "</span>" + button("learn", "Learn", true) + "</div>";
      note = "Live physical detection; mapping editing is not connected in "
             "this preview.";
    } else if (page == "setup") {
      html = "<div class=\"tabs\"><button>1  ROM</button><button>2  "
             "Verify</button><button>3  Build</button><button>4  "
             "Play</button></div><div class=\"name\">Ready to set up your "
             "game</div><p class=\"muted\">The game build is managed "
             "automatically. Select a ROM on the main screen.</p><div "
             "class=\"row\">Build progress and logs appear here while setup "
             "runs.</div>";
      html += button("setup-start", "Start setup", true);
      note = "Setup service integration is pending. No downloads are running.";
    } else if (page == "support") {
      html = "<p>Which session had the problem?</p><div class=\"row\">Session "
             "history will be read from your profile.</div><div "
             "class=\"label\">THE ZIP INCLUDES</div><p>Game and launcher "
             "logs<br/>Settings and mapping<br/>System and GPU info</p><p "
             "class=\"muted\">No ROM or save data. Review it, then attach it "
             "to your GitHub issue.</p>" +
             button("zip", "Create ZIP", true) +
             button("freeze", "Capture freeze", true);
    } else if (page == "game") {
      html = "<div class=\"row\">" + button("play", "Play") +
             button("setup", "Set up and build") + button("rom", "Change ROM") +
             "</div><p class=\"muted\">This is the native UI review build. "
             "Game start/stop will be integrated after the design surface.</p>";
    } else {
      html = "<div class=\"name\">RGO</div><p>Native RmlUi "
             "/ C++ interface preview</p><p class=\"muted\">Based on the eight "
             "owner-provided UI exports. Controller notifications follow "
             "device events. No browser or JavaScript engine is used.</p>";
    }
    el("dialog-body")->SetInnerRML(html);
    label("dialog-note", note);
  }
  void rom() {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrFilter = L"N64 ROM\0*.z64;*.n64;*.v64\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
      fs::path p(path);
      label("rom-name", u8(p.filename().wstring()));
      label("rom-folder", u8(p.parent_path().wstring()));
      label("rom-badge", "SELECTED");
      status("ROM selected. Verification is performed by setup.");
    }
  }
  void ProcessEvent(Rml::Event &event) override {
    auto *t = event.GetTargetElement();
    while (t && !t->HasAttribute("data-action"))
      t = t->GetParentNode();
    if (!t)
      return;
    auto a = t->GetAttribute<Rml::String>("data-action", "");
    if (a == "close") {
      current.clear();
      el("modal")->SetProperty("display", "none");
    } else if (a == "rom")
      rom();
    else if (a == "play") {
      status("UI preview: game launch is not connected yet.");
      for (int i = 0; i < 4; i++)
        if (connected[i]) {
          toast("Controller " + std::to_string(i + 1) + " connected");
          break;
        }
    } else if (a == "mute" || a == "up" || a == "down") {
      if (a == "mute")
        muted = !muted;
      else
        volume = std::clamp(volume + (a == "up" ? 5 : -5), 0, 100);
      saveAudio();
      modal("audio");
    } else if (a.rfind("player", 0) == 0) {
      player = std::clamp(a.back() - '0', 0, 3);
      modal("controllers");
    } else if (a == "fullscreen") {
      HWND w = GetActiveWindow();
      static WINDOWPLACEMENT place{sizeof(place)};
      static bool full = false;
      if (!full) {
        GetWindowPlacement(w, &place);
        MONITORINFO m{sizeof(m)};
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &m);
        SetWindowLongPtrW(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(w, nullptr, m.rcMonitor.left, m.rcMonitor.top,
                     m.rcMonitor.right - m.rcMonitor.left,
                     m.rcMonitor.bottom - m.rcMonitor.top, SWP_FRAMECHANGED);
        full = true;
      } else {
        SetWindowLongPtrW(w, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(w, &place);
        SetWindowPos(w, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        full = false;
      }
    } else
      modal(a);
  }
};
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t exe[32768];
  GetModuleFileNameW(nullptr, exe, 32768);
  fs::current_path(fs::path(exe).parent_path());
  Ui ui;
  ui.profile = fs::temp_directory_path() / "JFG-RmlUi-Preview";
  int argc = 0;
  auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i + 1 < argc; i++)
    if (std::wstring(argv[i]) == L"--profile")
      ui.profile = argv[++i];
  LocalFree(argv);
  if (!Backend::Initialize("RGO - RmlUi preview", 1280, 720, true))
    return 1;
  LogSystem system(Backend::GetSystemInterface());
  Rml::SetSystemInterface(&system);
  Rml::SetRenderInterface(Backend::GetRenderInterface());
  if (!Rml::Initialise()) {
    Backend::Shutdown();
    return 2;
  }
  Rml::LoadFontFace("ui/fonts/Barlow-Regular.ttf");
  Rml::LoadFontFace("ui/fonts/Barlow-SemiBold.ttf");
  Rml::LoadFontFace("ui/fonts/ChakraPetch-Bold.ttf");
  auto *context = Rml::CreateContext("launcher", {1280, 720});
  ui.doc = context->LoadDocument("ui/main.rml");
  if (!ui.doc) {
    Rml::Shutdown();
    Backend::Shutdown();
    return 3;
  }
  ui.doc->AddEventListener("click", &ui);
  ui.doc->Show();
  ui.loadAudio();
  BOOL animations = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
  ui.reducedMotion = !animations;
  ui.doc->SetClass("reduced-motion", ui.reducedMotion);
  while (Backend::ProcessEvents(context)) {
    ui.ports();
    context->Update();
    Backend::BeginFrame();
    context->Render();
    Backend::PresentFrame();
  }
  ui.doc->RemoveEventListener("click", &ui);
  Rml::Shutdown();
  Backend::Shutdown();
  return 0;
}
