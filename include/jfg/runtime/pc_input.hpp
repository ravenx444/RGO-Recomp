#pragma once
#include "jfg/runtime/controller_ports.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <bit>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

namespace jfg {
// Windows virtual keys, with two portable binding IDs for wheel pulses.
inline constexpr int kPcWheelUp = 256, kPcWheelDown = 257;
struct PcInputConfig {
  int modern = 0;
  int mouse_aim = 0, mouse_sensitivity = 100, mouse_invert_y = 0;
  int dual_stick = 0, aim_sensitivity = 100, aim_deadzone = 7849, aim_invert_y = 0;
  int mouse_aim_sensitivity = 100, camera_sensitivity = 100, aim_curve = 0;
  int mouse_vertical_sensitivity = 100, stick_vertical_sensitivity = 100;
  // RGO (version 4) extension settings; see kPcExtensions.
  int rumble = 100;
  int aim_toggle = 0, key_aim_speed = 120; // key_aim_speed in degrees per second
  // A, B, Z, Start, D-pad, L, R, C-buttons, movement, fast movement, alternate A,
  // and (RGO, version 4) aim up/down/left/right: Numpad 8/2/4/6.
  std::array<int,24> keys{32,'X','C',13,38,40,37,39,'Q','E','I','K','J','L',
                           'W','S','A','D',16,'Z',0x68,0x62,0x64,0x66};
};
// Keys 0..19 exist in every version; later keys are optional (version 4).
inline constexpr std::size_t kPcLegacyKeys = 20;
// This separate profile-wide opt-in keeps old per-player profiles disabled.
inline bool pc_experiments_enabled(std::string_view text) {
  return text == "version=1\nexperimental=1\n" ||
         text == "version=1\r\nexperimental=1\r\n";
}
inline std::string serialize_pc_experiments(bool enabled) {
  return enabled ? "version=1\nexperimental=1\n" : "version=1\nexperimental=0\n";
}
inline PcInputConfig effective_pc_input(bool enabled,const PcInputConfig &saved) {
  auto active=saved;
  if(!enabled)active.modern=active.mouse_aim=active.dual_stick=0;
  return active;
}
inline constexpr std::array<std::string_view,14> kPcSettings{
  "version","mouse_aim","mouse_sensitivity","mouse_invert_y","dual_stick",
  "aim_sensitivity","aim_deadzone","aim_invert_y","modern",
  "mouse_aim_sensitivity","camera_sensitivity","aim_curve",
  "mouse_vertical_sensitivity","stick_vertical_sensitivity"};
// RGO: version 4 adds these optional settings. A missing one keeps its default,
// so files written before a setting existed still load.
struct PcExtension { std::string_view name; int minimum, maximum; int PcInputConfig::*field; };
inline constexpr std::array<PcExtension,3> kPcExtensions{{
  {"rumble",0,200,&PcInputConfig::rumble},
  {"aim_toggle",0,1,&PcInputConfig::aim_toggle},
  {"key_aim_speed",30,360,&PcInputConfig::key_aim_speed}}};
inline std::string serialize_pc_input(const PcInputConfig &c) {
  const std::array<int,14> values{4,c.mouse_aim,c.mouse_sensitivity,c.mouse_invert_y,
    c.dual_stick,c.aim_sensitivity,c.aim_deadzone,c.aim_invert_y,c.modern,
    c.mouse_aim_sensitivity,c.camera_sensitivity,c.aim_curve,
    c.mouse_vertical_sensitivity,c.stick_vertical_sensitivity};
  std::ostringstream out;
  for (std::size_t i=0;i<values.size();++i) out<<kPcSettings[i]<<'='<<values[i]<<'\n';
  for (const auto &e:kPcExtensions) out<<e.name<<'='<<c.*e.field<<'\n';
  for (std::size_t i=0;i<c.keys.size();++i) out<<"key"<<i<<'='<<c.keys[i]<<'\n';
  return out.str();
}
inline bool parse_pc_input(std::string_view text,PcInputConfig &result) {
  if(text.empty() || text.size()>4096) return false;
  PcInputConfig c;
  constexpr std::size_t kExtensionSlot=kPcSettings.size()+std::tuple_size_v<decltype(c.keys)>;
  std::array<int,kExtensionSlot+kPcExtensions.size()> values{};
  std::array<bool,kExtensionSlot+kPcExtensions.size()> seen{};
  while(!text.empty()) {
    const auto n=text.find('\n');auto line=text.substr(0,n);
    text=n==text.npos?std::string_view{}:text.substr(n+1);
    if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
    const auto eq=line.find('=');if(eq==line.npos)return false;
    const auto key=line.substr(0,eq),value=line.substr(eq+1);
    std::size_t slot=seen.size();
    for(std::size_t i=0;i<kPcSettings.size();++i)if(key==kPcSettings[i])slot=i;
    for(std::size_t i=0;i<c.keys.size();++i)if(key=="key"+std::to_string(i))slot=i+kPcSettings.size();
    for(std::size_t i=0;i<kPcExtensions.size();++i)if(key==kPcExtensions[i].name)slot=kExtensionSlot+i;
    if(slot==seen.size() || seen[slot] || value.empty())return false;
    int number=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || value!=std::to_string(number))return false;
    seen[slot]=true;values[slot]=number;
  }
  if(values[0]<1 || values[0]>4)return false;
  for(std::size_t i=kPcLegacyKeys;i<c.keys.size();++i) {
    const auto slot=kPcSettings.size()+i;
    if(!seen[slot]){values[slot]=c.keys[i];seen[slot]=true;}
    else if(values[0]<4)return false;
  }
  for(std::size_t i=0;i<kPcExtensions.size();++i) {
    const auto &e=kPcExtensions[i];auto &value=values[kExtensionSlot+i];
    if(!seen[kExtensionSlot+i]){value=c.*e.field;seen[kExtensionSlot+i]=true;}
    else if(values[0]<4 || value<e.minimum || value>e.maximum)return false;
  }
  if(values[0]<3) {
    for(std::size_t i=9;i<kPcSettings.size();++i){if(seen[i])return false;seen[i]=true;}
    values[9]=values[2];values[10]=values[5];values[11]=0;values[12]=values[13]=100;
  }
  if(values[0]==1 && seen[8])return false;
  if(values[0]==1)seen[8]=true;
  for(bool found:seen)if(!found)return false;
  for(auto i:{1,3,4,7,8})if(values[static_cast<std::size_t>(i)]<0 || values[static_cast<std::size_t>(i)]>1)return false;
  if(values[2]<10 || values[2]>500 || values[5]<10 || values[5]>300 || values[6]<0 || values[6]>30000)return false;
  if(values[9]<10 || values[9]>500 || values[10]<10 || values[10]>300 || values[11]<0 || values[11]>2 ||
     values[12]<10 || values[12]>300 || values[13]<10 || values[13]>300)return false;
  for(std::size_t i=0;i<c.keys.size();++i) {
    if(values[i+kPcSettings.size()]<0 || values[i+kPcSettings.size()]>kPcWheelDown)return false;
    c.keys[i]=values[i+kPcSettings.size()];
  }
  c.mouse_aim=values[1];c.mouse_sensitivity=values[2];c.mouse_invert_y=values[3];
  c.dual_stick=values[4];c.aim_sensitivity=values[5];c.aim_deadzone=values[6];c.aim_invert_y=values[7];
  c.modern=values[8];c.mouse_aim_sensitivity=values[9];c.camera_sensitivity=values[10];c.aim_curve=values[11];
  c.mouse_vertical_sensitivity=values[12];c.stick_vertical_sensitivity=values[13];
  for(std::size_t i=0;i<kPcExtensions.size();++i)c.*kPcExtensions[i].field=values[kExtensionSlot+i];
  result=c;return true;
}
inline PcInputConfig pc_keyboard_preset(bool expert=false) {
  PcInputConfig c;c.modern=1;c.mouse_aim=1;c.keys[2]=1;c.keys[9]=2;
  c.keys[0]=expert?32:13;c.keys[3]=9;c.keys[10]=expert?'E':32;
  c.keys[8]=kPcWheelUp;c.keys[1]=kPcWheelDown;c.keys[19]='Z';return c;
}
template<class Down> ControllerPortSample map_pc_keyboard(const PcInputConfig &c,Down down) {
  ControllerPortSample out;out.connected=true;
  constexpr std::array<std::uint16_t,14> masks{0x8000,0x4000,0x2000,0x1000,0x800,0x400,0x200,0x100,0x20,0x10,8,4,2,1};
  const auto held=[&](std::size_t i){return c.keys[i]!=0 && down(c.keys[i]);};
  for(std::size_t i=0;i<masks.size();++i)if(held(i))out.buttons|=masks[i];
  if(held(19))out.buttons|=0x8000;
  const int speed=held(18)?127:80;
  out.x=static_cast<std::int8_t>((int(held(17))-int(held(16)))*speed);
  out.y=static_cast<std::int8_t>((int(held(14))-int(held(15)))*speed);
  return out;
}
// Menu shortcuts remain usable with either guest control scheme or custom
// gameplay bindings. Pointer position is not interpreted as a guest menu item.
template<class Down> ControllerPortSample map_pc_menu(ControllerPortSample sample,Down down) {
  if(down(13))sample.buttons|=0x8000; // Enter: confirm.
  if(down(8))sample.buttons|=0x4000;  // Backspace: back.
  const int x=int(down(39))-int(down(37)),y=int(down(38))-int(down(40));
  if(x)sample.x=static_cast<std::int8_t>(x*80);
  if(y)sample.y=static_cast<std::int8_t>(y*80);
  return sample;
}
// Keep normal mapped buttons in menus, scripted scenes and unsupported modes.
// The unused stick becomes look input only while a qualified look path owns it.
inline MappedControllerSample map_pc_controller(const ControllerMapping &mapping,
    StandardControllerSample pad,const PcInputConfig &config,bool look_active) {
  auto mapped=map_controller(mapping,pad);
  if(look_active && config.dual_stick && (config.modern || (mapped.buttons&0x10)!=0)) {
    const std::size_t axis=mapping.stick==0?2U:0U;
    pad.axes[axis]=pad.axes[axis+1U]=0;
    mapped=map_controller(mapping,pad);
  }
  return mapped;
}
inline N64StickSample pc_aim_stick(const PcInputConfig &c,const StandardControllerSample &pad,int movement_stick,bool aiming=true) {
  const std::size_t axis=movement_stick==0?2U:0U;
  auto aim=scale_xinput_left_stick(pad.axes[axis],-pad.axes[axis+1],c.aim_deadzone);
  const float magnitude=std::min(1.0F,std::hypot(float(aim.x),float(aim.y))/80.0F);
  const float response=c.aim_curve==1?magnitude:c.aim_curve==2?magnitude*magnitude:1.0F;
  const int sensitivity=aiming?c.aim_sensitivity:c.camera_sensitivity;
  aim.x=static_cast<std::int8_t>(std::clamp(int(float(aim.x)*response*float(sensitivity)/100.0F),-127,127));
  aim.y=static_cast<std::int8_t>(std::clamp(int(float(aim.y)*response*float(sensitivity)*float(c.stick_vertical_sensitivity)*(c.aim_invert_y?-1.0F:1.0F)/10000.0F),-127,127));
  return aim;
}
class PcWheelPulses {
  int remainder_=0, pending_=0;bool release_=false;
public:
  void add(int delta) { remainder_+=std::clamp(delta,-3840,3840);pending_=std::clamp(pending_+remainder_/120,-32,32);remainder_%=120; }
  int sample() {if(release_){release_=false;return 0;}if(!pending_)return 0;
    const int direction=pending_>0?1:-1;pending_-=direction;release_=true;return direction>0?kPcWheelUp:kPcWheelDown;}
  void clear(){remainder_=pending_=0;release_=false;}
};
struct PcLookDelta {
  int x=0,y=0;
  bool operator==(const PcLookDelta&) const = default;
};
inline std::int16_t pc_wrap_angle(int angle) {
  return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(angle));
}
inline PcLookDelta pc_direct_aim(int yaw,int pitch,PcLookDelta delta,int minimum,int maximum) {
  if(minimum>maximum || minimum < -32768 || maximum>32767)return {yaw,pitch};
  return {pc_wrap_angle(yaw-delta.x),std::clamp(pitch-delta.y,minimum,maximum)};
}
struct PcMoveVelocity { float side=0,forward=0; };
inline PcMoveVelocity pc_aim_movement(int x,int y) {
  // Aim walking stays within the guest's slow ground movement speeds.
  float sx=static_cast<float>(std::clamp(x,-80,80))/80.0F;
  float sy=static_cast<float>(std::clamp(y,-80,80))/80.0F;
  const float length=std::sqrt(sx*sx+sy*sy);
  if(length>1.0F){sx/=length;sy/=length;}
  return {sx*2.5F,-sy*3.0F};
}
struct PcOrbit {
  bool active=false;
  std::uint32_t actor=0;
  std::uint64_t retrace=0;
  int yaw=0,pitch=0;
};
struct PcOrbitPoint { float x=0,y=0,z=0; };
inline PcOrbitPoint pc_orbit_point(int yaw,int pitch,float radius) {
  constexpr float radians=6.283185307179586F/65536.0F;
  const float y=static_cast<float>(yaw)*radians,p=static_cast<float>(pitch)*radians;
  const float horizontal=radius*std::cos(p);
  return {std::sin(y)*horizontal,std::sin(p)*radius,-std::cos(y)*horizontal};
}
inline PcOrbitPoint pc_camera_clearance(PcOrbitPoint focus,PcOrbitPoint wanted,PcOrbitPoint resolved) {
  const float ex=resolved.x-wanted.x,ey=resolved.y-wanted.y,ez=resolved.z-wanted.z;
  const float changed=ex*ex+ey*ey+ez*ez;
  const float dx=resolved.x-focus.x,dy=resolved.y-focus.y,dz=resolved.z-focus.z;
  const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
  if(!std::isfinite(changed) || !std::isfinite(distance) || changed<1.0F || distance<=16.0F)return resolved;
  // Only retreat along an actual line-of-sight hit. A vertical floor/ceiling
  // correction is not evidence that this ray has free clearance behind it.
  const float dot=ex*dx+ey*dy+ez*dz;
  const float perpendicular=changed-dot*dot/(distance*distance);
  if(dot>=0 || perpendicular>std::max(1.0F,changed*0.01F))return resolved;
  const float scale=(distance-std::min(12.0F,distance-16.0F))/distance;
  return {focus.x+dx*scale,focus.y+dy*scale,focus.z+dz*scale};
}
inline PcLookDelta pc_camera_delta(std::uint8_t mode,PcLookDelta look,float ticks) {
  if(mode==3)return look;
  if(mode!=4 || !std::isfinite(ticks) || ticks<=0)return {};
  const float rate=256.0F*std::min(ticks,5.0F)/80.0F;
  return {static_cast<int>(std::lround(static_cast<float>(look.x)*rate)),
          static_cast<int>(std::lround(static_cast<float>(look.y)*rate))};
}
class PcMouseMotion {
  double x_=0,y_=0;
public:
  void add(int x,int y){x_=std::clamp(x_+double(x),-4096.0,4096.0);y_=std::clamp(y_+double(y),-4096.0,4096.0);}
  N64StickSample take(const PcInputConfig &c) {
    const double scale=double(c.mouse_aim_sensitivity)/100.0;
    const int x=std::clamp(static_cast<int>(std::trunc(x_*scale)),-127,127);
    const double y_scale=scale*double(c.mouse_vertical_sensitivity)/100.0*(c.mouse_invert_y?1.0:-1.0);
    const int y=std::clamp(static_cast<int>(std::trunc(y_*y_scale)),-127,127);
    // Preserve subpixel displacement at low sensitivity, but discard clipped
    // excess so a fast flick cannot keep turning after the mouse stops.
    x_=std::abs(x_*scale)>127.0?0.0:x_-double(x)/scale;
    y_=std::abs(y_*y_scale)>127.0?0.0:y_-double(y)/y_scale;
    return {static_cast<std::int8_t>(x),static_cast<std::int8_t>(y)};
  }
  PcLookDelta take_angles(const PcInputConfig &c,bool aiming=false) {
    // One pixel at 100% is approximately 0.1 degrees, independent of update rate.
    const double sx=18.0*double(aiming?c.mouse_aim_sensitivity:c.mouse_sensitivity)/100.0;
    const double sy=sx*double(c.mouse_vertical_sensitivity)/100.0*(c.mouse_invert_y?1.0:-1.0);
    const int x=std::clamp(static_cast<int>(std::trunc(x_*sx)),-16384,16384);
    const int y=std::clamp(static_cast<int>(std::trunc(y_*sy)),-16384,16384);
    x_=std::abs(x_*sx)>16384.0?0.0:x_-double(x)/sx;
    y_=std::abs(y_*sy)>16384.0?0.0:y_-double(y)/sy;
    return {x,y};
  }
  void clear(){x_=y_=0;}
};
// RGO: angle deltas from several sources (mouse, aim keys, gyro) add up.
inline PcLookDelta pc_add_look(PcLookDelta a,PcLookDelta b) {
  return {std::clamp(a.x+b.x,-16384,16384),std::clamp(a.y+b.y,-16384,16384)};
}
// RGO: keyboard aim keys turn the aim at key_aim_speed degrees per second.
// The result is a direct angle delta, so the aim stays put when the keys are
// released. Up and right are positive, as for the mouse.
class PcKeyAim {
  double x_=0,y_=0;
public:
  template<class Down> PcLookDelta take(const PcInputConfig &c,Down down,double seconds) {
    const auto held=[&](std::size_t i){return c.keys[i]!=0 && down(c.keys[i]);};
    const int dx=int(held(23))-int(held(22)),dy=int(held(20))-int(held(21));
    if(!dx){x_=0;}
    if(!dy){y_=0;}
    if(!(seconds>0.0))return {};
    const double units=double(std::clamp(c.key_aim_speed,30,360))*65536.0/360.0*std::min(seconds,0.1);
    x_+=dx*units;y_+=dy*units;
    const int x=static_cast<int>(std::trunc(x_)),y=static_cast<int>(std::trunc(y_));
    x_-=x;y_-=y;
    return {x,y};
  }
  template<class Down> static bool held(const PcInputConfig &c,Down down) {
    for(std::size_t i=20;i<24;++i)if(c.keys[i]!=0 && down(c.keys[i]))return true;
    return false;
  }
  void clear(){x_=y_=0;}
};
// RGO: hold or toggle aim (the R button). Toggle only applies while gameplay
// is active; anywhere else, and whenever input is blocked, R is plain hold
// and a toggled aim is released, so it can never stick on in a menu.
class PcAimLatch {
  bool latched_=false,previous_=false;
public:
  bool apply(bool held,bool toggle,bool active) {
    const bool pressed=held && !previous_;
    previous_=held;
    if(!toggle || !active){latched_=false;return held;}
    if(pressed)latched_=!latched_;
    return latched_;
  }
};
} // namespace jfg
