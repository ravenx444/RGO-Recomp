// RGO: modern twin-stick controller scheme (pc_twin_stick.hpp).
#include "jfg/runtime/pc_twin_stick.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
namespace {
int checks=0;
void check(bool result,const char* what){++checks;if(!result){std::cerr<<what<<'\n';std::exit(1);}}
jfg::StandardControllerSample right(int x,int y){jfg::StandardControllerSample p;p.axes[2]=x;p.axes[3]=y;return p;}
}
int main() {
  jfg::PcInputConfig c,roundtrip;
  check(c.scheme==0 && c.camera_invert_y==0,"original scheme by default");
  check(!jfg::pc_twin_stick(c),"original is not twin-stick");
  c.scheme=1;c.camera_invert_y=1;
  check(jfg::parse_pc_input(jfg::serialize_pc_input(c),roundtrip) && roundtrip.scheme==1 && roundtrip.camera_invert_y==1,"scheme roundtrips");
  {
    auto text=jfg::serialize_pc_input(jfg::PcInputConfig{});
    auto bad=text;bad.replace(bad.find("scheme=0"),8,"scheme=2");
    check(!jfg::parse_pc_input(bad,roundtrip),"unknown scheme rejected");
    // A file from before the setting existed loads with the original scheme.
    auto old=text;old.erase(old.find("scheme=0\n"),9);
    old.erase(old.find("camera_invert_y=0\n"),18);
    check(jfg::parse_pc_input(old,roundtrip) && roundtrip.scheme==0,"older files load as original");
  }
  // The scheme needs no experimental switch, and original leaves it alone.
  jfg::PcInputConfig twin;twin.scheme=1;
  auto active=jfg::effective_pad_input(false,twin);
  check(active.modern==1 && active.dual_stick==1,"twin-stick works without the experimental switch");
  jfg::PcInputConfig original;original.modern=original.dual_stick=1;
  active=jfg::effective_pad_input(false,original);
  check(active.modern==0 && active.dual_stick==0,"original scheme keeps experimental options gated");
  check(jfg::effective_pad_input(true,original).dual_stick==1,"experimental switch unchanged for original");

  // Look stick: radial deadzone, full travel, curves, independent of movement.
  jfg::PcInputConfig look;
  check(jfg::pc_look_stick(look,right(0,0),0)==jfg::PcStickLook{},"neutral");
  check(jfg::pc_look_stick(look,right(5000,-5000),0)==jfg::PcStickLook{},"drift inside the deadzone");
  auto full=jfg::pc_look_stick(look,right(32767,0),0);
  check(std::abs(full.x-1.0)<1e-9 && full.y==0,"full right");
  full=jfg::pc_look_stick(look,right(0,-32768),0);
  check(std::abs(full.y-1.0)<1e-9,"stick up looks up");
  auto diagonal=jfg::pc_look_stick(look,right(23170,-23170),0);
  check(std::abs(std::hypot(diagonal.x,diagonal.y)-1.0)<1e-3 && std::abs(diagonal.x-diagonal.y)<1e-9,"diagonal is as fast as straight");
  jfg::StandardControllerSample left;left.axes[0]=32767;
  check(jfg::pc_look_stick(look,left,0)==jfg::PcStickLook{},"movement stick never looks");
  check(jfg::pc_look_stick(look,left,1).x>0.99,"swapped sticks");
  auto half=jfg::pc_look_stick(look,right(20000,0),0).x;
  look.aim_curve=1;auto precise=jfg::pc_look_stick(look,right(20000,0),0).x;
  look.aim_curve=2;auto extra=jfg::pc_look_stick(look,right(20000,0),0).x;
  check(half>precise && precise>extra && extra>0,"precise curves slow small movements");
  check(jfg::pc_look_stick(look,right(32767,0),0).x>0.999,"curves keep full travel");
  look.aim_curve=0;look.aim_deadzone=0;
  check(jfg::pc_look_stick(look,right(100,0),0).x>0,"zero deadzone");

  // Rates: degrees per second, independent of frame rate.
  jfg::PcInputConfig rate;
  const auto camera=jfg::pc_twin_rate(rate,{1,0},false);
  const auto aim=jfg::pc_twin_rate(rate,{1,0},true);
  const auto per_second=[](int r){return r/16.0*60.0*360.0/65536.0;};
  check(std::abs(per_second(camera.x)-200.0)<0.1,"camera 200 degrees/s at 100%");
  check(std::abs(per_second(aim.x)-100.0)<0.1,"aim 100 degrees/s at 100%");
  rate.camera_sensitivity=300;
  check(std::abs(per_second(jfg::pc_twin_rate(rate,{1,0},false).x)-600.0)<0.2,"sensitivity is not capped by a fake stick");
  rate={};rate.aim_invert_y=1;
  check(jfg::pc_twin_rate(rate,{0,1},true).y<0 && jfg::pc_twin_rate(rate,{0,1},false).y>0,"aim invert Y leaves the camera alone");
  rate={};rate.camera_invert_y=1;
  check(jfg::pc_twin_rate(rate,{0,1},false).y<0 && jfg::pc_twin_rate(rate,{0,1},true).y>0,"camera invert Y leaves the aim alone");
  rate={};rate.stick_vertical_sensitivity=50;
  check(std::abs(jfg::pc_twin_rate(rate,{0,1},false).y*2-jfg::pc_twin_rate(jfg::PcInputConfig{},{0,1},false).y)<=1,"vertical scale");

  // Same turn per second at 30 fps (2 ticks an update) and True 60 (1 tick).
  {
    const auto r=jfg::pc_twin_rate(jfg::PcInputConfig{},{0.37,-0.21},true);
    jfg::PcTwinTurn at30,at60;int x30=0,y30=0,x60=0,y60=0;
    for(int i=0;i<30;++i){auto d=at30.apply(r,2.0F);x30+=d.x;y30+=d.y;}
    for(int i=0;i<60;++i){auto d=at60.apply(r,1.0F);x60+=d.x;y60+=d.y;}
    check(std::abs(x30-x60)<=1 && std::abs(y30-y60)<=1,"frame-rate independent");
    check(std::abs(x30-r.x*60/16)<=1,"one second of turning");
    jfg::PcTwinTurn slow;int total=0;
    for(int i=0;i<600;++i)total+=slow.apply({3,0},1.0F).x;
    check(total>=112 && total<=113,"slow turns accumulate fractions");
    check(slow.apply({100,0},0.0F)==jfg::PcLookDelta{},"paused game does not turn");
    check(slow.apply({100,0},std::nanf(""))==jfg::PcLookDelta{},"invalid elapsed ignored");
    slow.apply({7,7},1.0F);check(slow.apply({0,0},1.0F)==jfg::PcLookDelta{},"released stick stops at once");
  }

  // Bindings: L2 aims (R), R2 fires (Z), nothing on the right stick.
  for(bool expert:{false,true}) {
    const auto m=jfg::pc_twin_stick_mapping(expert,2);
    check(m.device==2 && m.stick==0,"keeps the device, left stick moves");
    check(m.bindings[9]==23 && m.bindings[2]==25,"L2 aims, R2 fires");
    for(int b:m.bindings)check(b<19 || b>22,"no binding on the right stick");
    jfg::StandardControllerSample pad;pad.axes[4]=32767;
    check(jfg::map_controller(m,pad).buttons==0x10,"L2 is the game's R");
    pad={};pad.axes[2]=32767;pad.axes[3]=32767;
    check(jfg::map_controller(m,pad).buttons==0 && jfg::map_controller(m,pad).stick==jfg::N64StickSample{},"right stick sends nothing to the game");
    pad={};pad.buttons[0]=true;
    check(jfg::map_controller(m,pad).buttons==(expert?0x8000:0x0008),"Cross jumps in both game schemes");
  }
  check(jfg::pc_camera_heading(0)==-32768 && jfg::pc_camera_heading(0x4000)==0x4000 && jfg::pc_camera_heading(-0x4000)==-0x4000,"camera heading");
  // Aim walking: the left stick holds the game's own aim-walk keys.
  {
    const jfg::PcAimWalkKeys normal{0x8000,0x4000,0x2,0x1}; // ControlModeNormal: A/B, C-left/C-right
    check(jfg::pc_twin_aim_walk({0,0},normal)==0,"still stick walks nowhere");
    check(jfg::pc_twin_aim_walk({10,-10},normal)==0,"small push inside the threshold");
    check(jfg::pc_twin_aim_walk({0,80},normal)==0x8000,"forward");
    check(jfg::pc_twin_aim_walk({0,-80},normal)==0x4000,"back");
    check(jfg::pc_twin_aim_walk({-80,0},normal)==(0x8000|0x2),"strafe left");
    check(jfg::pc_twin_aim_walk({80,-60},normal)==(0x4000|0x1),"back and right");
    check(jfg::pc_twin_aim_state(5) && jfg::pc_twin_aim_state(11) && !jfg::pc_twin_aim_state(0) && !jfg::pc_twin_aim_state(1),"aim states");
  }
  std::cout<<checks<<" twin-stick checks passed\n";
}
