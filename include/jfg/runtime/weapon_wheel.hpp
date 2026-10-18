#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
namespace jfg {
inline constexpr int kWheelFloyd = 15;
constexpr bool hide_roaming_floyd(bool requested,bool multiplayer,bool cooperative,
                                 bool gameplay,bool scripted_camera,bool piloting) noexcept {
 return requested && !multiplayer && !cooperative && gameplay && !scripted_camera && !piloting;
}
inline constexpr std::array<const wchar_t*,16> kWheelLabels{
 L"Pistol",L"Homing missiles",L"Machine gun",L"Plasma shotgun",L"Shocker",
 L"Tri-rockets",L"Flame thrower",L"Sniper rifle",L"Grenades",L"Shurikens",
 L"Fish food",L"Timed mines",L"Remote mines",L"Flares",L"Cluster bombs",L"Toggle Floyd"};
inline constexpr int wheel_count(std::uint16_t mask) noexcept {
 int n = 0;
 for (int i = 0; i < 16; ++i) {
  if (mask & (1U << i)) ++n;
 }
 return n;
}
inline constexpr int wheel_item(std::uint16_t mask,int index) noexcept {
 for (int i = 0; i < 16; ++i) {
  if (mask & (1U << i)) {
   if (index-- == 0) return i;
  }
 }
 return -1;
}
// Up is the first item; selection proceeds clockwise and ignores look inversion.
inline int wheel_pick(std::uint16_t mask,double x,double y) noexcept {
 if(!std::isfinite(x)||!std::isfinite(y)||x*x+y*y<0.16)return -1;
 const int n=wheel_count(mask);if(!n)return -1;
 constexpr double tau=2.0*std::numbers::pi;
 double a=std::atan2(x,-y);if(a<0)a+=tau;
 const int index=static_cast<int>(std::floor(a/tau*n+0.5))%n;
 return wheel_item(mask,index);
}
struct WeaponWheel {
 bool open=false,held=false,armed=false,interact_held=false;
 bool side_action(bool down,bool available) noexcept {
  const bool pressed=open && available && down && !interact_held;
  interact_held=down;return pressed;
 }
 int selected=-1;
 std::uint16_t mask=0;
 std::uint32_t actor=0,level=0;
 double mouse_x=0,mouse_y=0;
 // A blocked or changed context requires a release before reopening. Returning
 // an action only on release prevents accidental equips on focus loss or travel.
 int update(bool down,bool allowed,std::uint32_t next_actor,std::uint32_t next_level,
            std::uint16_t next_mask,double x,double y,bool mouse) noexcept {
  if(!allowed || (open&&(actor!=next_actor||level!=next_level||mask!=next_mask))) {
   open=false;selected=-1;armed=false;held=down;return -1;
  }
  if(!down) {
   const int action=open?selected:-1;open=false;selected=-1;held=false;armed=true;return action;
  }
  if(!held&&armed) {
   open=true;actor=next_actor;level=next_level;mask=next_mask;selected=-1;
   mouse_x=mouse_y=0;
  }
  held=true;
  if(open) {
   if(mouse){mouse_x=std::clamp(mouse_x+x,-1.0,1.0);mouse_y=std::clamp(mouse_y+y,-1.0,1.0);x=mouse_x;y=mouse_y;}
   selected=wheel_pick(mask,x,y);
  }
  return -1;
 }
 std::uint64_t presentation(bool hidden) const noexcept {
  return open?(1ULL<<63)|mask|(std::uint64_t(selected+1)<<16)|(std::uint64_t(hidden)<<24):0;
 }
};
} // namespace jfg
