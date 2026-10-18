// RGO: modern twin-stick controller scheme (pc_twin_stick.hpp).
#include "jfg/runtime/pc_twin_stick.hpp"
#include "jfg/rgo/headless_pad_script.hpp"
#include "jfg/runtime/game_actions.hpp"
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

  // Mouse and keyboard: the same scheme turns on mouse look and aim.
  jfg::PcInputConfig keyboard;keyboard.scheme=1;
  active=jfg::effective_keyboard_input(false,keyboard);
  check(active.modern==1 && active.mouse_aim==1,"Modern mouse look works without the experimental switch");
  keyboard.scheme=0;keyboard.modern=keyboard.mouse_aim=1;
  active=jfg::effective_keyboard_input(false,keyboard);
  check(active.modern==0 && active.mouse_aim==0,"Original keyboard scheme keeps experimental mouse options gated");
  check(jfg::pc_twin_mode(6) && jfg::pc_twin_mode(7) && !jfg::pc_twin_mode(3) && !jfg::pc_twin_mode(5),"twin look modes");
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

  // Original one-stick and experimental stick emulation use the same
  // physical up convention before the guest converts Y to pitch.
  for(bool inverted:{false,true}) {
    for(int direction:{-1,1}) {
      jfg::ControllerMapping original_mapping;original_mapping.invert_y=inverted;
      jfg::StandardControllerSample original_pad;original_pad.axes[1]=-direction*32767;
      const auto original_sample=jfg::map_controller(original_mapping,original_pad);
      check((original_sample.stick.y>0)==(direction>0?!inverted:inverted),"original controller up/down inversion");
      jfg::PcInputConfig legacy_config;legacy_config.aim_invert_y=inverted;
      check((jfg::pc_aim_stick(legacy_config,right(0,-direction*32767),0).y>0)==
        (direction>0?!inverted:inverted),"experimental look stick has identical semantics");
      legacy_config.mouse_invert_y=inverted;jfg::PcMouseMotion legacy_mouse;legacy_mouse.add(0,-direction*10);
      check((legacy_mouse.take(legacy_config).y>0)==(direction>0?!inverted:inverted),"legacy mouse emulation has identical semantics");
    }
  }
  // Mouse settings remain independent from controller settings and from
  // each other, including when mouse input joins a controller player.
  for(bool aim_inverted:{false,true}) for(bool camera_inverted:{false,true}) {
    jfg::PcInputConfig independent;independent.mouse_invert_y=aim_inverted;
    independent.mouse_camera_invert_y=camera_inverted;
    independent.aim_invert_y=!aim_inverted;independent.camera_invert_y=!camera_inverted;
    for(bool aiming:{false,true}) {
      jfg::PcMouseMotion mouse;mouse.add(0,-10);
      check((mouse.take_angles(independent,aiming).y<0)==(aiming?aim_inverted:camera_inverted),
        "mouse camera/aim choice is independent of the controller inversion");
    }
  }

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
  for(int character=0;character<3;++character) {
    check(jfg::pc_twin_camera_state(0,static_cast<std::uint8_t>(character)),"all three characters can look without aiming");
    check(jfg::pc_twin_camera_state(0,static_cast<std::uint8_t>(0x80|character)),"preserve character flag bits");
    check(jfg::pc_twin_camera_state(3,static_cast<std::uint8_t>(character)),"normal-follow water state permits look input");
    check(jfg::pc_twin_camera_state(4,static_cast<std::uint8_t>(character)),"alternate water state permits look input");
    check(!jfg::pc_twin_camera_state(6,static_cast<std::uint8_t>(character)),"special movement retains guest camera");
  }
  check(!jfg::pc_twin_camera_state(0,3),"hovership keeps its camera");
  for(int state=32;state<42;++state) {
    auto sample=jfg::pc_boss_movement({0x2000,-80,50,true},static_cast<std::uint8_t>(state));
    check(sample.buttons==0x2002 && sample.x==-80 && sample.y==50,"boss left stick preserves fire and maps native strafe");
    check(jfg::pc_boss_movement({0,80,0,true},static_cast<std::uint8_t>(state)).buttons==1,"boss right strafe");
  }
  check(jfg::pc_boss_movement({0,80,0,true},42).buttons==0,"frozen boss state unchanged");
  check(jfg::pc_boss_movement({0,80,0,true},0).buttons==0,"regular gameplay unchanged");
  check(jfg::pc_boss_movement({0,80,0,false},32).buttons==0,"disconnected input cannot strafe");
  // Test the result, not just the sign of an intermediate input. Negative
  // guest aim pitch and a lowered camera position both point the view up.
  for(bool inverted:{false,true}) {
    for(bool aiming:{false,true}) {
      for(int up:{-1,1}) {
        jfg::PcInputConfig audit_config;audit_config.scheme=1;
        audit_config.aim_invert_y=audit_config.camera_invert_y=audit_config.mouse_invert_y=audit_config.mouse_camera_invert_y=inverted;
        jfg::PcTwinTurn turn;
        const auto stick=turn.apply(jfg::pc_twin_rate(audit_config,jfg::pc_look_stick(audit_config,right(0,-up*32767),0),aiming),1.0F);
        jfg::PcMouseMotion mouse;mouse.add(0,-up*10);
        const auto motion=mouse.take_angles(audit_config,aiming);
        jfg::PcKeyAim keys;
        const auto key=keys.take(audit_config,[&](int k){return k==audit_config.keys[up>0?20:21];},1.0/60.0,aiming);
        jfg::PcGyroAim gyro;audit_config.gyro_invert_y=inverted;
        const auto tilt=gyro.apply(audit_config,0,up*0.01);
        for(auto audit_look:{stick,motion,key,tilt}) {
          const auto result=jfg::pc_direct_aim(0,0,audit_look,-16384,16384);
          check((result.y<0)==(up>0?!inverted:inverted),"every audit_look source follows normal/inverted aim contract");
          const auto orbit=jfg::pc_orbit_point(0,-audit_look.y,200);
          check((orbit.y<0)==(up>0?!inverted:inverted),"every audit_look source follows normal/inverted camera contract");
        }
        const auto together=jfg::pc_add_look(stick,motion);
        check((together.y>0)==(motion.y>0),"dual input mouse and stick reinforce the same direction");
      }
    }
  }
  check(jfg::pc_twin_turn_in_place(0,0,0) && jfg::pc_twin_turn_in_place(0,5,-5),"standing still turns in place");
  check(!jfg::pc_twin_turn_in_place(0,0,40) && !jfg::pc_twin_turn_in_place(0,-20,0),"moving steers instead");
  check(!jfg::pc_twin_turn_in_place(11,0,0) && !jfg::pc_twin_turn_in_place(6,0,0),"aim and special states keep their own heading");
  {
    const auto f=jfg::pc_twin_aim_movement(0,80),b=jfg::pc_twin_aim_movement(0,-80),r=jfg::pc_twin_aim_movement(80,0);
    check(std::abs(f.forward+2.0F)<1e-5F && std::abs(b.forward-2.0F)<1e-5F && std::abs(r.side-2.5F)<1e-5F && r.forward==0.0F,"aim walk speeds");
    const auto d=jfg::pc_twin_aim_movement(80,80);
    check(std::abs(d.side*d.side/6.25F+d.forward*d.forward/4.0F-1.0F)<1e-3F,"diagonal aim walk is not faster");
  }
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
  // Controllers page labels: the action, then the N64 button.
  check(jfg::game_action_label(10,false)=="Jump / zoom in (C-up)" && jfg::game_action_label(0,true)=="Jump / zoom in (A)","jump label per game option");
  check(jfg::game_action_label(11,false)=="Crouch / zoom out (C-down)" && jfg::game_action_label(1,true)=="Crouch / zoom out (B)","crouch label");
  check(jfg::game_action_label(0,false)=="Previous weapon (A)" && jfg::game_action_label(10,true)=="Next weapon (C-up)","weapon labels");
  check(jfg::game_action_label(2,true)=="Fire (Z)" && jfg::game_action_label(9,false)=="Aim (R)" && jfg::game_action_label(12,false)=="Sidestep left (C-left)","shared labels");
  check(jfg::game_action_label(4,false)=="D-pad up" && jfg::game_action_label(8,true)=="L" && jfg::game_action_label(14,false).empty(),"no action, no guess");
  {
    jfg::PcInputConfig labels;labels.game_controls=1;
    check(jfg::parse_pc_input(jfg::serialize_pc_input(labels),roundtrip) && roundtrip.game_controls==1,"game control option roundtrips");
    jfg::PcInputConfig camera_invert;camera_invert.mouse_camera_invert_y=1;
    check(jfg::parse_pc_input(jfg::serialize_pc_input(camera_invert),roundtrip) && roundtrip.mouse_camera_invert_y==1 &&
          roundtrip.mouse_invert_y==0,"mouse camera invert roundtrips separately from the aim's");
  }
  // Headless scripted controller, mouse and keyboard.
  {
    const auto script=jfg::rgo::parse_pad_script("# test\n2 0 -32767 0 0 32767 0 0,10 # run\n1 0 0 0 0 0 0 - k:87,2 m:12,-3\n");
    check(script && script->size()==2,"pad script parses");
    check((*script)[0].pad.axes[1]==-32767 && (*script)[0].pad.axes[4]==32767 && (*script)[0].pad.buttons[10] && (*script)[0].pad.buttons[0],"pad script axes and buttons");
    check((*script)[1].keys==std::vector<int>{87,2} && (*script)[1].mouse_x==12 && (*script)[1].mouse_y==-3,"pad script keys and mouse");
    jfg::rgo::PadScript timed(*script);
    check(timed.at_tick(0U).polls==2 && timed.at_tick(0U).polls==2 && timed.at_tick(1U).polls==2,"extra polls cannot shorten timed input");
    check(timed.at_tick(2U).polls==1 && timed.at_tick(9999999999ULL).polls==1,"timed input crosses boundary and holds final step");
    check(timed.at_tick(0U).polls==2 && timed.step()==0U,"timed input can rewind reproducibly");
    jfg::rgo::PadScript run(*script);
    check(run.next().polls==2 && run.next().polls==2 && run.next().polls==1 && run.next().polls==1 && run.step()==1,"pad script steps hold");
    for(const char* bad:{"","0 0 0 0 0 0 0 -\n","1 0 0 0 0 0 0\n","1 0 0 0 0 0 0 17\n","1 0 0 0 0 0 40000 -\n","1 0 0 0 0 0 0 - k:300\n","1 0 0 0 0 0 0 - m:1\n","1 0 0 0 0 0 0 - x:1\n"})
      check(!jfg::rgo::parse_pad_script(bad),"bad pad script rejected");
  }
  std::cout<<checks<<" twin-stick checks passed\n";
}
