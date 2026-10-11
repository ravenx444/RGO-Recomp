// Native launcher presentation and live settings over the game viewport.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "native_ui.hpp"
#include "frontend_build_info.hpp"
#include "home_scene.hpp"
#include "live_tools.hpp"
#include <iomanip>
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi_Platform_Win32.h>
#include <RmlUi_Renderer_DX11.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <commdlg.h>
#include <dcomp.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <jfg/audio/master_volume.hpp>
#include <jfg/runtime/controller_ports.hpp>
#include <jfg/runtime/gamepad.hpp>
#include <jfg/platform/sdl_gamepads.hpp>
#include <jfg/runtime/pc_input.hpp>
#include <jfg/runtime/pc_twin_stick.hpp>
#include <jfg/runtime/game_actions.hpp>
#include <jfg/renderer/widescreen.hpp>
#include <jfg/rgo/persistent_gore.hpp>
#include <memory>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
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
  explicit LogSystem(Rml::SystemInterface *p) : platform(p), file() {}
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
  bool building = false, controllerWindow = false;
  HWND window = nullptr;
  std::function<void(const std::string &)> action;
  FrontendUiState state;
  // RGO display settings: text typed into the custom size fields, kept while
  // the page is rebuilt, and whether the custom output row is shown.
  std::string outputWidth, outputHeight, renderWidth, renderHeight;
  bool customOutput = false;
  jfg::ControllerPorts assignments;
  std::array<jfg::ControllerMapping, 4> mappings{};
  std::array<jfg::PcInputConfig,4> pcInput{};
  std::array<int, 4> devices{-2, -2, -2, -2};
  int learning = -1, keyLearning = -1;
  bool keyReleased = false, devicePicker = false, controllerSettings = false;
  int pickerType = 0, pickerDevice = -2;
  double keyDeadline = 0;
  // RGO: gamepads come from SDL, as in the game; XInput is the fallback.
  jfg::SdlGamepads gamepads;
  bool gamepadsTried = false;
  jfg::GamepadSnapshot pads;
  std::array<std::string, 4> padNames;
  static jfg::StandardControllerSample fromXInput(const XINPUT_STATE &s) {
    constexpr WORD masks[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x20,
                              0,      0x10,   0x40,   0x80,   0x100,
                              0x200,  1,      2,      4,      8};
    jfg::StandardControllerSample out;
    for (int i = 0; i < 15; i++)
      out.buttons[static_cast<std::size_t>(i)] = masks[i] != 0 && (s.Gamepad.wButtons & masks[i]) != 0;
    out.axes = {s.Gamepad.sThumbLX, -int(s.Gamepad.sThumbLY), s.Gamepad.sThumbRX,
                -int(s.Gamepad.sThumbRY), s.Gamepad.bLeftTrigger * 32767 / 255,
                s.Gamepad.bRightTrigger * 32767 / 255};
    return out;
  }
  void pollPads() {
    if (!gamepadsTried) {
      gamepadsTried = true;
      gamepads.start();
    }
    if (gamepads.ready()) {
      pads = gamepads.snapshot();
    } else {
      for (DWORD d = 0; d < 4; ++d) {
        XINPUT_STATE s{};
        auto &info = pads.info[d];
        info = {};
        info.connected = XInputGetState(d, &s) == ERROR_SUCCESS;
        pads.pads[d] = info.connected ? fromXInput(s) : jfg::StandardControllerSample{};
      }
    }
    for (std::size_t d = 0; d < 4; ++d)
      if (pads.info[d].connected && !pads.info[d].name.empty())
        padNames[d] = pads.info[d].name;
  }
  static std::uint32_t padButtons(const jfg::StandardControllerSample &s) {
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < s.buttons.size(); ++i)
      if (s.buttons[i]) mask |= 1U << i;
    return mask;
  }
  std::string padLabel(int d) const {
    std::string text = "Controller " + std::to_string(d + 1);
    if (d >= 0 && d < 4 && !padNames[static_cast<std::size_t>(d)].empty())
      text += " (" + padNames[static_cast<std::size_t>(d)] + ")";
    return text;
  }
  jfg::GamepadFamily padFamily() const {
    const int d = devices[player];
    return d >= 0 && d < 4 ? pads.info[static_cast<std::size_t>(d)].family : jfg::GamepadFamily::generic;
  }
  void lightPads() {
    if (!gamepads.ready()) return;
    for (int p = 0; p < 4; ++p)
      if (devices[p] >= 0) gamepads.light(devices[p], jfg::player_light(p));
  }
  bool learnReleased = false;
  double learnDeadline = 0;
  void close() {
    if(controllerWindow){PostMessageW(window,WM_CLOSE,0,0);return;}
    current.clear();
    doc->SetClass("panel-open", false);
    doc->SetClass("menu-open", false);
    for (const auto *name :
         {"game", "controllers", "video", "audio", "tools", "help"})
      el((std::string("menu-") + name).c_str())->SetClass("selected", false);
    learning = -1;
    el("modal")->SetProperty("display", "none");
    el("game-popup")->SetProperty("display", "none");
  }
  bool writeFile(const std::string &name, const std::string &value) {
    auto dest = profile / name, tmp = profile / (name + ".ui.tmp");
    {
      std::ofstream f(tmp, std::ios::binary);
      f << value;
      f.flush();
      if (!f) {
        status("Cannot save settings.");
        return false;
      }
    }
    if (!MoveFileExW(tmp.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      status("Cannot save settings.");
      return false;
    }
    return true;
  }
  bool pcExperimental() {
    return jfg::pc_experiments_enabled(read("experimental-controls.ini"));
  }
  bool pcKeyboard() const { return mappings[player].device==-3 || (mappings[player].device==-1 && devices[player]==-3); }
  std::string pcExperimentalHtml() {
    const bool keyboard=pcKeyboard();
    return "<div class=\"row\"><div class=\"grow\">"+
      std::string(keyboard?"Experimental mouse look and movement":"Experimental dual-stick controls")+
      "<div class=\"muted\">Opt in to modern controls. This switch applies to the whole profile.</div></div>"+
      button("pc-experimental",pcExperimental()?"On":"Off")+"</div>";
  }
  // RGO gyro aim, on the device card of a player whose controller has a gyro.
  std::string gyroHtml(const jfg::PcInputConfig &c) {
    std::string html="<div class=\"device-row\"><div class=\"grow\">Gyro aim<div class=\"muted\">Turn the controller to aim.</div></div>"+
      button("gyro",c.gyro?"On":"Off")+"</div>";
    if(!c.gyro)return html;
    const int d=devices[player];
    const bool calibrated=d>=0 && d<4 && pads.info[static_cast<std::size_t>(d)].gyro_calibrated;
    html+="<div class=\"device-row\"><span>Gyro sensitivity (%)</span><input class=\"volume\" type=\"range\" min=\"10\" max=\"1000\" step=\"10\" value=\""+
      std::to_string(c.gyro_sensitivity)+"\" data-action=\"gyro-tune\"/><span id=\"gyro-value\">"+std::to_string(c.gyro_sensitivity)+"</span></div>";
    html+="<div class=\"device-row\">"+button("gyro-mode",c.gyro_mode?"Active: always":"Active: while aiming")+
      button("gyro-invert",c.gyro_invert_y?"Gyro Y: inverted":"Gyro Y: normal")+"</div>";
    html+="<div class=\"device-row\"><span>Ratchet (hold to pause gyro)</span><select id=\"gyro-ratchet\" data-action=\"gyro-ratchet\">";
    for(int b=-1;b<jfg::kBindingCount;++b)
      html+="<option value=\""+std::to_string(b)+"\""+(b==c.gyro_ratchet?" selected=\"selected\"":"")+">"+escape(bindingName(b))+"</option>";
    html+="</select></div><p class=\"muted\">"+std::string(calibrated?"Calibrated.":"Put the controller down for a second to calibrate.")+
      " 100% turns the aim as far as you turn the controller.</p>";
    return html;
  }
  // RGO: binding labels name each button's action for the game's own Normal
  // or Expert control option (game_actions.hpp), which the player picks here.
  std::string gameControlsHtml(const jfg::PcInputConfig &c) {
    return "<div class=\"row\"><span>Labels for the game's</span>"+
      button("game-controls",c.game_controls?"Expert controls":"Normal controls")+
      "<span class=\"muted\">Match Options > Controls in the game. Rebinding still sets the N64 button in brackets.</span></div>";
  }
  // RGO controller scheme: Original (one stick moves and aims, as on the
  // N64) or Modern twin-stick. Shown for every controller player, with no
  // Experimental switch needed.
  std::string twinStickHtml() {
    const auto &c=pcInput[static_cast<std::size_t>(player)];
    const auto &m=mappings[player];
    const bool twin=jfg::pc_twin_stick(c);
    std::string html="<div class=\"row\"><div class=\"grow\">Controller scheme<div class=\"muted\">"+
      std::string(twin?"Modern twin-stick: the left stick moves (and strafes while aiming), the right stick turns the camera and aims.":
        "Original: one stick moves and aims, as on the N64.")+" Switching sets the matching button layout.</div></div>"+
      button("twin-scheme",twin?"Modern twin-stick":"Original")+"</div>";
    if(!twin)return html;
    const auto name=[&](int i){return escape(bindingName(m.bindings[static_cast<std::size_t>(i)]));};
    html+="<p class=\"muted\">Aim: "+name(9)+". Fire: "+name(2)+". Jump: "+name(c.game_controls?0:10)+". "
      "Movement: "+std::string(m.stick?"right":"left")+" stick; the other stick looks. Rebind below.</p>";
    html+="<div class=\"row\"><span>Button layout for the game's</span>"+button("twin-layout-normal","Normal controls")+
      button("twin-layout-expert","Expert controls")+"</div>";
    const char* labels[]={"Camera speed (%)","Aim speed (%)","Right stick deadzone","Vertical speed (%)"};
    const int index[]={4,1,2,6};
    const int values[]={c.camera_sensitivity,c.aim_sensitivity,c.aim_deadzone,c.stick_vertical_sensitivity};
    for(int i=0;i<4;++i)
      html+="<div class=\"row\"><span>"+std::string(labels[i])+"</span><input class=\"volume\" type=\"range\" min=\""+(index[i]==2?"0":"10")+
        "\" max=\""+(index[i]==2?"30000":"300")+"\" value=\""+std::to_string(values[i])+"\" data-action=\"twin-tune"+std::to_string(index[i])+
        "\"/><span id=\"pc-value"+std::to_string(index[i])+"\">"+std::to_string(values[i])+"</span></div>";
    html+="<div class=\"row\"><span>Stick response</span>"+button("twin-curve",c.aim_curve==0?"Linear":c.aim_curve==1?"Precise":"Extra precise")+
      "<span class=\"muted\">Precise slows small movements near the centre.</span></div>";
    html+="<div class=\"row\">"+button("twin-invert-camera",c.camera_invert_y?"Camera Y: inverted":"Camera Y: normal")+
      button("twin-invert-aim",c.aim_invert_y?"Aim Y: inverted":"Aim Y: normal")+"</div>";
    html+="<p class=\"muted\">100% turns the camera 200 degrees and the aim 100 degrees a second at full stick. Gyro aim adds to the right stick. "
      "Swimming, jetpack, vehicles and split-screen keep the game's own camera.</p>";
    return html;
  }
  std::string pcFile() const { return player ? "pc-input-"+std::to_string(player+1)+".ini" : "pc-input.ini"; }
  void loadPcInput() {
    pcInput[static_cast<std::size_t>(player)]={};
    (void)jfg::parse_pc_input(read(pcFile().c_str()),pcInput[static_cast<std::size_t>(player)]);
  }
  void savePcInput() {
    if(writeFile(pcFile(),jfg::serialize_pc_input(pcInput[static_cast<std::size_t>(player)])))
      label("dialog-note","Saved. Release controls before returning to the game.");
  }
  static std::string pcKeyName(int key) {
    switch(key) {
      case 0:return "Unbound";case 1:return "Mouse left";case 2:return "Mouse right";
      case 4:return "Mouse middle";case 5:return "Mouse side 1";case 6:return "Mouse side 2";
      case 256:return "Wheel up";case 257:return "Wheel down";
    }
    wchar_t text[64]{};
    auto scan=MapVirtualKeyW(static_cast<UINT>(key),MAPVK_VK_TO_VSC);
    if(key>=VK_PRIOR && key<=VK_DELETE)scan|=0x100;
    if(GetKeyNameTextW(static_cast<LONG>(scan<<16),text,64)>0)return u8(text);
    return "Key "+std::to_string(key);
  }
  // RGO: the same Controls scheme for the mouse and keyboard player. Modern
  // turns on mouse look and aim with no Experimental switch.
  std::string keyboardSchemeHtml(const jfg::PcInputConfig &c) {
    const bool twin=jfg::pc_twin_stick(c);
    return "<div class=\"row\"><div class=\"grow\">Controls scheme<div class=\"muted\">"+
      std::string(twin?"Modern: the move keys only move (and walk and strafe while aiming); the mouse turns the camera, and aims while you hold the aim button.":
        "Original: as on the N64. Mouse look and aim need Modern (or the Experimental switch below).")+"</div></div>"+
      button("twin-scheme",twin?"Modern":"Original")+"</div>";
  }
  std::string pcInputHtml() {
    auto &c=pcInput[static_cast<std::size_t>(player)];
    const bool keyboard=pcKeyboard(), enabled=pcExperimental();
    const bool twin=keyboard && jfg::pc_twin_stick(c);
    std::string html=keyboard?keyboardSchemeHtml(c):std::string{};
    if(!twin)html+=pcExperimentalHtml();
    if(!keyboard && !enabled)return html;
    html+="<div class=\"row\">"+
      button(keyboard?"pc-normal":"pc-pad-normal","Normal preset",!keyboard && !enabled)+
      button(keyboard?"pc-expert":"pc-pad-expert","Expert preset",!keyboard && !enabled)+
      button("pc-reset",keyboard?"Reset keyboard and mouse":"Reset aim settings",!keyboard && !enabled)+
      (keyboard && !twin?button("controller-settings",controllerSettings?"Hide settings":"Settings",!enabled):std::string{})+"</div>";
    if(twin)html+="<p class=\"muted\">Match the preset to the game's Normal or Expert control scheme. Aim: "+escape(pcKeyName(c.keys[9]))+
      ". Fire: "+escape(pcKeyName(c.keys[2]))+".</p>";
    if(twin || (enabled && controllerSettings)) {
      if(!twin)html+="<div class=\"row\">"+
        button(keyboard?"pc-mouse":"pc-dual",keyboard?(c.mouse_aim?"Mouse look and aim: on":"Mouse look and aim: off"):(c.dual_stick?"Dual-stick aim: on":"Dual-stick aim: off"))+
        button("pc-modern",c.modern?"Modern camera + movement: on":"Modern camera + movement: off")+"</div>";
      if(!twin)html+="<p class=\"muted\">Match the preset to the game's Normal or Expert control scheme. Modern camera and aim movement currently support single-player.</p>";
      const char* labels[]={"Mouse look sensitivity (camera)","Aim stick sensitivity","Aim stick deadzone","Mouse aim sensitivity","Camera stick sensitivity","Mouse vertical scale","Stick vertical scale","Aim key speed (degrees/s)"};
      const int values[]={c.mouse_sensitivity,c.aim_sensitivity,c.aim_deadzone,c.mouse_aim_sensitivity,c.camera_sensitivity,c.mouse_vertical_sensitivity,c.stick_vertical_sensitivity,c.key_aim_speed};
      const int maxima[]={500,300,30000,500,300,300,300,360};
      for(int i=0;i<8;++i) {
        if(keyboard!=(i==0 || i==3 || i==5 || i==7))continue;
        html+="<div class=\"row\"><span>"+std::string(labels[i])+"</span><input class=\"volume\" type=\"range\" min=\""+std::to_string(i==2?0:i==7?30:10)+"\" max=\""+std::to_string(maxima[i])+"\" value=\""+std::to_string(values[i])+"\" data-action=\"pc-tune"+std::to_string(i)+"\"/><span id=\"pc-value"+std::to_string(i)+"\">"+std::to_string(values[i])+"</span></div>";
      }
      if(keyboard)html+="<div class=\"row\"><span>Aim button</span>"+button("pc-aim-toggle",c.aim_toggle?"Toggle":"Hold")+
        "<span class=\"muted\">Toggle: press R / aim once to start aiming, again to stop.</span></div>";
      if(!keyboard)html+="<div class=\"row\"><span>Stick response</span>"+button("pc-curve",c.aim_curve==0?"Linear":c.aim_curve==1?"Precise":"Extra precise")+"<span class=\"muted\">Precise response slows small movements near the center.</span></div>";
      html+="<div class=\"row\">"+button(keyboard?"pc-invert-mouse":"pc-invert-stick",keyboard?(c.mouse_invert_y?"Mouse Y: inverted":"Mouse Y: normal"):(c.aim_invert_y?"Aim stick Y: inverted":"Aim stick Y: normal"))+"</div>";
    }
    if(!keyboard)return html;
    html+="<div class=\"label\">KEYBOARD AND MOUSE BINDINGS</div>"+gameControlsHtml(c);
    if(!enabled && !twin)html+="<p class=\"muted\">Bindings are always active. Mouse look and aim need the Modern scheme above (or the Experimental switch).</p>";
    const auto &shown=c;
    const char* names[]={"A / confirm","B / weapon","Z / fire","Start","D-pad up","D-pad down","D-pad left","D-pad right","L","R / aim","C up / Normal jump","C down","C left","C right","Move forward","Move back","Move left","Move right","Full movement","Alternate A","Aim up","Aim down","Aim left","Aim right"};
    for(std::size_t i=0;i<shown.keys.size();++i) {
      // RGO: N64 buttons are named by their action (game_actions.hpp).
      const std::string label=i<jfg::kN64ButtonNames.size()?jfg::game_action_label(static_cast<int>(i),c.game_controls!=0):std::string(names[i]);
      html+="<div class=\"key-binding-row\"><span>"+escape(label)+"</span>"+
        button("pc-bind"+std::to_string(i),pcKeyName(shown.keys[i]))+
        button("pc-unbind"+std::to_string(i),"Clear")+"</div>";
    }
    return html;
  }
  std::string controllerFile(int p) {
    return p ? "controller-" + std::to_string(p + 1) + ".ini"
             : "controller.ini";
  }
  void loadControllers() {
    for (int p = 0; p < 4; p++) {
      jfg::parse_controller_mapping(read(controllerFile(p).c_str()),
                                    mappings[p]);
      assignments.configure(p, mappings[p]);
    }
    assignments.pins=jfg::parse_controller_pins(read("controller-ids.ini"));
    resolve();
  }
  // RGO: players resolve to controllers by identity first (controller_ports.hpp).
  void resolve() {
    std::array<std::string,4> ids;
    for(std::size_t d=0;d<4;++d)if(connected[d])ids[d]=pads.info[d].id;
    devices=assignments.devices(connected,ids);
  }
  bool saveController(int p = -1) {
    if(p<0)p=player;
    auto &m = mappings[p];
    std::ostringstream s;
    s << "version=1\ndevice=" << m.device << "\nstick=" << m.stick
      << "\ndeadzone=" << m.deadzone << "\nthreshold=" << m.threshold
      << "\ntrigger=" << m.trigger << "\ninvert_x=" << m.invert_x
      << "\ninvert_y=" << m.invert_y << "\n";
    for (int i = 0; i < 14; i++)
      s << "map" << i << "=" << m.bindings[i] << "\n";
    if (writeFile(controllerFile(p), s.str())) {
      assignments.configure(static_cast<std::size_t>(p), m);
      label("dialog-note", "Saved. Applied to the game immediately.");
      return true;
    }
    return false;
  }
  // Any connected controller can be chosen; one another player has moves here.
  bool deviceAvailable(int d) const {
    return jfg::controller_device_selectable(d,connected);
  }
  // The other player who has a device now, or -1.
  int deviceOwner(int d) const {
    for(int p=0;p<4;++p)if(p!=player && devices[p]==d)return p;
    return -1;
  }
  void discoverDevices() {
    pollPads();
    for(std::size_t d=0;d<4;++d)connected[d]=pads.info[d].connected;
    resolve();
  }
  void normalizePicker() {
    if(pickerType==-3 || pickerType==-1){pickerDevice=pickerType;return;}
    if(pickerDevice>=0 && deviceAvailable(pickerDevice))return;
    pickerDevice=-2; // a free controller first, then one another player has
    for(int d=0;d<4 && pickerDevice<0;++d)if(deviceAvailable(d) && deviceOwner(d)<0)pickerDevice=d;
    for(int d=0;d<4 && pickerDevice<0;++d)if(deviceAvailable(d))pickerDevice=d;
  }
  bool assignDevice(int d) {
    discoverDevices(); // Revalidate immediately before saving a selection.
    if(!deviceAvailable(d)) {
      normalizePicker();modal("controllers");
      label("dialog-note","That controller is disconnected. Choose another device.");
      return false;
    }
    // RGO: remember which physical controller was chosen, and take it from
    // whoever had it, so choosing a controller works first time.
    const auto previousMappings=mappings;
    auto pins=assignments.pins;
    const std::string id=d>=0 && d<4 ? pads.info[static_cast<std::size_t>(d)].id : std::string{};
    const auto changed=jfg::claim_controller(static_cast<std::size_t>(player),d,id,mappings,pins,devices);
    bool saved=writeFile("controller-ids.ini",jfg::serialize_controller_pins(pins));
    for(int p=0;p<4 && saved;++p)if(changed[static_cast<std::size_t>(p)])saved=saveController(p);
    if(!saved){
      mappings=previousMappings;
      for(int p=0;p<4;++p)if(changed[static_cast<std::size_t>(p)])(void)saveController(p);
      (void)writeFile("controller-ids.ini",jfg::serialize_controller_pins(assignments.pins));
      resolve();label("dialog-note","Could not save the device assignment.");return false;
    }
    assignments.pins=pins;
    std::string moved;
    for(int p=0;p<4;++p)if(p!=player && changed[static_cast<std::size_t>(p)])moved+=(moved.empty()?"":", ")+std::string("Player ")+std::to_string(p+1);
    resolve();learning=keyLearning=-1;
    devicePicker=false;controllerSettings=false;
    modal("controllers");el("dialog-body")->SetScrollTop(0);
    if(!moved.empty())label("dialog-note","Moved from "+moved+", now set to Automatic.");
    return true;
  }
  std::string deviceSetupHtml() {
    const int configured=mappings[player].device,d=devices[player];
    const bool online=d==-3 || (d>=0 && connected[d]);
    const std::string name=d==-3?"Mouse and keyboard":d>=0?"Controller "+std::to_string(d+1):"No device";
    const std::string model=d>=0 && !padNames[static_cast<std::size_t>(d)].empty()?
      padNames[static_cast<std::size_t>(d)]+" - "+std::string(jfg::gamepad_family_name(pads.info[static_cast<std::size_t>(d)].family)):"";
    std::string html="<div class=\"input-device-card\"><div class=\"device-summary\"><div class=\"grow\"><div class=\"label\">INPUT DEVICE</div><div class=\"device-name\">"+name+
      "</div>"+(model.empty()?std::string{}:"<div class=\"muted\">"+escape(model)+"</div>")+"<div id=\"assignment\" class=\"muted\"></div></div><span class=\"connection-state "+std::string(online?"online":"offline")+"\">"+(online?"Connected":"Disconnected")+"</span>"+
      button("device-open",configured==-2?"+ Add device":"Change device")+
      button("device-remove","Remove",configured==-2)+"</div>";
    if(player==0 && configured!=-2) {
      // RGO dual input: Player 1 also uses the free keyboard/mouse or controller.
      const int companion=jfg::companion_device(devices,connected);
      const bool on=pcInput[0].dual_input!=0;
      const std::string second=companion==-3?"mouse and keyboard":companion>=0?padLabel(companion):"";
      html+="<div class=\"device-row\"><div class=\"grow\">Dual input<div class=\"muted\">"+
        escape(second.empty()?"Player 1 also uses a free controller or the mouse and keyboard when one is available.":
          "Also using "+second+". ")+
        escape(on?"On: both at the same time.":"Off: the device you used last controls Player 1.")+"</div></div>"+
        button("dual-input",on?"On":"Off")+"</div>";
    }
    if(d>=0) {
      const int strength=pcInput[static_cast<std::size_t>(player)].rumble;
      const bool canRumble=online && pads.info[static_cast<std::size_t>(d)].rumble;
      html+="<div class=\"device-row\"><span>Rumble strength</span><input class=\"volume\" type=\"range\" min=\"0\" max=\"200\" step=\"10\" value=\""+
        std::to_string(strength)+"\" data-action=\"rumble-strength\"/><span id=\"rumble-value\">"+(strength?std::to_string(strength)+"%":std::string("Off"))+"</span>"+
        button("rumble-test","Test rumble",!canRumble || !strength)+"</div>";
      if(online && pads.info[static_cast<std::size_t>(d)].gyro)html+=gyroHtml(pcInput[static_cast<std::size_t>(player)]);
    }
    html+="</div>";
    if(!devicePicker)return html;
    normalizePicker();
    html+="<div id=\"device-picker\" class=\"device-picker\"><div class=\"label\">CHOOSE INPUT DEVICE</div><div class=\"device-row\"><span>Input type</span><select id=\"input-type\" data-action=\"input-type\">";
    for(int type:{0,-3,-1})html+="<option value=\""+std::to_string(type)+"\""+(type==pickerType?" selected=\"selected\"":"")+">"+(type==0?"Gamepad":type==-3?"Mouse and keyboard":"Automatic")+"</option>";
    html+="</select></div>";
    bool available=pickerType!=0?deviceAvailable(pickerDevice):pickerDevice>=0;
    if(pickerType==0){
      html+="<div class=\"device-row\"><span>Controller</span><select id=\"available-device\" data-action=\"device\""+std::string(available?"":" disabled=\"disabled\"")+">";
      if(!available)html+="<option value=\"-2\">No controllers connected</option>";
      for(int slot=0;slot<4;++slot)if(deviceAvailable(slot)) {
        const int owner=deviceOwner(slot);
        html+="<option value=\""+std::to_string(slot)+"\""+(slot==pickerDevice?" selected=\"selected\"":"")+">"+escape(padLabel(slot))+
          (owner>=0?" - Player "+std::to_string(owner+1):std::string{})+"</option>";
      }
      html+="</select>"+button("device-refresh","Refresh")+"</div><p class=\"muted\">"+
        std::string(pickerDevice>=0 && deviceOwner(pickerDevice)>=0?"This controller moves here from Player "+std::to_string(deviceOwner(pickerDevice)+1)+".":"Connected controllers are listed.")+"</p>";
    } else if(pickerType==-3)html+="<p class=\"muted\">"+std::string(deviceOwner(-3)<0?"Use keyboard keys, mouse buttons and mouse look.":"Mouse and keyboard move here from Player "+std::to_string(deviceOwner(-3)+1)+".")+"</p>";
    else html+="<p class=\"muted\">Use the next free controller. Player 1 falls back to keyboard when no controller has been assigned.</p>";
    html+="<div class=\"picker-actions\">"+button("device-apply",configured==-2?"Add":"Use device",!available)+button("device-cancel","Cancel")+"</div></div>";
    return html;
  }
  bool bindingMessage(UINT message,WPARAM key) {
    if(keyLearning<0)return false;
    if(message==WM_KILLFOCUS || (message==WM_ACTIVATEAPP && !key)) {
      keyLearning=-1;modal("controllers");return false;
    }
    if((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && key==VK_ESCAPE) {
      keyLearning=-1;modal("controllers");label("dialog-note","Binding cancelled.");return true;
    }
    int binding=-1;
    if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN)binding=static_cast<int>(key);
    else if(message==WM_LBUTTONDOWN)binding=VK_LBUTTON;
    else if(message==WM_RBUTTONDOWN)binding=VK_RBUTTON;
    else if(message==WM_MBUTTONDOWN)binding=VK_MBUTTON;
    else if(message==WM_XBUTTONDOWN)binding=GET_XBUTTON_WPARAM(key)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2;
    else if(message==WM_MOUSEWHEEL)binding=GET_WHEEL_DELTA_WPARAM(key)>0?256:257;
    if(binding>=0){
      if(keyReleased && binding!=VK_F11 && pcKeyboard()) {
        pcInput[static_cast<std::size_t>(player)].keys[static_cast<std::size_t>(keyLearning)]=binding;
        keyLearning=-1;savePcInput();modal("controllers");
      }
      return true;
    }
    // Keep release events away from RmlUi so the capture click cannot activate another field.
    return message==WM_KEYUP || message==WM_SYSKEYUP || message==WM_CHAR ||
      message==WM_LBUTTONUP || message==WM_RBUTTONUP || message==WM_MBUTTONUP || message==WM_XBUTTONUP;
  }
  std::string bindingName(int b) const {
    return std::string(jfg::gamepad_binding_name(b, padFamily()));
  }
  static std::array<bool, jfg::kBindingCount> sample(const jfg::StandardControllerSample &s) {
    std::array<bool, jfg::kBindingCount> out{};
    for (std::size_t i = 0; i < 15; i++)
      out[i] = s.buttons[i];
    for (std::size_t i = 0; i < 6; i++) {
      out[15 + i * 2] = s.axes[i] > 16000;
      out[16 + i * 2] = s.axes[i] < -16000;
    }
    out[jfg::kBindingTouchpad] = s.buttons[15];
    out[jfg::kBindingMisc] = s.buttons[16];
    return out;
  }

  Rml::ElementDocument *doc = nullptr;
  fs::path profile;
  int volume = 100, player = 0;
  bool muted = false;
  // RGO persistent gore (gore.ini, docs/rgo/gore.md).
  jfg::rgo::GoreSettings gore{};
  double toastUntil = 0, decorationTick = -1;
  std::array<bool, 4> connected{};
  std::array<std::uint32_t, 4> buttons{};
  std::array<bool, 4> activated{};
  std::deque<std::string> notices;
  bool reducedMotion = false;
  std::string current, sessionList, selectedSession;
  Rml::Element *el(const char *id) { return doc->GetElementById(id); }
  void label(const char *id, const std::string &s) {
    el(id)->SetInnerRML(escape(s));
  }
  std::string read(const char *file) {
    std::ifstream f(profile / file, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() < 0 || f.tellg() > 65536)
      return {};
    f.seekg(0);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  void status(const std::string &s) { label("status", s); }
  void toast(const std::string &s) {
    if (toastUntil > Rml::GetSystemInterface()->GetElapsedTime()) {
      if ((notices.empty() || notices.back() != s) && notices.size() < 8)
        notices.push_back(s);
      return;
    }
    label("toast-text", s);
    el("toast")->SetClass("visible", false);
    doc->GetContext()->Update();
    el("toast")->SetClass("visible", true);
    toastUntil = Rml::GetSystemInterface()->GetElapsedTime() + 4.62;
  }
  void refreshController() {
    if (current != "controllers" || !el("assignment"))
      return;
    const int d = devices[player];
    std::string description =
        pcKeyboard() ? "Mouse and keyboard assigned to this player."
        : d < 0 && assignments.pinned(static_cast<std::size_t>(player)) ? "The chosen controller is not connected."
        : d < 0 ? "No physical controller is assigned."
                : "Controller " + std::to_string(d + 1) +
                      (connected[d] ? " connected." : " offline.");
    if(mappings[player].device==-1)description="Automatic: "+description;
    if(d>=0 && !connected[d])description+=" Saved bindings will return when it reconnects.";
    label("assignment", description);
    for (int p = 0; p < 4; p++) {
      const int assigned = devices[p];
      label(("tab-device" + std::to_string(p)).c_str(),
            assigned == -3 ? "Keyboard"
            : assigned < 0 ? "Not connected"
                           : "Controller " + std::to_string(assigned + 1) +
                                 (connected[assigned] ? "" : " (offline)"));
    }
    const bool disabled = d < 0 || !connected[d];
    for (int i = 0; i < 14; i++)
      if (auto *b = el(("action-learn" + std::to_string(i)).c_str())) {
        b->SetClass("disabled", disabled);
        if (disabled)
          b->SetAttribute("disabled", "disabled");
        else
          b->RemoveAttribute("disabled");
      }
  }
  void ports() {
    const bool wasKeyboard=pcKeyboard();
    const auto wasConnected=connected;const auto wasDevices=devices;
    pollPads();
    for (std::size_t i = 0; i < 4; i++) {
      const bool previouslyConnected = connected[i];
      std::string assignedPlayer;
      for (int p = 0; p < 4; p++)
        if (devices[p] == int(i))
          assignedPlayer = " - Player " + std::to_string(p + 1);
      const bool on = pads.info[i].connected;
      const std::uint32_t held = on ? padButtons(pads.pads[i]) : 0U;
      const std::uint32_t pressed = held & ~buttons[i];
      if (on != connected[i]) {
        connected[i] = on;
        activated[i] = false;
        resolve();
        std::string port;
        for (int p = 0; p < 4; p++)
          if (devices[p] == int(i))
            port = " - Player " + std::to_string(p + 1);
        toast(padLabel(static_cast<int>(i)) +
              (on ? " connected" : " disconnected") +
              (on ? port : assignedPlayer));
      } else if (pressed && !activated[i]) {
        activated[i] = true;
        toast(padLabel(static_cast<int>(i)) + " active" +
              assignedPlayer);
      }

      if (learning >= 0 && devices[player] == int(i)) {
        if (!on) {
          learning = -1;
          modal("controllers");
          label("dialog-note",
                "Controller disconnected. Select a device and retry.");
        } else {
          auto active = sample(pads.pads[i]);
          bool neutral = std::none_of(active.begin(), active.end(),
                                      [](bool b) { return b; });
          if (!learnReleased) {
            if (neutral) {
              learnReleased = true;
              label("dialog-note",
                    "Press the new button or move an axis. Esc cancels.");
            }
          } else
            for (int b = 0; b < jfg::kBindingCount; b++)
              if (active[static_cast<std::size_t>(b)]) {
                mappings[player].bindings[learning] = b;
                learning = -1;
                saveController();
                modal("controllers");
                break;
              }
        }
      }
      if (pressed && on && previouslyConnected && current.empty() &&
          !state.playing && !state.busy && (pressed & 1U)) // A / Cross
        action("play");
      buttons[i] = held;
    }

    resolve();
    lightPads();
    if(current=="controllers" && (wasConnected!=connected || wasDevices!=devices || wasKeyboard!=pcKeyboard())) {
      learning=keyLearning=-1;normalizePicker();modal("controllers");
      if(wasKeyboard!=pcKeyboard())el("dialog-body")->SetScrollTop(0);
    }
    if(keyLearning>=0) {
      bool released=true;for(int k=1;k<256;++k)if(GetAsyncKeyState(k)&0x8000){released=false;break;}
      if(released && !keyReleased){keyReleased=true;label("dialog-note","Press a key, mouse button or scroll the wheel. Esc cancels. F11 is reserved.");}
      if(Rml::GetSystemInterface()->GetElapsedTime()>keyDeadline){keyLearning=-1;modal("controllers");label("dialog-note","Binding timed out. No binding was changed.");}
    }
    refreshController();
    for (int p = 0; p < 4; p++) {
      bool on = devices[p] == -3 || (devices[p] >= 0 && connected[devices[p]]);
      el(("p" + std::to_string(p + 1)).c_str())->SetClass("connected", on);
    }
    if (learning >= 0 &&
        Rml::GetSystemInterface()->GetElapsedTime() > learnDeadline) {
      learning = -1;
      modal("controllers");
      label("dialog-note", "Mapping timed out. No binding was changed.");
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
          "translate(0dp, " + std::to_string(reducedMotion ? 0 : y) + "dp)");
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
    if (auto settings = jfg::audio::read_volume(profile / "audio.ini")) {
      volume = static_cast<int>(settings->percent);
      muted = settings->muted;
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
  void loadGore() {
    std::ifstream f(profile / jfg::rgo::kGoreSettingsFileName, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    gore = jfg::rgo::parse_gore_settings(text).value_or(jfg::rgo::GoreSettings{});
  }
  void saveGore() {
    std::error_code ec;
    fs::create_directories(profile, ec);
    const auto dest = profile / jfg::rgo::kGoreSettingsFileName;
    const auto tmp = profile / (std::string(jfg::rgo::kGoreSettingsFileName) + ".ui.tmp");
    {
      std::ofstream f(tmp, std::ios::binary);
      f << jfg::rgo::encode_gore_settings(gore);
      f.flush();
      if (ec || !f) {
        status("Cannot save gameplay settings.");
        return;
      }
    }
    if (!MoveFileExW(tmp.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      status("Cannot save gameplay settings.");
  }
  std::string goreHtml() {
    std::string html = "<div class=\"row\"><div class=\"grow\">Alien heads stay<div class=\"muted\">Drone heads (and "
                       "dropped pickups) stay until you pick them up. Off: they fade after about 10 seconds.</div></div>"
                       "<div class=\"segments\">" + button("heads-on", "On") + button("heads-off", "Off") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Body parts stay<div class=\"muted\">Severed limbs stay where they "
            "land. Off: they fade after about 10 seconds.</div></div><div class=\"segments\">" +
            button("limbs-on", "On") + button("limbs-off", "Off") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Blood splats stay<div class=\"muted\">Blood on floors and walls "
            "stays. Off: it fades after about 5 seconds.</div></div><div class=\"segments\">" +
            button("blood-on", "On") + button("blood-off", "Off") + "</div></div>";
    html += "<p class=\"muted\">Up to " + std::to_string(gore.head_limit) + " heads, " +
            std::to_string(gore.limb_limit) + " body parts and " + std::to_string(gore.blood_limit) +
            " splats stay at once; past that the oldest fades as in the original game. They also fade "
            "early if the game runs low on memory, and when you leave the area.</p>";
    return html;
  }
  // Click actions on the Gameplay page. Returns false for other actions.
  bool goreAction(const std::string &a) {
    if (a == "heads-on" || a == "heads-off") gore.heads = a == "heads-on";
    else if (a == "limbs-on" || a == "limbs-off") gore.limbs = a == "limbs-on";
    else if (a == "blood-on" || a == "blood-off") gore.blood = a == "blood-on";
    else return false;
    saveGore();
    modal("gameplay");
    return true;
  }
  std::string button(const std::string &action, const std::string &text,
                     bool disabled = false, const std::string &shortcut = "") {
    return "<button id=\"action-" + action + "\" data-action=\"" + action +
           "\"" +
           (disabled ? " disabled=\"disabled\" class=\"disabled\"" : "") + ">" +
           escape(text) + (shortcut.empty() ? "" :
               "<span class=\"menu-shortcut\">" + escape(shortcut) + "</span>") + "</button>";
  }
  // ---- RGO display settings -------------------------------------------
  std::vector<jfg::PixelSize> monitorSizes() const {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    std::vector<jfg::PixelSize> modes;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
      return modes;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(monitor.szDevice, i, &mode); ++i)
      modes.push_back({mode.dmPelsWidth, mode.dmPelsHeight});
    return jfg::unique_output_sizes(modes);
  }
  static std::string sizeText(jfg::PixelSize size) {
    return std::to_string(size.width) + " x " + std::to_string(size.height);
  }
  // The renderer's choice for the current game area, as the shell makes it.
  jfg::PixelSize plannedRenderSize(const jfg::VideoSettings &v) const {
    const jfg::PixelSize window{static_cast<std::uint32_t>(std::max(state.viewWidth, 0)),
                                static_cast<std::uint32_t>(std::max(state.viewHeight, 0))};
    const jfg::PixelSize framebuffer{320U, 240U};
    const double source = 4.0 / 3.0;
    const double target = v.aspect == jfg::AspectMode::widescreen
        ? jfg::widescreen_aspect_target(window.width, window.height, 320U, 240U, 320U, 240U, v.max_aspect.value())
        : source;
    const auto picture = jfg::fit_picture(window, target);
    if (!picture.valid())
      return {};
    return jfg::render_size(jfg::render_multiplier(v, picture, framebuffer.height), framebuffer, target / source);
  }
  std::string renderSizeNote(const jfg::VideoSettings &v) const {
    const auto size = plannedRenderSize(v);
    return size.valid() ? "Renders at " + sizeText(size) + " for the current game area (" +
                              std::to_string(state.viewWidth) + " x " + std::to_string(state.viewHeight) + ")."
                        : "";
  }
  static std::string option(const std::string &value, const std::string &text, bool selected) {
    return "<option value=\"" + value + "\"" + (selected ? " selected=\"selected\"" : "") + ">" +
           escape(text) + "</option>";
  }
  std::string sizeInputs(const std::string &prefix, const std::string &width,
                         const std::string &height, const std::string &apply) {
    return "<input type=\"text\" class=\"size-input\" id=\"" + prefix + "-width\" data-action=\"" + prefix +
           "-width\" size=\"5\" maxlength=\"5\" value=\"" + escape(width) + "\"/><span>x</span>" +
           "<input type=\"text\" class=\"size-input\" id=\"" + prefix + "-height\" data-action=\"" + prefix +
           "-height\" size=\"5\" maxlength=\"5\" value=\"" + escape(height) + "\"/>" + button(apply, "Apply");
  }
  std::string videoHtml() {
    const auto &v = state.video;
    const bool exclusive = v.fullscreen == jfg::FullscreenMode::exclusive;
    std::string html = "<div class=\"split\"><div class=\"grow\">";
    html += "<div class=\"row\"><span class=\"grow\">Window mode</span><div class=\"segments\">" +
            button("window-windowed", "Windowed") + button("window-borderless", "Borderless") +
            button("window-exclusive", "Exclusive") + "</div></div>";
    // Output resolution: the monitor's own modes, or any typed size.
    const auto modes = monitorSizes();
    const bool listed = !v.output.valid() || std::find(modes.begin(), modes.end(), v.output) != modes.end();
    const bool custom = customOutput || !listed;
    if (outputWidth.empty() && v.output.valid()) {
      outputWidth = std::to_string(v.output.width);
      outputHeight = std::to_string(v.output.height);
    }
    std::string outputNote = state.fullscreen && !exclusive
        ? "Borderless uses your desktop resolution."
        : state.fullscreen ? "Display mode for exclusive fullscreen."
                           : "Sets the game area. You can also resize the window freely.";
    html += "<div class=\"row\"><div class=\"grow\">Output resolution<div class=\"muted\">" + outputNote +
            "</div></div><select id=\"output-select\" data-action=\"output-select\">" +
            option("auto", state.fullscreen ? "Desktop resolution" : "Current window", !v.output.valid() && !custom);
    for (const auto size : modes)
      html += option(std::to_string(size.width) + "x" + std::to_string(size.height), sizeText(size),
                     !custom && size == v.output);
    html += option("custom", "Custom...", custom) + "</select></div>";
    if (custom)
      html += "<div class=\"row size-row\"><span class=\"grow muted\">Custom output size</span>" +
              sizeInputs("output", outputWidth, outputHeight, "output-apply") + "</div>";
    // Render resolution.
    html += "<div class=\"row\"><div class=\"grow\">Render resolution<div class=\"muted\" id=\"render-note\">" +
            escape(renderSizeNote(v)) + "</div></div><select id=\"render-mode\" data-action=\"render-mode\">" +
            option("match", "Match output", v.render == jfg::RenderMode::match) +
            option("scale", "Render scale", v.render == jfg::RenderMode::scale) +
            option("custom", "Custom size", v.render == jfg::RenderMode::custom) + "</select></div>";
    if (v.render == jfg::RenderMode::scale)
      html += "<div class=\"row size-row\"><span class=\"grow muted\">Render scale</span><input class=\"volume\" "
              "type=\"range\" min=\"25\" max=\"200\" step=\"5\" value=\"" + std::to_string(v.render_scale) +
              "\" data-action=\"render-scale\"/><span id=\"render-scale-value\" class=\"scale-value\">" +
              std::to_string(v.render_scale) + "%</span></div>";
    if (v.render == jfg::RenderMode::custom) {
      if (renderWidth.empty()) {
        renderWidth = std::to_string(v.render_size.width);
        renderHeight = std::to_string(v.render_size.height);
      }
      html += "<div class=\"row size-row\"><span class=\"grow muted\">Custom render size</span>" +
              sizeInputs("render", renderWidth, renderHeight, "render-apply") + "</div>";
    }
    html += "<div class=\"row\"><span class=\"grow\">Aspect ratio</span><div class=\"segments\">" +
            button("aspect-original", "Original 4:3") + button("aspect-widescreen", "Widescreen") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Max aspect ratio<div class=\"muted\">Wider screens get side bars.</div></div>"
            "<select id=\"max-aspect\" data-action=\"max-aspect\"" +
            std::string(v.aspect == jfg::AspectMode::widescreen ? "" : " disabled=\"disabled\"") + ">";
    bool known = false;
    for (const char *limit : {"unlimited", "32:9", "21:9", "16:9", "16:10"}) {
      const bool selected = jfg::aspect_limit_text(v.max_aspect) == limit;
      known |= selected;
      html += option(limit, std::string(limit) == "unlimited" ? "Unlimited" : limit, selected);
    }
    if (!known)
      html += option(jfg::aspect_limit_text(v.max_aspect), jfg::aspect_limit_text(v.max_aspect), true);
    html += "</select></div>";
    html += "<div class=\"row\"><div class=\"grow\">Anti-aliasing<div class=\"muted\">MSAA. Applies the next time the game starts.</div></div>"
            "<div class=\"segments\">" + button("msaa-1", "Off") + button("msaa-2", "2x") + button("msaa-4", "4x") +
            button("msaa-8", "8x") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Frame rate<div class=\"muted\">Above Original the game itself runs at 60. "
            "Game speed stays the same.</div></div><div class=\"segments\">" + button("framerate-original", "Original") +
            button("framerate-60", "True 60") + button("framerate-display", "Display") + button("framerate-unlocked", "Unlocked") +
            "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Interpolate extra frames<div class=\"muted\">Display and Unlocked: blend frames "
            "between the game's 60 to reach your display's rate.</div></div><div class=\"segments\">" +
            button("interpolate-on", "On") + button("interpolate-off", "Off") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Blend moving objects<div class=\"muted\">With interpolation: characters "
            "and objects move smoothly too. Off: only the camera blends.</div></div><div class=\"segments\">" +
            button("objects-on", "On") + button("objects-off", "Off") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Original N64 slowdown<div class=\"muted\">Original frame rate only. On: heavy "
            "scenes drop to 20 fps as on the console. Off keeps a steady 30.</div></div><div class=\"segments\">" +
            button("slowdown-off", "Off") + button("slowdown-on", "On") + "</div></div>";
    html += "<div class=\"row\"><span class=\"grow\">VSync</span><div class=\"segments\">" + button("vsync-on", "On") +
            button("vsync-off", "Off") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">FPS counter<div class=\"muted\">Game: logic updates per second. Render: frames "
            "drawn. Display: frames shown, blended ones included. Detailed adds new frames, camera updates, input "
            "and frame times.</div></div><div class=\"segments\">" + button("fps-off", "Off") +
            button("fps-on", "On") + button("fps-detailed", "Detailed") + "</div></div>";
    html += "<div class=\"row\"><div class=\"grow\">Pause when inactive<div class=\"muted\">Pause when you switch to another app. Resume when you return.</div></div>" +
            button("pause-inactive", state.pauseInactive ? "On" : "Off") + "</div>";
    html += "</div><div class=\"about\"><div class=\"label\">ABOUT</div><div class=\"name\">Display</div>"
            "<p>Widescreen keeps the original height of view and shows more to the sides, at any window shape, "
            "without stretching. Narrow and portrait windows show the original 4:3 picture with bars.</p>"
            "<p>Match output renders exactly as many pixels as the picture covers. Lower the render scale for "
            "speed, or raise it for sharper edges.</p><p>True 60 runs the game's own logic and drawing at 60 "
            "per second. Display and Unlocked add blended frames on top (experimental).</p><p class=\"muted\">F11 switches between windowed and the "
            "chosen fullscreen. Leave the game's own Widescreen option off.</p></div></div>";
    return html;
  }
  void saveVideo(const jfg::VideoSettings &v, bool rebuild) {
    state.video = v;
    action("video-set\n" + jfg::encode_video_settings(v));
    if (rebuild && current == "video")
      modal("video");
    else if (auto *e = el("render-note"))
      e->SetInnerRML(escape(renderSizeNote(v)));
  }
  // Click actions on the Video page. Returns false for other actions.
  bool videoAction(const std::string &a) {
    auto v = state.video;
    if (a == "aspect-original" || a == "aspect-widescreen")
      v.aspect = a == "aspect-widescreen" ? jfg::AspectMode::widescreen : jfg::AspectMode::original;
    else if (a.rfind("msaa-", 0) == 0)
      v.msaa = static_cast<std::uint8_t>(std::clamp(std::atoi(a.c_str() + 5), 1, 8));
    else if (a == "vsync-on" || a == "vsync-off")
      v.vsync = a == "vsync-on";
    else if (a == "fps-on" || a == "fps-off" || a == "fps-detailed") {
      v.fps_counter = a != "fps-off";
      v.fps_details = a == "fps-detailed";
    }
    else if (a == "slowdown-on" || a == "slowdown-off")
      v.n64_slowdown = a == "slowdown-on";
    else if (a == "interpolate-on" || a == "interpolate-off")
      v.interpolate_frames = a == "interpolate-on";
    else if (a == "objects-on" || a == "objects-off")
      v.interpolate_objects = a == "objects-on";
    else if (a == "framerate-original" || a == "framerate-60" || a == "framerate-display" ||
             a == "framerate-unlocked")
      v.framerate = a == "framerate-60"        ? jfg::FrameRate::sixty
                    : a == "framerate-display"  ? jfg::FrameRate::display
                    : a == "framerate-unlocked" ? jfg::FrameRate::unlocked
                                                : jfg::FrameRate::original;
    else if (a == "output-apply" || a == "render-apply") {
      const bool output = a == "output-apply";
      const auto size = jfg::parse_typed_size(output ? outputWidth : renderWidth, output ? outputHeight : renderHeight,
                                              output ? jfg::kMinimumOutputDimension : jfg::kMinimumRenderDimension);
      if (!size) {
        label("dialog-note", output ? "Enter a width and height of 320 to 16384 pixels."
                                    : "Enter a width and height of 160 to 16384 pixels.");
        return true;
      }
      (output ? v.output : v.render_size) = *size;
    } else
      return false;
    saveVideo(v, true);
    return true;
  }
  // Change events (selects, slider, typed sizes). Returns false for others.
  bool videoChange(const std::string &a, Rml::Event &event) {
    auto v = state.video;
    const auto value = event.GetParameter<Rml::String>("value", "");
    if (a == "output-width") { outputWidth = value; return true; }
    if (a == "output-height") { outputHeight = value; return true; }
    if (a == "render-width") { renderWidth = value; return true; }
    if (a == "render-height") { renderHeight = value; return true; }
    if (a == "output-select") {
      customOutput = value == "custom";
      if (customOutput) { modal("video"); return true; }
      if (value == "auto") v.output = {};
      else if (const auto split = value.find('x'); split != std::string::npos) {
        if (const auto size = jfg::parse_typed_size(value.substr(0, split), value.substr(split + 1), jfg::kMinimumOutputDimension))
          v.output = *size;
      }
      saveVideo(v, true);
      return true;
    }
    if (a == "render-mode") {
      v.render = value == "scale" ? jfg::RenderMode::scale : value == "custom" ? jfg::RenderMode::custom : jfg::RenderMode::match;
      saveVideo(v, true);
      return true;
    }
    if (a == "render-scale") {
      const int scale = std::clamp((event.GetParameter<int>("value", 100) + 2) / 5 * 5, 25, 200);
      if (static_cast<std::uint32_t>(scale) == v.render_scale) return true;
      v.render_scale = static_cast<std::uint32_t>(scale);
      label("render-scale-value", std::to_string(scale) + "%");
      saveVideo(v, false);  // Rebuilding would interrupt the drag.
      return true;
    }
    if (a == "max-aspect") {
      if (const auto limit = jfg::parse_aspect_limit(value)) {
        v.max_aspect = *limit;
        saveVideo(v, true);
      }
      return true;
    }
    return false;
  }
  std::string aboutHtml() const {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(209), RT_RCDATA);
    const auto data = resource ? LockResource(LoadResource(nullptr, resource)) : nullptr;
    if (!data) return "<p>About information is unavailable. Rebuild the launcher.</p>";
    std::string html(static_cast<const char *>(data), SizeofResource(nullptr, resource));
    auto fill = [&](const std::string &key, const std::string &value) {
      const auto at = html.find(key);
      if (at != std::string::npos) html.replace(at, key.size(), escape(value));
    };
    fill("@VERSION@", jfg::frontend::version);
    fill("@BUILD@", jfg::frontend::revision);
    fill("@DATE@", jfg::frontend::built);
    fill("@ROM@", state.rom.empty() ? "Not selected" : state.playing ? "US \xC2\xB7 verified" : "Selected \xC2\xB7 check on play");
    return html;
  }
  void toggleMute() {
    loadAudio();
    muted = !muted;
    saveAudio();
    if (current == "audio" || current == "menu-audio")
      modal(current);
    else
      toast(muted ? "Audio muted" : "Audio unmuted");
  }
  bool popup() const { return current.rfind("menu-", 0) == 0; }
  void modal(const std::string &page) {
    if(page=="pc-input"){modal("controllers");return;}
    struct Building {
      bool &b;
      Building(bool &v) : b(v) { b = true; }
      ~Building() { b = false; }
    } guard(building);
    auto *focused = doc->GetContext()->GetFocusElement();
    const std::string focusId =
        current == page && focused ? focused->GetId() : "";
    const auto scroll = (current == page && el("dialog-body"))
                            ? el("dialog-body")->GetScrollTop()
                            : 0.f;
    current = page;
    doc->SetClass("panel-open", !popup());
    doc->SetClass("menu-open", popup());
    for (const auto *name :
         {"game", "controllers", "video", "audio", "tools", "help"})
      el((std::string("menu-") + name).c_str())
          ->SetClass("selected",
                     page == name || page == std::string("menu-") + name ||
                         (page == "support" && std::string(name) == "help"));
    el("dialog")->SetClassNames("dialog " + page);
    el("game-popup")->SetProperty("display", "none");
    if (popup()) {
      const auto menuName = page.substr(5);
      el("modal")->SetProperty("display", "none");
      doc->GetContext()->Update(); // Restore fullscreen menu layout before focusing its anchor.
      auto *anchor = el(page.c_str());
      anchor->Focus(true);
      el("game-popup")->SetProperty("left", std::to_string(anchor->GetAbsoluteOffset(Rml::BoxArea::Border).x) + "px");
      std::string menu;
      if (menuName == "controllers") {
        menu = button("controllers", "Controller mapping...");
      } else if (menuName == "video") {
        menu = button("video", "Video settings...") + button("fullscreen", state.fullscreen ? "Leave fullscreen" : "Fullscreen", false, "F11");
      } else if (menuName == "audio") {
        loadAudio();
        menu = button("audio", "Audio settings...") + button("mute", muted ? "Unmute" : "Mute", false, "Ctrl+M");
      } else if (menuName == "help") {
        menu = button("guide", "Setup guide") + button("support", "Support report...") + "<div class=\"separator\"/>" + button("help", "About Jet Force Gemini...");
      } else if (menuName == "tools") {
        menu = button("map", "Live map") +
               button("inventory", "Live inventory");
      } else {
        if (state.playing) {
          const bool paused = GetPropW(window, L"JfgFrontendPaused") != nullptr;
          menu = button(paused ? "resume" : "pause", paused ? "Resume game" : "Pause game") +
                 button("stop", "Stop / return home");
        }
        menu += button("gameplay", "Gameplay settings...");
        menu += button("setup", "Set up and build...", state.playing) +
                button("rom", "Change ROM...", state.busy);
        menu += button("shortcut", "Save direct-launch shortcut...", state.busy || state.rom.empty() || state.runtime.empty());
        menu += button("saves", "Open save folder");
        menu += "<div class=\"separator\"/>";
        menu += button("quit", "Quit");
      }
      el("game-popup")->SetInnerRML(menu);
      el("game-popup")->SetProperty("display", "block");
      return;
    }
    el("modal")->SetProperty("display", "block");
    std::string html, note = "Settings are saved to your game profile.";
    label("dialog-title", page == "pc-input" ? "KEYBOARD, MOUSE AND AIM"
                          : page == "controllers" ? "CONTROLLER MAPPING"
                          : page == "setup"     ? "SET UP AND BUILD"
                          : page == "support"   ? "SUPPORT REPORT"
                          : page == "video"     ? "VIDEO"
                          : page == "audio"     ? "AUDIO"
                          : page == "gameplay"  ? "GAMEPLAY"
                                                : "JFG RECOMP");
    el("dialog")->SetProperty("width", page == "controllers" || page == "video" ? "880dp"
                                       : page == "audio"     ? "480dp"
                                       : page == "setup"     ? "640dp"
                                       : page == "support"   ? "720dp"
                                       : page == "help"      ? "960dp"
                                                             : "760dp");
    el("dialog")->SetProperty("height", page == "controllers" ? "600dp"
                                        : page == "audio"     ? "280dp"
                                        : page == "video"     ? "620dp"
                                        : page == "setup"     ? "420dp"
                                        : page == "help"      ? "640dp"
                                                              : "460dp");
    el("dialog-extra")
        ->SetInnerRML(
            page == "controllers" && !pcKeyboard() && mappings[player].device>=-1 ? button("reset", "Restore controller defaults") : "");
    if (page == "pc-input") {
      html=pcExperimental()?pcInputHtml():"<p>Enable Experimental PC controls in Controller mapping first.</p>";
      el("dialog")->SetProperty("width","880dp");el("dialog")->SetProperty("height","640dp");
      note="Changes apply live. Mouse bindings work on the player assigned Keyboard.";
    } else if (page == "audio") {
      html = "<div class=\"row\"><div class=\"grow\">Master volume<div "
             "class=\"muted\">Music and effects</div></div><span "
             "id=\"volume-value\" class=\"title-font\" "
             "style=\"font-size:36dp;color:#ffb23e\">" +
             (muted ? std::string("MUTED") : std::to_string(volume) + "%") +
             "</span></div>";
      html += "<div class=\"row\">" + button("down", "-") +
              "<input class=\"volume\" type=\"range\" min=\"0\" max=\"100\" "
              "step=\"1\" value=\"" +
              std::to_string(volume) + "\" data-action=\"volume\"/>" +
              button("up", "+") + "</div>";
      note = "Applied immediately. Volume is remembered when muted.";
    } else if (page == "gameplay") {
      html = goreHtml();
      note = "Changes apply immediately, also while playing.";
    } else if (page == "video") {
      html = videoHtml();
      note = "Changes apply immediately. Anti-aliasing applies the next time the game starts.";
    } else if (page == "controllers") {
      auto &m = mappings[player];
      loadPcInput();
      html = "<div class=\"tabs\">";
      for (int i = 0; i < 4; i++) {
        const int d = devices[i];
        std::string device = d == -3 ? "Keyboard"
                             : d < 0 ? "Not connected"
                                     : "Controller " + std::to_string(d + 1);
        html += "<button id=\"action-player" + std::to_string(i) +
                "\" data-action=\"player" + std::to_string(i) + "\" class=\"" +
                (i == player ? "selected" : "") +
                "\"><span class=\"tab-title\">PLAYER " + std::to_string(i + 1) +
                "</span><span class=\"tab-device\" id=\"tab-device" +
                std::to_string(i) + "\">" + device + "</span></button>";
      }
      html += "</div>"+deviceSetupHtml();
      if(pcKeyboard()) {
        html+="<div id=\"mapping-content\" class=\"keyboard-mapping\">"+pcInputHtml()+"</div>";
        note="Keyboard and mouse settings save automatically for this player.";
      } else if(m.device==-2) {
        html+="<div id=\"mapping-content\" class=\"muted\">Choose mouse and keyboard or a controller to configure bindings.</div>";
        note="This player port is disconnected.";
      } else {
      // RGO: in the twin-stick scheme this is "Swap sticks" (left-handed):
      // the other stick always looks and aims.
      const bool twinScheme=jfg::pc_twin_stick(pcInput[static_cast<std::size_t>(player)]);
      html+="<div id=\"mapping-content\" class=\"gamepad-mapping\"><div class=\"row\"><span>"+std::string(twinScheme?"Swap sticks (left-handed)":"Movement")+"</span>"+
        button("stick",twinScheme?(m.stick?"On: right stick moves":"Off: left stick moves"):(m.stick?"Right stick":"Left stick"))+
        button("controller-settings",controllerSettings?"Hide settings":"Settings")+"</div>";
      html+=twinStickHtml();
      html+=gameControlsHtml(pcInput[static_cast<std::size_t>(player)]);
      const bool expertLabels=pcInput[static_cast<std::size_t>(player)].game_controls!=0;
      const int groups[3][6] = {
          {0, 1, 2, 9, 8, 3}, {10, 11, 12, 13, -1, -1}, {4, 5, 6, 7, -1, -1}};
      const char *titles[] = {"BUTTONS", "C BUTTONS", "D-PAD"};
      html += "<div class=\"mapping-groups\">";
      for (int g = 0; g < 3; g++) {
        html += "<div class=\"mapping-group\"><div class=\"label\">" +
                std::string(titles[g]) + "</div>";
        for (int i : groups[g]) {
          if (i < 0)
            continue;
          html += "<div class=\"binding-row\" id=\"binding-" +
                  std::to_string(i) + "\"><span class=\"binding-label\">" +
                  escape(jfg::game_action_label(i, expertLabels)) + "</span>" +
                  button("learn" + std::to_string(i), bindingName(m.bindings[i]),
                         devices[player] < 0) +
                  button("clear" + std::to_string(i), "x") + "</div>";
        }
        html += "</div>";
      }
      html += "</div>";
      if(controllerSettings){
      html += "<div id=\"controller-settings\"><div class=\"tuning\">";
      const char *tuneNames[] = {"Dead zone", "Stick threshold",
                                 "Trigger threshold"};
      int n = 0;
      for (auto pair : {std::pair{"deadzone", m.deadzone},
                        std::pair{"threshold", m.threshold},
                        std::pair{"trigger", m.trigger}}) {
        std::string key = pair.first;
        html += "<div class=\"tune\"><div class=\"tune-label\"><span>" +
                std::string(tuneNames[n++]) + "</span><span id=\"value-" + key +
                "\">" + std::to_string((pair.second * 100 + 16383) / 32767) +
                "%</span></div><input class=\"volume\" type=\"range\" min=\"" +
                (key == "deadzone" ? "0" : "1000") + "\" max=\"" +
                (key == "deadzone" ? "30000" : "32000") +
                "\" step=\"100\" value=\"" + std::to_string(pair.second) +
                "\" data-action=\"tune-" + key + "\"/></div>";
      }
      html += "</div><div class=\"inversions\">" +
              button("invertx", m.invert_x ? "Invert X: on" : "Invert X: off") +
              button("inverty", m.invert_y ? "Invert Y: on" : "Invert Y: off") +
              "</div>";
      html+="</div>";
      }
      html+="</div>";
      note = "Click a binding, release controls, then press a button or move a stick. Esc cancels.";
      }
    } else if (page == "setup") {
      html = "<div class=\"steps\"><span>1 &nbsp; ROM</span><span>2 &nbsp; "
             "Verify</span><span>3 &nbsp; Build</span><span>4 &nbsp; "
             "Play</span></div><div class=\"name\">" +
             std::string(state.busy              ? "Setting up your game"
                         : state.runtime.empty() ? "Set up your game"
                                                 : "Your game build") +
             "</div><p class=\"muted\">Verify your ROM and prepare the game on "
             "this PC. Setup downloads any missing build tools.</p>";
      html += "<div class=\"setup-log\" id=\"setup-status\"><span>" +
              escape(u8(state.status)) + "</span></div>";
      if (!state.runtime.empty())
        html += "<p class=\"muted install-path\">Installed build: " +
                escape(u8(state.runtime)) + "</p>";
      html +=
          "<div class=\"panel-actions\">" +
          button("setup-start",
                 state.runtime.empty() ? "Set up game" : "Rebuild",
                 state.busy) +
          button("cancel-setup", "Cancel setup", !state.busy || state.playing) +
          "</div>";
      note = "You can close this panel. Setup keeps running; completed "
             "downloads are retained.";
    } else if (page == "support") {
      html = "<div class=\"split\"><div class=\"grow\"><p>Which session had "
             "the problem?</p><div class=\"session-list\">";
      std::istringstream lines(read("frontend-sessions.tsv"));
      std::string line;
      bool any = false;
      while (std::getline(lines, line)) {
        auto split = line.find('\t');
        if (split == std::string::npos)
          continue;
        auto id = line.substr(0, split);
        if (selectedSession.empty())
          selectedSession = id;
        html += "<button class=\"session " +
                std::string(id == selectedSession ? "selected" : "") +
                "\" data-action=\"report-" + escape(id) + "\">" +
                escape(line.substr(split + 1)) + "</button>";
        any = true;
      }
      if (!any)
        html += "<p class=\"muted\">No recorded sessions yet. Start the game "
                "to create a session report.</p>";
      html +=
          "</div>" + button("refresh-sessions", "Refresh sessions") +
          "</div><div class=\"about\"><div class=\"label\">THE ZIP "
          "INCLUDES</div><p>Game and launcher logs</p><p>Settings and "
          "mapping</p><p>System and GPU info</p><p class=\"muted\">No ROM or "
          "save data. Review it, then attach it to your GitHub issue.</p>" +
          button("zip", "CREATE ZIP", !any) +
          "</div></div><div class=\"freeze-row\"><span class=\"grow "
          "muted\">Game frozen right now? Capture it before closing the "
          "game.</span>" +
          button("freeze", "Capture freeze", !state.playing) + "</div>";
      note = "Nothing is uploaded automatically.";
    } else {
      html = aboutHtml();
      el("dialog-title")->SetInnerRML("JFG RECOMP<span class=\"about-title-divider\">/</span><span class=\"about-title-section\">About</span>");
    }
    if(controllerWindow) {
      el("dialog")->SetProperty("width","100%");el("dialog")->SetProperty("height","100%");
    }
    el("dialog-body")->SetInnerRML(html);
    el("dialog-body")->SetScrollTop(scroll);
    label("dialog-note", note);
    if (!focusId.empty())
      if (auto *focus = el(focusId.c_str()))
        focus->Focus(true);
    if (page == "video") {
      const auto &v = state.video;
      const char *selected[] = {
          !state.fullscreen ? "action-window-windowed"
          : v.fullscreen == jfg::FullscreenMode::exclusive ? "action-window-exclusive"
                                                           : "action-window-borderless",
          v.aspect == jfg::AspectMode::widescreen ? "action-aspect-widescreen" : "action-aspect-original",
          v.msaa >= 8U ? "action-msaa-8" : v.msaa >= 4U ? "action-msaa-4" : v.msaa >= 2U ? "action-msaa-2" : "action-msaa-1",
          v.vsync ? "action-vsync-on" : "action-vsync-off",
          !v.fps_counter ? "action-fps-off" : v.fps_details ? "action-fps-detailed" : "action-fps-on",
          v.n64_slowdown ? "action-slowdown-on" : "action-slowdown-off",
          v.interpolate_frames ? "action-interpolate-on" : "action-interpolate-off",
          v.interpolate_objects ? "action-objects-on" : "action-objects-off",
          v.framerate == jfg::FrameRate::sixty      ? "action-framerate-60"
          : v.framerate == jfg::FrameRate::display  ? "action-framerate-display"
          : v.framerate == jfg::FrameRate::unlocked ? "action-framerate-unlocked"
                                                    : "action-framerate-original"};
      for (const char *id : selected)
        if (auto *e = el(id))
          e->SetClass("selected", true);
    }
    if (page == "gameplay")
      for (const char *id : {gore.heads ? "action-heads-on" : "action-heads-off",
                             gore.limbs ? "action-limbs-on" : "action-limbs-off",
                             gore.blood ? "action-blood-on" : "action-blood-off"})
        if (auto *e = el(id))
          e->SetClass("selected", true);
    if (page == "controllers") {
      if(auto *e=el("action-invertx"))e->SetClass("selected", mappings[player].invert_x);
      if(auto *e=el("action-inverty"))e->SetClass("selected", mappings[player].invert_y);
      refreshController();
    }
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
    if (building)
      return;
    auto *t = event.GetTargetElement();
    while (t && !t->HasAttribute("data-action"))
      t = t->GetParentNode();
    if (event.GetType() == "mouseover") {
      if (t && t->GetId().rfind("menu-", 0) == 0 &&
          popup()) {
        auto page = t->GetAttribute<Rml::String>("data-action", "");
        if (page != current) modal(page);
      }
      return;
    }
    if (!t) {
      if (event.GetType() == "click" && popup()) close();
      return;
    }
    if (t->HasAttribute("disabled")) return;
    auto a = t->GetAttribute<Rml::String>("data-action", "");
    const bool keyboardBinding=pcKeyboard() && (a.rfind("pc-bind",0)==0 ||
      a.rfind("pc-unbind",0)==0 || a.rfind("pc-key",0)==0 ||
      a=="pc-normal" || a=="pc-expert" || a=="pc-reset");
    const bool twinKeyboard=pcKeyboard() && jfg::pc_twin_stick(pcInput[static_cast<std::size_t>(player)]);
    if(a.rfind("pc-",0)==0 && a!="pc-experimental" && !keyboardBinding && !twinKeyboard && !pcExperimental())return;
    if (event.GetType() == "change") {
      if(a=="rumble-strength") {
        auto &c=pcInput[static_cast<std::size_t>(player)];
        c.rumble=std::clamp((event.GetParameter<int>("value",c.rumble)+5)/10*10,0,200);
        savePcInput();
        label("rumble-value",c.rumble?std::to_string(c.rumble)+"%":std::string("Off"));
        return;
      }
      if(a=="gyro-ratchet") {
        auto &c=pcInput[static_cast<std::size_t>(player)];
        c.gyro_ratchet=std::clamp(event.GetParameter<int>("value",c.gyro_ratchet),-1,jfg::kBindingCount-1);
        savePcInput();return;
      }
      if(a=="gyro-tune") {
        auto &c=pcInput[static_cast<std::size_t>(player)];
        c.gyro_sensitivity=std::clamp((event.GetParameter<int>("value",c.gyro_sensitivity)+5)/10*10,10,1000);
        savePcInput();label("gyro-value",std::to_string(c.gyro_sensitivity));return;
      }
      if(a.rfind("pc-key",0)==0) {
        const auto i=static_cast<std::size_t>(std::stoi(a.substr(6)));
        if(i<pcInput[0].keys.size()){pcInput[static_cast<std::size_t>(player)].keys[i]=std::clamp(event.GetParameter<int>("value",0),0,257);savePcInput();}
        return;
      }
      if(a.rfind("pc-tune",0)==0 || a.rfind("twin-tune",0)==0) {
        const int i=std::stoi(a.substr(a[0]=='t'?9:7));auto &c=pcInput[static_cast<std::size_t>(player)];
        if(i<0 || i>7)return;
        int* values[]={&c.mouse_sensitivity,&c.aim_sensitivity,&c.aim_deadzone,&c.mouse_aim_sensitivity,&c.camera_sensitivity,&c.mouse_vertical_sensitivity,&c.stick_vertical_sensitivity,&c.key_aim_speed};
        int* value=values[i];
        *value=std::clamp(event.GetParameter<int>("value",*value),i==2?0:i==7?30:10,i==2?30000:(i==0 || i==3)?500:i==7?360:300);
        savePcInput();label(("pc-value"+std::to_string(i)).c_str(),std::to_string(*value));return;
      }
      if (a.rfind("tune-", 0) == 0) {
        auto key = a.substr(5);
        auto &m = mappings[player];
        int *value = key == "deadzone"    ? &m.deadzone
                     : key == "threshold" ? &m.threshold
                                          : &m.trigger;
        *value = std::clamp(event.GetParameter<int>("value", *value),
                            key == "deadzone" ? 0 : 1000,
                            key == "deadzone" ? 30000 : 32000);
        saveController();
        label(("value-" + key).c_str(),
              std::to_string((*value * 100 + 16383) / 32767) + "%");
        return;
      }
      if (current == "video" && videoChange(a, event))
        return;
      if (a == "volume") {
        volume = std::clamp(event.GetParameter<int>("value", volume), 0, 100);
        saveAudio();
        label("volume-value", muted ? "MUTED" : std::to_string(volume) + "%");
        return;
      }
      if (a == "session") {
        selectedSession = event.GetParameter<Rml::String>("value", "");
        return;
      }
      if(a=="input-type") {
        const int type=event.GetParameter<int>("value",0);
        if(type==0 || type==-3 || type==-1){pickerType=type;normalizePicker();modal("controllers");}
        return;
      }
      if(a=="device") {
        const int d=event.GetParameter<int>("value",-2);
        if(d>=0 && deviceAvailable(d))pickerDevice=d;
        return;
      }
      return;
    }
    if (current == "video" && videoAction(a))
      return;
    if (current == "gameplay" && goreAction(a))
      return;
    if (a == "window-windowed" || a == "window-borderless" || a == "window-exclusive") {
      // The frontend owns the window; it saves the fullscreen kind and
      // reports the new state back through FrontendUiState.
      action(a);
      return;
    }
    if(a=="device-open") {
      learning=keyLearning=-1;discoverDevices();devicePicker=true;
      pickerType=pcKeyboard()?-3:0;pickerDevice=devices[player];normalizePicker();
      modal("controllers");el("dialog-body")->SetScrollTop(0);return;
    }
    if(a=="device-refresh"){discoverDevices();normalizePicker();modal("controllers");return;}
    if(a=="device-cancel"){devicePicker=false;modal("controllers");return;}
    if(a=="device-apply"){if(devicePicker && (pickerType!=0 || pickerDevice>=0))assignDevice(pickerDevice);return;}
    if(a=="device-remove"){assignDevice(-2);return;}
    if(a=="controller-settings"){controllerSettings=!controllerSettings;modal("controllers");return;}
    if(a=="gyro" || a=="gyro-mode" || a=="gyro-invert") {
      auto &c=pcInput[static_cast<std::size_t>(player)];
      if(a=="gyro")c.gyro=!c.gyro;
      else if(a=="gyro-mode")c.gyro_mode=!c.gyro_mode;
      else c.gyro_invert_y=!c.gyro_invert_y;
      savePcInput();modal("controllers");return;
    }
    if(a=="dual-input" && player==0) {
      pcInput[0].dual_input=!pcInput[0].dual_input;
      savePcInput();modal("controllers");return;
    }
    if(a=="rumble-test") {
      const int d=devices[player];
      const float amplitude=std::min(1.0F,0.8F*static_cast<float>(pcInput[static_cast<std::size_t>(player)].rumble)/100.0F);
      if(d>=0 && amplitude>0.0F)gamepads.rumble(d,amplitude,amplitude*0.6F,500U);
      return;
    }
    if(a.rfind("pc-bind",0)==0 && pcKeyboard()) {
      const int index=std::stoi(a.substr(7));if(index<0 || index>=static_cast<int>(pcInput[0].keys.size()))return;
      learning=-1;keyLearning=index;keyReleased=false;
      keyDeadline=Rml::GetSystemInterface()->GetElapsedTime()+12;
      modal("controllers");label(("action-pc-bind"+std::to_string(index)).c_str(),"Press an input...");
      label("dialog-note","Release keys and mouse buttons first. Esc cancels.");return;
    }
    if(a.rfind("pc-unbind",0)==0 && pcKeyboard()) {
      const int index=std::stoi(a.substr(9));if(index<0 || index>=static_cast<int>(pcInput[0].keys.size()))return;
      keyLearning=-1;pcInput[static_cast<std::size_t>(player)].keys[static_cast<std::size_t>(index)]=0;
      savePcInput();modal("controllers");return;
    }
    if(a=="game-controls") {
      auto &c=pcInput[static_cast<std::size_t>(player)];
      c.game_controls=!c.game_controls;savePcInput();modal("controllers");return;
    }
    // RGO twin-stick scheme (no Experimental switch needed).
    if(a.rfind("twin-",0)==0 && a.rfind("twin-tune",0)!=0) {
      auto &c=pcInput[static_cast<std::size_t>(player)];
      auto &m=mappings[player];
      if(a=="twin-scheme" && pcKeyboard()) {
        // Modern for the mouse and keyboard: the preset's mouse buttons
        // (left fire, right aim) unless keys are already bound that way.
        c.scheme=jfg::pc_twin_stick(c)?jfg::kPcSchemeOriginal:jfg::kPcSchemeTwinStick;
        if(c.scheme && c.keys[9]!=2){const auto preset=jfg::pc_keyboard_preset(false);c.keys=preset.keys;}
      }
      else if(a=="twin-scheme") {
        c.scheme=jfg::pc_twin_stick(c)?jfg::kPcSchemeOriginal:jfg::kPcSchemeTwinStick;
        // The right stick looks in the modern scheme, so its buttons move.
        const int device=m.device,stick=m.stick;
        if(c.scheme)m=jfg::pc_twin_stick_mapping(false,device);
        else {m=jfg::ControllerMapping{};m.device=device;}
        m.stick=stick;saveController();
      }
      else if(a=="twin-layout-normal" || a=="twin-layout-expert") {
        const int stick=m.stick;
        m=jfg::pc_twin_stick_mapping(a=="twin-layout-expert",m.device);m.stick=stick;saveController();
        c.game_controls=a=="twin-layout-expert";
      }
      else if(a=="twin-curve")c.aim_curve=(c.aim_curve+1)%3;
      else if(a=="twin-invert-camera")c.camera_invert_y=!c.camera_invert_y;
      else if(a=="twin-invert-aim")c.aim_invert_y=!c.aim_invert_y;
      else return;
      savePcInput();modal("controllers");return;
    }
    if(a=="pc-experimental") {
      if(writeFile("experimental-controls.ini",jfg::serialize_pc_experiments(!pcExperimental())))
        modal("controllers");
      return;
    }
    if(a.rfind("pc-",0)==0 && !keyboardBinding && !twinKeyboard && !pcExperimental())return;
    if(a.rfind("pc-key",0)==0 || a.rfind("pc-tune",0)==0)return;
    if(a=="pc-input") {if(controllerWindow)modal("controllers");else {close();action("controllers");}return;}
    if(a.rfind("pc-player",0)==0) {player=std::clamp(a.back()-'0',0,3);loadPcInput();modal("pc-input");return;}
    if(a.rfind("pc-",0)==0) {
      auto &c=pcInput[static_cast<std::size_t>(player)];
      if(a=="pc-pad-normal" || a=="pc-pad-expert") {
        const int device=mappings[player].device;mappings[player]=jfg::ControllerMapping{};mappings[player].device=device;
        auto &m=mappings[player];m.bindings[0]=a=="pc-pad-expert"?0:2;m.bindings[10]=a=="pc-pad-expert"?2:0;
        m.bindings[2]=25;m.bindings[9]=23;m.bindings[11]=7;m.bindings[12]=-1;m.bindings[13]=-1;
        c.modern=1;c.dual_stick=1;c.game_controls=a=="pc-pad-expert";saveController();
      }
      else if(a=="pc-normal" || a=="pc-expert" || (a=="pc-reset" && pcKeyboard())) {
        // RGO: in the Modern scheme a reset keeps the mouse buttons (left
        // fire, right aim); presets and resets keep the controller's
        // settings, the scheme, gyro and dual input.
        auto next=a!="pc-reset"?jfg::pc_keyboard_preset(a=="pc-expert"):
          jfg::pc_twin_stick(c)?jfg::pc_keyboard_preset(false):jfg::PcInputConfig{};
        next.dual_stick=c.dual_stick;next.aim_sensitivity=c.aim_sensitivity;
        next.aim_deadzone=c.aim_deadzone;next.aim_invert_y=c.aim_invert_y;
        next.camera_sensitivity=c.camera_sensitivity;next.aim_curve=c.aim_curve;next.stick_vertical_sensitivity=c.stick_vertical_sensitivity;
        next.rumble=c.rumble;next.scheme=c.scheme;next.camera_invert_y=c.camera_invert_y;next.dual_input=c.dual_input;
        next.gyro=c.gyro;next.gyro_sensitivity=c.gyro_sensitivity;next.gyro_mode=c.gyro_mode;
        next.gyro_ratchet=c.gyro_ratchet;next.gyro_invert_y=c.gyro_invert_y;
        if(a!="pc-reset"){next.aim_toggle=c.aim_toggle;next.key_aim_speed=c.key_aim_speed;}
        next.game_controls=a=="pc-reset"?c.game_controls:a=="pc-expert";
        c=next;
      }
      else if(a=="pc-reset"){c.dual_stick=0;c.aim_sensitivity=100;c.camera_sensitivity=100;c.aim_curve=0;c.stick_vertical_sensitivity=100;c.aim_deadzone=7849;c.aim_invert_y=0;c.modern=0;}
      else if(a=="pc-curve")c.aim_curve=(c.aim_curve+1)%3;
      else if(a=="pc-modern")c.modern=!c.modern;
      else if(a=="pc-mouse")c.mouse_aim=!c.mouse_aim;
      else if(a=="pc-dual")c.dual_stick=!c.dual_stick;
      else if(a=="pc-invert-mouse")c.mouse_invert_y=!c.mouse_invert_y;
      else if(a=="pc-aim-toggle")c.aim_toggle=!c.aim_toggle;
      else if(a=="pc-invert-stick")c.aim_invert_y=!c.aim_invert_y;
      else return;
      savePcInput();modal("controllers");return;
    }
    if (a == "device" || a == "input-type" || a == "session" || a == "volume" ||
        a == "rumble-strength" || a == "gyro-tune" || a == "gyro-ratchet" || a.rfind("tune-", 0) == 0 ||
        a.rfind("twin-tune", 0) == 0)
      return;
    if (a.rfind("report-", 0) == 0) {
      selectedSession = a.substr(7);
      modal("support");
      return;
    }
    if (a == "windowed" || a == "borderless") {
      if ((a == "borderless") != state.fullscreen)
        action("fullscreen");
      return;
    }
    if (a.rfind("menu-", 0) == 0) {
      if (current == a) close(); else modal(a);
      return;
    }
    if (a == "zip") {
      if (writeFile("frontend-report.txt", selectedSession))
        action("export");
      return;
    }
    if (a == "freeze") {
      writeFile("frontend-capture.request", "1");
      label("dialog-note",
            "Capture requested. Leave the game open while diagnostics finish.");
      return;
    }
    if (a == "refresh-sessions") {
      action("sessions");
      return;
    }
    if (a == "close") {
      close();
      return;
    }
    if (a == "mute") {
      toggleMute();
      return;
    }
    if (a == "up" || a == "down") {
      volume = std::clamp(volume + (a == "up" ? 5 : -5), 0, 100);
      saveAudio();
      modal("audio");
      return;
    }
    if (a.rfind("player", 0) == 0) {
      learning = -1;
      player = std::clamp(a.back() - '0', 0, 3);
      keyLearning=-1;devicePicker=false;controllerSettings=false;
      modal("controllers");
      el("dialog-body")->SetScrollTop(0);
      return;
    }
    if (a.rfind("learn", 0) == 0) {
      const int next = std::stoi(a.substr(5));
      if (learning == next) {
        learning = -1;
        modal("controllers");
        return;
      }
      if (learning >= 0)
        modal("controllers");
      learning = next;
      el(("action-learn" + std::to_string(learning)).c_str())
          ->SetInnerRML("Cancel");
      el(("binding-" + std::to_string(learning)).c_str())
          ->SetClass("selected", true);
      learnReleased = false;
      learnDeadline = Rml::GetSystemInterface()->GetElapsedTime() + 12;
      label("dialog-note", "Release all controls first...");
      return;
    }
    if (a.rfind("clear", 0) == 0) {
      learning = -1;
      mappings[player].bindings[std::stoi(a.substr(5))] = -1;
      saveController();
      modal("controllers");
      return;
    }
    auto &m = mappings[player];
    bool changed = true;
    if (a == "stick")
      m.stick = !m.stick;
    else if (a == "invertx")
      m.invert_x = !m.invert_x;
    else if (a == "inverty")
      m.invert_y = !m.invert_y;
    else if (a == "reset") {
      int d = m.device;
      m = jfg::pc_twin_stick(pcInput[static_cast<std::size_t>(player)]) ? jfg::pc_twin_stick_mapping(false, d) : jfg::ControllerMapping{};
      m.device = d;
    } else if (a.rfind("deadzone", 0) == 0)
      m.deadzone =
          std::clamp(m.deadzone + (a.back() == '+' ? 1000 : -1000), 0, 30000);
    else if (a.rfind("threshold", 0) == 0)
      m.threshold = std::clamp(m.threshold + (a.back() == '+' ? 1000 : -1000),
                               1000, 32000);
    else if (a.rfind("trigger", 0) == 0)
      m.trigger =
          std::clamp(m.trigger + (a.back() == '+' ? 1000 : -1000), 1000, 32000);
    else
      changed = false;
    if (changed) {
      learning = -1;
      saveController();
      modal("controllers");
      return;
    }
    if (a == "audio") {
      loadAudio();
      modal(a);
    } else if (a == "gameplay") {
      loadGore();
      modal(a);
    } else if (a == "controllers") {
      if(controllerWindow){loadControllers();modal(a);}
      else {close();action("controllers");}
    } else if (a == "support") {
      action("sessions");
      modal(a);
    } else if (a == "video" || a == "setup" || a == "game" || a == "tools" ||
               a == "help")
      modal(a);
    else {
      if (a == "pause" || a == "resume" || a == "play" || a == "stop" || a == "map" || a == "inventory" || a == "saves" || a == "quit" || a == "guide" || a == "rom" || a == "fullscreen" || a == "shortcut")
        close();
      action(a);
    }
  }
};

namespace {
using Microsoft::WRL::ComPtr;
struct MemoryFile {
  const unsigned char *data;
  size_t size, position = 0;
  std::vector<unsigned char> owned;
};
struct Resources : Rml::FileInterface {
  fs::path liveAssets;
  Rml::FileHandle Open(const Rml::String &path) override {
    struct Entry {
      const char *name;
      int id;
    };
    constexpr Entry entries[] = {{"main.rml", 201},
                                 {"theme.rcss", 202},
                                 {"Barlow-Regular.ttf", 203},
                                 {"Barlow-SemiBold.ttf", 204},
                                 {"ChakraPetch-Bold.ttf", 205}, {"live-map.rml", 206}, {"live-inventory.rml", 207}, {"live-tools.rcss", 208}, {"about.rml", 209}, {"about.rcss", 210}};
    for (auto &e : entries)
      if (fs::path(path).filename() == e.name) {
        auto r = FindResourceW(nullptr, MAKEINTRESOURCEW(e.id), RT_RCDATA);
        if (!r)
          return 0;
        auto p = LockResource(LoadResource(nullptr, r));
        if (!p)
          return 0;
        return reinterpret_cast<Rml::FileHandle>(new MemoryFile{
            static_cast<const unsigned char *>(p), SizeofResource(nullptr, r)});
      }
    if (!liveAssets.empty() && path.rfind("rom-asset/", 0) == 0) {
      auto name=path.substr(10);
      if (name.find_first_of("/\\:") != std::string::npos || name.rfind("asset-",0)!=0 || !name.ends_with(".tga")) return 0;
      std::ifstream file(liveAssets/name,std::ios::binary|std::ios::ate);
      if(!file || file.tellg()<18 || file.tellg()>128*128*4+18)return 0;
      auto memory=std::make_unique<MemoryFile>();memory->size=static_cast<size_t>(file.tellg());memory->owned.resize(memory->size);file.seekg(0);file.read(reinterpret_cast<char*>(memory->owned.data()),memory->size);
      if(!file)return 0;
      auto& b=memory->owned;unsigned width=b[12]+256u*b[13],height=b[14]+256u*b[15];
      if(b[0]!=0 || b[2]!=2 || b[16]!=32 || width==0 || height==0 || width>128 || height>128 || memory->size!=18+width*height*4)return 0;
      memory->data=memory->owned.data();return reinterpret_cast<Rml::FileHandle>(memory.release());
    }
    return 0;
  }
  void Close(Rml::FileHandle f) override {
    delete reinterpret_cast<MemoryFile *>(f);
  }
  size_t Read(void *b, size_t n, Rml::FileHandle f) override {
    auto &m = *reinterpret_cast<MemoryFile *>(f);
    n = std::min(n, m.size - m.position);
    memcpy(b, m.data + m.position, n);
    m.position += n;
    return n;
  }
  bool Seek(Rml::FileHandle f, long off, int origin) override {
    auto &m = *reinterpret_cast<MemoryFile *>(f);
    auto p = (origin == SEEK_SET   ? 0LL
              : origin == SEEK_CUR ? static_cast<long long>(m.position)
                                   : static_cast<long long>(m.size)) +
             off;
    if (p < 0 || p > static_cast<long long>(m.size))
      return false;
    m.position = size_t(p);
    return true;
  }
  size_t Tell(Rml::FileHandle f) override {
    return reinterpret_cast<MemoryFile *>(f)->position;
  }
};
struct Surface {
  HWND window = nullptr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> gpu;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<IDCompositionDevice> composition;
  ComPtr<IDCompositionTarget> compositionTarget;
  ComPtr<IDCompositionVisual> visual;
  SystemInterface_Win32 platform;
  TextInputMethodEditor_Win32 ime;
  Resources files;
  Rml::ElementInstancerGeneric<HomeScene> homeInstancer;
  Rml::ElementInstancerGeneric<jfg_live::MapScene> mapInstancer;
  std::unique_ptr<jfg_live::ToolUi> live;
  std::unique_ptr<LogSystem> log;
  std::unique_ptr<RenderInterface_DX11> renderer;
  Rml::Context *context = nullptr;
  Ui ui;
  bool initialized = false;
  int width = 0, height = 0;
  ~Surface() {
    if (initialized) {
      if (ui.doc) {
        ui.doc->RemoveEventListener("click", &ui);
        ui.doc->RemoveEventListener("change", &ui);
      }
      Rml::Shutdown();
    }
  }
};
std::unique_ptr<Surface> surface;
void check(HRESULT r) {
  if (FAILED(r))
    throw std::runtime_error("UI graphics error " +
                             std::to_string(static_cast<unsigned long>(r)));
}
} // namespace
bool FrontendUiInit(HWND w, const fs::path &profile,
                    std::function<void(const std::string &)> action, const std::string &tool) {
  try {
    surface = std::make_unique<Surface>();
    auto &s = *surface;
    s.window = w;
    const UINT iconDpi = GetDpiForWindow(w);
    const auto module = GetModuleHandleW(nullptr);
    for (const bool smallIcon : {false, true}) {
      const auto icon = LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON,
          GetSystemMetricsForDpi(smallIcon ? SM_CXSMICON : SM_CXICON, iconDpi),
          GetSystemMetricsForDpi(smallIcon ? SM_CYSMICON : SM_CYICON, iconDpi), LR_SHARED);
      if (icon)
        SendMessageW(w, WM_SETICON, smallIcon ? ICON_SMALL : ICON_BIG,
                     reinterpret_cast<LPARAM>(icon));
    }
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &s.device, nullptr, &s.gpu));
    ComPtr<IDXGIDevice> dxgi;
    check(s.device.As(&dxgi));
    ComPtr<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory;
    check(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    check(factory->CreateSwapChainForComposition(s.device.Get(), &desc, nullptr,
                                                 &s.swap));
    check(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&s.composition)));
    check(s.composition->CreateTargetForHwnd(w, TRUE, &s.compositionTarget));
    check(s.composition->CreateVisual(&s.visual));
    check(s.visual->SetContent(s.swap.Get()));
    check(s.compositionTarget->SetRoot(s.visual.Get()));
    check(s.composition->Commit());
    s.platform.SetWindow(w);
    s.log = std::make_unique<LogSystem>(&s.platform);
    s.log->file.close();
    s.log->file.open(profile / (tool.empty()?"frontend-ui.log":"live-"+tool+"-ui.log"), std::ios::trunc);
    s.renderer = std::make_unique<RenderInterface_DX11>(s.device.Get());
    Rml::SetSystemInterface(s.log.get());
    Rml::SetRenderInterface(s.renderer.get());
    Rml::SetFileInterface(&s.files);
    Rml::SetTextInputHandler(&s.ime);
    if (!Rml::Initialise())
      throw std::runtime_error("Cannot initialize RmlUi");
    s.initialized = true;
    Rml::Factory::RegisterElementInstancer("home-scene", &s.homeInstancer);
    Rml::Factory::RegisterElementInstancer("map-scene", &s.mapInstancer);
    for (const char *font :
         {"Barlow-Regular.ttf", "Barlow-SemiBold.ttf", "ChakraPetch-Bold.ttf"})
      if (!Rml::LoadFontFace(font))
        throw std::runtime_error("Cannot load UI font");
    s.context = Rml::CreateContext("frontend", {1, 1});
    s.ui.window = w;
    s.ui.profile = profile;
    s.ui.action = std::move(action);
    s.ui.controllerWindow=tool=="controllers";
    s.ui.doc = s.context->LoadDocument(tool.empty() || s.ui.controllerWindow?"main.rml":tool=="map"?"live-map.rml":"live-inventory.rml");
    if (!s.ui.doc)
      throw std::runtime_error("Cannot load UI document");
    if(!tool.empty() && !s.ui.controllerWindow) {
      s.files.liveAssets=profile/("live-tool-"+std::to_string(reinterpret_cast<std::uintptr_t>(w))+"-"+std::to_string(reinterpret_cast<std::uintptr_t>(GetPropW(w,L"JfgLiveToken"))));
      std::error_code error;fs::create_directories(s.files.liveAssets,error);
      s.live=std::make_unique<jfg_live::ToolUi>();s.live->init(w,s.ui.doc,s.files.liveAssets,tool,s.ui.action);
      s.ui.doc->Show();FrontendUiResize();return true;
    }
    s.ui.doc->AddEventListener("click", &s.ui);
    s.ui.doc->AddEventListener("mouseover", &s.ui);
    s.ui.doc->AddEventListener("change", &s.ui);
    s.ui.doc->Show();
    s.ui.loadAudio();
    s.ui.loadControllers();
    s.ui.doc->SetClass("controller-window",s.ui.controllerWindow);
    if(s.ui.controllerWindow)s.ui.modal("controllers");
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    s.ui.reducedMotion = !animations;
    s.ui.doc->SetClass("reduced-motion", s.ui.reducedMotion);
    FrontendUiResize();
    return true;
  } catch (const std::exception &e) {
    MessageBoxW(w, RmlWin32::ConvertToUTF16(e.what()).c_str(),
                L"Cannot start interface", MB_OK | MB_ICONERROR);
    surface.reset();
    return false;
  }
}
void FrontendUiShutdown() { surface.reset(); }
void FrontendUiResize() {
  if (!surface || !surface->context)
    return;
  auto &s = *surface;
  RECT r{};
  GetClientRect(s.window, &r);
  int w = std::max(1L, r.right), h = std::max(1L, r.bottom);
  s.context->SetDensityIndependentPixelRatio(GetDpiForWindow(s.window) / 96.f);
  if (w == s.width && h == s.height)
    return;
  s.width = w;
  s.height = h;
  s.gpu->OMSetRenderTargets(0, nullptr, nullptr);
  s.target.Reset();
  check(s.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0));
  ComPtr<ID3D11Texture2D> buffer;
  check(s.swap->GetBuffer(0, IID_PPV_ARGS(&buffer)));
  check(s.device->CreateRenderTargetView(buffer.Get(), nullptr, &s.target));
  s.context->SetDimensions({w, h});
  s.renderer->SetViewport(w, h);
}
void FrontendUiFrame(const FrontendUiState &state) {
  if (!surface || !surface->context || IsIconic(surface->window))
    return;
  auto &s = *surface;
  auto &ui = s.ui;
  if (s.live) {
    s.live->tick();s.context->Update();const float clear[]={0,0,0,0};s.gpu->ClearRenderTargetView(s.target.Get(),clear);s.renderer->BeginFrame();s.context->Render();s.renderer->EndFrame(s.target.Get());s.swap->Present(1,0);return;
  }
  const bool paused = state.playing && GetPropW(s.window, L"JfgFrontendPaused") != nullptr;
  const bool pauseChanged = ui.doc->IsClassSet("paused") != paused;
  const bool refreshPanel =
      (pauseChanged && ui.current == "menu-game") ||
      (state.mods != ui.state.mods || state.busy != ui.state.busy ||
       state.playing != ui.state.playing) &&
          (ui.current == "menu-game" || ui.current == "setup") ||
      (state.fullscreen != ui.state.fullscreen && (ui.current == "video" || ui.current == "menu-video")) ||
      (state.pauseInactive != ui.state.pauseInactive && ui.current == "video") ||
      (!(state.video == ui.state.video) && ui.current == "video") ||
      (ui.current == "help" && (state.rom != ui.state.rom || state.playing != ui.state.playing));
  if (state.playing != ui.state.playing) {
    if (state.playing) {
      ui.assignments = jfg::ControllerPorts{};
      ui.loadControllers();
    }
    ui.doc->SetClass("playing", state.playing);
    if (state.playing)
      for (int p = 0; p < 4; p++)
        if (ui.devices[p] >= 0 && ui.connected[ui.devices[p]])
          ui.toast("Controller " + std::to_string(ui.devices[p] + 1) +
                   " connected - Player " + std::to_string(p + 1));
  }
  if (state.fullscreen != ui.state.fullscreen)
    ui.doc->SetClass("fullscreen", state.fullscreen);
  if (state.rom != ui.state.rom) {
    ui.action("assets");
    fs::path p(state.rom);
    ui.label("rom-name", state.rom.empty() ? "Choose your game ROM"
                                           : u8(p.filename().wstring()));
    ui.label("rom-folder", state.rom.empty() ? "Required to set up and play."
                                             : u8(p.parent_path().wstring()));
    ui.label("rom-badge", state.rom.empty() ? "" : "SELECTED");
  }
  if (state.status != ui.state.status) {
    ui.status(u8(state.status));
    if (auto *e = ui.el("setup-status"))
      e->SetInnerRML("<span>" + escape(u8(state.status)) + "</span>");
  }
  ui.doc->SetClass("paused", paused);
  // RGO FPS counter, reported by the game twice a second.
  if (auto *fps = ui.el("fps-counter")) {
    const bool show = state.playing && state.video.fps_counter && state.frameStatsValid;
    fps->SetClass("visible", show);
    // Game: logic updates per second, the rate the game plays at. Render:
    // real frames drawn. Display: frames shown, blended ones included.
    if (show && (!ui.state.frameStatsValid || state.frameCounter != ui.state.frameCounter ||
                 state.frameStats != ui.state.frameStats ||
                 state.video.fps_details != ui.state.video.fps_details)) {
      std::string rml = escape(jfg::format_frame_counter(state.frameCounter));
      if (state.video.fps_details) {
        // Per second unless marked ms (frame times: shortest-longest gap).
        const auto &d = state.frameStats;
        auto t = [](std::uint32_t tenths) {
          return std::to_string(tenths / 10U) + "." + std::to_string(tenths % 10U);
        };
        rml += "<br/>logic " + t(d.logic_tenths) + " | ticks " + t(d.ticks_tenths) +
               "<br/>drawn " + t(d.drawn_tenths) + " | new " + t(d.new_tenths) +
               " | camera " + t(d.camera_tenths) + " | repeats " + t(d.repeat_tenths) +
               "<br/>input " + t(d.input_tenths) +
               "<br/>game frame " + t(d.frame_min) + "-" + t(d.frame_max) + " ms" +
               "<br/>display frame " + t(d.display_min) + "-" + t(d.display_max) + " ms";
        // Host time per frame, graphics tasks and the True 60 governor.
        std::string host = escape(jfg::format_frame_host(d));
        for (std::size_t at = host.find('\n'); at != std::string::npos; at = host.find('\n', at))
          host.replace(at, 1U, "<br/>");
        rml += "<br/>" + host;
      }
      fps->SetInnerRML(rml);
    }
  }
  const bool viewChanged = state.viewWidth != ui.state.viewWidth || state.viewHeight != ui.state.viewHeight;
  ui.state = state;
  // The render size note follows the game area without rebuilding the page.
  if (viewChanged && ui.current == "video")
    if (auto *note = ui.el("render-note"))
      note->SetInnerRML(escape(ui.renderSizeNote(state.video)));
  if (refreshPanel)
    ui.modal(ui.current);
  if (ui.current == "support") {
    auto list = ui.read("frontend-sessions.tsv");
    if (list != ui.sessionList) {
      ui.sessionList = list;
      ui.modal("support");
    }
  }
  if (!state.playing) {
    auto* scene = static_cast<HomeScene*>(ui.el("home-scene"));
    scene->reducedMotion = ui.reducedMotion;
    scene->ships.Source(ui.profile / "frontend-ships.bin", u8(state.rom));
    const double now = Rml::GetSystemInterface()->GetElapsedTime();
    if (ui.decorationTick < 0 || (!ui.reducedMotion && now - ui.decorationTick >= .14)) {
      ui.decorationTick = now;
      const double t = ui.reducedMotion ? 0 : now;
      std::ostringstream readout;
      readout << std::fixed << std::setprecision(2) << "X +" << 482.19 + t * 2.64
              << "   Y " << -1137.04 - t * 1.5 << "   Z +" << 29.77 + std::sin(t*.4)*.8;
      ui.label("coordinates", readout.str());
      readout.str(""); readout << std::setprecision(1) << "HDG " << 214.6 + std::sin(t*.17)*2
              << " deg   VEL " << std::setprecision(3) << .830 + std::sin(t*.23)*.012 << "c";
      ui.label("heading", readout.str());
    }
  }
  ui.ports();
  s.context->Update();
  const float clear[] = {0, 0, 0, 0};
  s.gpu->ClearRenderTargetView(s.target.Get(), clear);
  s.renderer->BeginFrame();
  s.context->Render();
  s.renderer->EndFrame(s.target.Get());
  s.swap->Present(1, 0);
}
void FrontendUiMessage(UINT m, WPARAM w, LPARAM l) {
  if(surface && surface->live) {
    if(surface->live->keyboard(m,w))return;
    RmlWin32::WindowProcedure(surface->context,surface->ime,surface->window,m,w,l);return;
  }
  if (surface && surface->context) {
    auto &ui = surface->ui;
    if((m==WM_KILLFOCUS || (m==WM_ACTIVATEAPP && !w)) && ui.learning>=0) {
      ui.learning=-1;ui.modal("controllers");
    }
    if(ui.bindingMessage(m,w))return;
    if(ui.controllerWindow) {
      if(m==WM_KEYDOWN && w==VK_ESCAPE){
        if(ui.learning>=0){ui.learning=-1;ui.modal("controllers");}
        else if(ui.devicePicker){ui.devicePicker=false;ui.modal("controllers");}
        else ui.close();
        return;
      }
      if(m==WM_SYSKEYDOWN || m==WM_SYSKEYUP || (m==WM_KEYDOWN && w==VK_F10))return;
    }
    const bool popup = ui.popup();
    if (m == WM_ACTIVATEAPP && !w && popup) ui.close();
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) &&
        (w == VK_F10 || (m == WM_SYSKEYDOWN && (w == 'G' || w == 'T' || w == 'C' || w == 'V' || w == 'A' || w == 'H')))) {
      const char *page = w == 'T' ? "tools" : w == 'C' ? "controllers" : w == 'V' ? "video" : w == 'A' ? "audio" : w == 'H' ? "help" : "game";
      if (w == VK_F10 && popup) ui.close(); else ui.modal(std::string("menu-") + page);
      return;
    }
    if (m == WM_KEYDOWN && popup && (w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END)) {
      Rml::ElementList items; ui.el("game-popup")->QuerySelectorAll(items,"button");
      items.erase(std::remove_if(items.begin(),items.end(),[](Rml::Element *e){return e->HasAttribute("disabled");}),items.end());
      if (!items.empty()) {
        auto it=std::find(items.begin(),items.end(),surface->context->GetFocusElement());
        int i=it==items.end() ? (w==VK_UP?0:-1) : int(it-items.begin());
        i=w==VK_HOME?0:w==VK_END?int(items.size())-1:(i+(w==VK_UP?-1:1)+int(items.size()))%int(items.size());
        items[i]->Focus(true);
      }
      return;
    }
    if (m == WM_KEYDOWN && popup && (w == VK_LEFT || w == VK_RIGHT)) {
      constexpr const char *menus[] = {"menu-game", "menu-controllers", "menu-video", "menu-audio", "menu-tools", "menu-help"};
      int index = 0; while (index < 5 && ui.current != menus[index]) ++index;
      ui.modal(menus[(index + (w == VK_RIGHT ? 1 : 5)) % 6]); return;
    }
    if (m == WM_KEYDOWN && popup && w == VK_RETURN) {
      auto *focused=surface->context->GetFocusElement();
      if(focused && focused->GetId().rfind("action-",0)==0 && !focused->HasAttribute("disabled")) focused->Click();
      return;
    }
  }
  if (surface && surface->context && m == WM_KEYDOWN && w == VK_TAB &&
      !surface->ui.current.empty()) {
    auto &ui = surface->ui;
    auto *root =
        ui.el(ui.popup() ? "game-popup"
                                                            : "dialog");
    Rml::ElementList controls;
    root->QuerySelectorAll(controls, "button, input, select");
    controls.erase(std::remove_if(controls.begin(), controls.end(),
                                  [](Rml::Element *e) {
                                    return !e->IsVisible(true) ||
                                           e->HasAttribute("disabled");
                                  }),
                   controls.end());
    if (!controls.empty()) {
      auto it = std::find(controls.begin(), controls.end(),
                          surface->context->GetFocusElement());
      const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      int i =
          it == controls.end() ? (back ? 0 : -1) : int(it - controls.begin());
      i = (i + (back ? -1 : 1) + int(controls.size())) % int(controls.size());
      controls[i]->Focus(true);
      controls[i]->ScrollIntoView(false);
    }
    return;
  }
  if (surface && surface->context)
    RmlWin32::WindowProcedure(surface->context, surface->ime, surface->window,
                              m, w, l);
}
void FrontendUiPanel(const std::string &name) {
  if (surface) {
    if (name.empty()) {
      if (surface->ui.learning >= 0) {
        surface->ui.learning = -1;
        surface->ui.modal("controllers");
      } else
        surface->ui.close();
    } else
      surface->ui.modal(name == "game" ? "menu-game" : name);
  }
}
void FrontendUiToggleMute() { if (surface && !surface->live) surface->ui.toggleMute(); }
bool FrontendUiCapturing() { return surface && !surface->ui.current.empty(); }

bool FrontendUiModalOpen() { return surface && !surface->ui.current.empty() && !surface->ui.popup(); }
