#include "jfg/runtime/pc_input.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
namespace {
int checks=0;
void check(bool result,const char* what){++checks;if(!result){std::cerr<<what<<'\n';std::exit(1);}}
}
int main() {
  jfg::PcInputConfig config,roundtrip;
  check(!jfg::pc_experiments_enabled(""),"missing switch stays disabled");
  check(!jfg::pc_experiments_enabled("version=1\nexperimental=2\n"),"invalid opt-in stays disabled");
  check(!jfg::pc_experiments_enabled("version=1\nexperimental=1\nunknown=1\n"),"unknown opt-in rejected");
  check(jfg::pc_experiments_enabled(jfg::serialize_pc_experiments(true)),"explicit opt-in");
  check(!jfg::pc_experiments_enabled(jfg::serialize_pc_experiments(false)),"explicit opt-out");
  check(jfg::pc_experiments_enabled("version=1\r\nexperimental=1\r\n"),"CRLF opt-in");
  auto stored=jfg::pc_keyboard_preset();stored.dual_stick=1;
  auto gated=jfg::effective_pc_input(false,stored);
  check(gated.keys==stored.keys && !gated.mouse_aim && !gated.dual_stick && !gated.modern,"opt-out retains bindings and disables modern behavior");
  check(jfg::effective_pc_input(true,stored).keys==stored.keys && stored.mouse_aim,"opt-in retains saved bindings");
  auto offMouse=jfg::map_pc_keyboard(gated,[](int k){return k==1 || k==2;});
  check((offMouse.buttons&0x2010)==0x2010,"mouse buttons remain bound with experiments off");
  gated.keys[14]='T';gated.keys[2]=jfg::kPcWheelUp;
  const auto offCustom=jfg::map_pc_keyboard(gated,[](int k){return k=='T' || k==jfg::kPcWheelUp;});
  check(offCustom.y==80 && (offCustom.buttons&0x2000),"custom movement and wheel work with experiments off");
  const auto text=jfg::serialize_pc_input(config);
  check(jfg::parse_pc_input(text,roundtrip)&&roundtrip.keys==config.keys,"default roundtrip");
  auto invalid=[&](std::string s){const auto before=jfg::serialize_pc_input(roundtrip);check(!jfg::parse_pc_input(s,roundtrip)&&jfg::serialize_pc_input(roundtrip)==before,"invalid config changed active settings");};
  invalid(text+"mouse_aim=1\n");invalid(text.substr(text.find('\n')+1));invalid(text+"unknown=2\n");
  auto replace=[&](std::string a,std::string b){auto t=text;t.replace(t.find(a),a.size(),b);invalid(t);};
  replace("key0=32","key0=258");replace("key0=32","key0=-1");replace("mouse_sensitivity=100","mouse_sensitivity=0");
  replace("aim_deadzone=7849","aim_deadzone=32767");replace("dual_stick=0","dual_stick=2");replace("key0=32","key0=32junk");
  replace("version=4","version=5");invalid(std::string(4097,'x'));
  std::array<bool,258> keys{};
  const auto sample=[&](){return jfg::map_pc_keyboard(config,[&](int k){return keys[static_cast<std::size_t>(k)];});};
  keys['W']=keys['D']=true;auto move=sample();check(move.x==80&&move.y==80,"movement directions");
  keys['S']=keys['A']=true;check(sample().x==0&&sample().y==0,"opposite movement cancellation");
  keys={};keys[32]=true;check(sample().buttons==0x8000,"Space compatibility");
  keys={};keys['Z']=true;check(sample().buttons==0x8000,"alternate A compatibility");
  config=jfg::pc_keyboard_preset();keys={};keys[1]=keys[2]=true;check((sample().buttons&0x2010)==0x2010,"mouse fire and aim together");
  keys={};keys[32]=true;check(sample().buttons==8,"Normal jump preset");
  config=jfg::pc_keyboard_preset(true);check(sample().buttons==0x8000,"Expert jump preset");
  config.keys[0]=0;keys={};keys[0]=true;check(sample().buttons==0,"unbound key");
  jfg::PcWheelPulses wheel;wheel.add(60);check(wheel.sample()==0,"partial wheel notch");wheel.add(300);
  check(wheel.sample()==256&&wheel.sample()==0&&wheel.sample()==256&&wheel.sample()==0&&wheel.sample()==256&&wheel.sample()==0&&wheel.sample()==0,"wheel pulses retain releases");
  wheel.add(-240);check(wheel.sample()==257,"wheel down");wheel.clear();check(wheel.sample()==0,"wheel cleared on capture loss");
  jfg::PcMouseMotion mouse;config={};mouse.add(12,-9);auto aim=mouse.take(config);
  check(aim.x==12&&aim.y==9,"relative mouse axes");check(mouse.take(config)==jfg::N64StickSample{},"mouse delta consumed once");
  config.mouse_invert_y=1;config.mouse_aim_sensitivity=200;mouse.add(10,10);aim=mouse.take(config);check(aim.x==20&&aim.y==20,"mouse sensitivity and inversion");
  mouse.add(2147483647,-2147483647);aim=mouse.take(config);check(aim.x==127&&aim.y==-127,"bounded mouse spikes");
  mouse.add(20,20);mouse.clear();check(mouse.take(config)==jfg::N64StickSample{},"stale mouse cleared");
  config={};config.mouse_aim_sensitivity=10;int slow_total=0;
  for(int i=0;i<20;++i){mouse.add(1,0);slow_total+=mouse.take(config).x;}
  check(slow_total==2,"slow mouse displacement retained across updates");
  mouse.add(4096,0);(void)mouse.take(config);
  check(mouse.take(config)==jfg::N64StickSample{},"clipped mouse does not drift afterward");
  mouse.add(1,0);(void)mouse.take(config);mouse.clear();
  check(mouse.take(config)==jfg::N64StickSample{},"fractional motion cleared on disable");
  config.mouse_aim_sensitivity=50;mouse.add(1,-1);bool idle_stable=true;
  for(int i=0;i<10;++i)idle_stable &= mouse.take(config)==jfg::N64StickSample{};
  check(idle_stable,"half-pixel residual must not oscillate while idle");
  mouse.add(1,-1);check(mouse.take(config)==jfg::N64StickSample{1,1},"half-pixel residual accumulates on new motion");
  jfg::StandardControllerSample pad;pad.axes={32767,0,0,-32768,0,0};config={};
  aim=jfg::pc_aim_stick(config,pad,0);check(aim.x==0&&aim.y==80,"right aim independent of left movement");
  aim=jfg::pc_aim_stick(config,pad,1);check(aim.x==80&&aim.y==0,"swapped sticks remain independent");
  pad.axes={0,0,2000,-2000,0,0};check(jfg::pc_aim_stick(config,pad,0)==jfg::N64StickSample{},"aim stick drift deadzone");
  pad.axes={0,0,0,-32768,0,0};config.aim_invert_y=1;check(jfg::pc_aim_stick(config,pad,0).y==-80,"aim stick Y inversion");
  jfg::ControllerInputGate gate;std::array<jfg::ControllerPortSample,4> raw{};
  check(gate.blocked(true,raw),"settings capture");raw[0].buttons=1;check(gate.blocked(false,raw),"aim deflection prevents premature release");raw[0].buttons=0;check(!gate.blocked(false,raw),"neutral release");
  config=jfg::pc_keyboard_preset();check(config.modern==1,"modern keyboard preset");
  auto old=jfg::serialize_pc_input(config);
  for(const auto name:{"mouse_aim_sensitivity","camera_sensitivity","aim_curve","mouse_vertical_sensitivity","stick_vertical_sensitivity","rumble","aim_toggle","key_aim_speed","key20","key21","key22","key23","gyro","gyro_sensitivity","gyro_mode","gyro_ratchet","gyro_invert_y","dual_input","scheme","camera_invert_y","game_controls"}) {
    const auto at=old.find(std::string(name)+"=");old.erase(at,old.find('\n',at)-at+1);
  }
  old.replace(old.find("version=4"),9,"version=1");
  old.erase(old.find("modern=1\n"),9);
  check(jfg::parse_pc_input(old,roundtrip) && !roundtrip.modern,"v1 profile retains legacy aim");
  config={};mouse.add(10,-5);auto angular=mouse.take_angles(config);
  check(angular==jfg::PcLookDelta{180,90},"mouse displacement maps directly to angles");
  check(mouse.take_angles(config)==jfg::PcLookDelta{},"direct mouse motion consumed once");
  auto direct=jfg::pc_direct_aim(-32760,0,{100,200},-100,100);
  check(direct.x==32676 && direct.y==-100,"direct aim wraps yaw and clamps pitch");
  check(jfg::pc_direct_aim(20,30,{},-100,100)==jfg::PcLookDelta{20,30},"stationary direct aim remains still");
  const auto diagonal=jfg::pc_aim_movement(80,80);
  check(std::abs(diagonal.side/2.5F*diagonal.side/2.5F+diagonal.forward/3.0F*diagonal.forward/3.0F-1.0F)<0.001F,"aim diagonal stays normalized");
  check(jfg::pc_aim_movement(0,0).side==0 && jfg::pc_aim_movement(0,0).forward==0,"neutral aim movement");
  const auto clear=jfg::pc_camera_clearance({0,0,0},{0,0,100},{0,0,50});
  check(clear.x==0 && clear.y==0 && clear.z==38,"wall hit retreats toward focus for camera clearance");
  check(jfg::pc_camera_clearance({0,0,0},{0,0,100},{0,0,100}).z==100,"unblocked camera is unchanged");
  check(jfg::pc_camera_clearance({0,0,0},{0,0,100},{0,0,20}).z==16,"camera clearance keeps minimum focus distance");
  const auto floor_clear=jfg::pc_camera_clearance({0,0,0},{0,0,100},{0,16,100});
  check(floor_clear.y==16 && floor_clear.z==100,"floor correction is not pulled back into terrain");
  const auto orbit=jfg::pc_orbit_point(16384,0,200);
  check(std::abs(orbit.x-200)<0.001F && std::abs(orbit.y)<0.001F && std::abs(orbit.z)<0.001F,"quarter orbit preserves radius");
  check(jfg::pc_camera_delta(3,{15,20},5)==jfg::PcLookDelta{15,20},"mouse orbit is displacement independent of guest ticks");
  check(jfg::pc_camera_delta(4,{80,-80},2)==jfg::PcLookDelta{512,-512},"stick orbit is a rate");
  check(jfg::pc_camera_delta(4,{80,80},0)==jfg::PcLookDelta{},"paused stick orbit is still");
  keys={};keys[13]=true;keys[39]=true;
  auto menu=jfg::map_pc_menu(jfg::ControllerPortSample{},[&](int key){return keys[static_cast<std::size_t>(key)];});
  check(menu.buttons==0x8000 && menu.x==80,"Enter and arrows work independently of gameplay preset");
  keys={};keys[8]=true;keys[38]=true;keys[40]=true;
  menu=jfg::map_pc_menu(jfg::ControllerPortSample{},[&](int key){return keys[static_cast<std::size_t>(key)];});
  check(menu.buttons==0x4000 && menu.y==0,"Backspace and opposing menu arrows");
  config={};config.mouse_sensitivity=200;config.mouse_aim_sensitivity=50;
  mouse.clear();mouse.add(10,0);check(mouse.take_angles(config,false).x==360,"camera mouse sensitivity independent");
  mouse.add(10,0);check(mouse.take_angles(config,true).x==90,"aim mouse sensitivity independent");
  config.mouse_vertical_sensitivity=50;mouse.add(0,-10);
  check(mouse.take_angles(config,true).y==45,"mouse vertical scale");
  config={};config.aim_deadzone=0;config.aim_sensitivity=50;config.camera_sensitivity=150;
  pad.axes={0,0,32767,0,0,0};
  check(jfg::pc_aim_stick(config,pad,0,true).x==40,"stick aim sensitivity independent");
  check(jfg::pc_aim_stick(config,pad,0,false).x==120,"stick camera sensitivity independent");
  config.aim_sensitivity=100;config.aim_curve=1;pad.axes[2]=16384;
  const auto precise=jfg::pc_aim_stick(config,pad,0);config.aim_curve=0;
  const auto linear=jfg::pc_aim_stick(config,pad,0);
  check(precise.x>0&&precise.x<linear.x,"precision curve retains analog magnitude near center");
  config.aim_curve=2;check(jfg::pc_aim_stick(config,pad,0).x<precise.x,"extra precision curve");
  pad.axes[2]=32767;check(jfg::pc_aim_stick(config,pad,0).x==80,"curves retain full stick travel");
  pad.axes[2]=0;check(jfg::pc_aim_stick(config,pad,0)==jfg::N64StickSample{},"curve neutral stays neutral");
  config={};config.mouse_sensitivity=175;config.aim_sensitivity=125;
  auto v2=jfg::serialize_pc_input(config);
  for(const auto name:{"mouse_aim_sensitivity","camera_sensitivity","aim_curve","mouse_vertical_sensitivity","stick_vertical_sensitivity","rumble","aim_toggle","key_aim_speed","key20","key21","key22","key23","gyro","gyro_sensitivity","gyro_mode","gyro_ratchet","gyro_invert_y","dual_input","scheme","camera_invert_y","game_controls"}) {
    const auto at=v2.find(std::string(name)+"=");v2.erase(at,v2.find('\n',at)-at+1);
  }
  v2.replace(v2.find("version=4"),9,"version=2");
  check(jfg::parse_pc_input(v2,roundtrip)&&roundtrip.mouse_aim_sensitivity==175&&roundtrip.camera_sensitivity==125&&roundtrip.aim_curve==0,"v2 migration preserves both existing look speeds");
  invalid(v2+"camera_sensitivity=100\n");
  config.aim_curve=2;config.mouse_aim_sensitivity=50;config.camera_sensitivity=150;config.mouse_vertical_sensitivity=80;config.stick_vertical_sensitivity=120;
  check(jfg::parse_pc_input(jfg::serialize_pc_input(config),roundtrip)&&jfg::serialize_pc_input(roundtrip)==jfg::serialize_pc_input(config),"all v3 settings roundtrip");
  replace("aim_curve=0","aim_curve=3");replace("camera_sensitivity=100","camera_sensitivity=301");
  replace("mouse_vertical_sensitivity=100","mouse_vertical_sensitivity=0");
  // RGO version 4: optional extension settings with defaults.
  replace("rumble=100","rumble=201");replace("rumble=100","rumble=-1");
  {
    auto v3=text;v3.replace(v3.find("version=4"),9,"version=3");
    invalid(v3);
    for(const auto name:{"rumble","key20","key21","key22","key23","aim_toggle","key_aim_speed","gyro","gyro_sensitivity","gyro_mode","gyro_ratchet","gyro_invert_y","dual_input","scheme","camera_invert_y","game_controls"}) {
      const auto at=v3.find(std::string(name)+"=");v3.erase(at,v3.find('\n',at)-at+1);
    }
    check(jfg::parse_pc_input(v3,roundtrip)&&roundtrip.rumble==100,"v3 profile loads with default rumble");
    auto v4=text;v4.erase(v4.find("rumble=100\n"),11);
    check(jfg::parse_pc_input(v4,roundtrip)&&roundtrip.rumble==100,"missing v4 extension keeps its default");
    jfg::PcInputConfig quiet;quiet.rumble=0;
    check(jfg::parse_pc_input(jfg::serialize_pc_input(quiet),roundtrip)&&roundtrip.rumble==0,"rumble strength roundtrips");
    invalid(text+"rumble=50\n");
    // RGO direct aim: aim keys, toggle and key speed are optional too.
    const auto legacy=v3;
    check(jfg::parse_pc_input(legacy,roundtrip)&&roundtrip.keys[20]==0x68&&roundtrip.keys[23]==0x66&&
      roundtrip.aim_toggle==0&&roundtrip.key_aim_speed==120,"v3 profile gets default aim keys");
    invalid(legacy+"key20=38\n");
    replace("aim_toggle=0","aim_toggle=2");replace("key_aim_speed=120","key_aim_speed=29");
    replace("key_aim_speed=120","key_aim_speed=361");replace("key23=102","key23=258");
    jfg::PcInputConfig aimKeys;aimKeys.keys[20]='I';aimKeys.aim_toggle=1;aimKeys.key_aim_speed=200;
    check(jfg::parse_pc_input(jfg::serialize_pc_input(aimKeys),roundtrip)&&roundtrip.keys[20]=='I'&&
      roundtrip.aim_toggle==1&&roundtrip.key_aim_speed==200,"aim keys and options roundtrip");
  }
  {
    // Keyboard aim keys turn at a set speed and stop dead on release.
    jfg::PcInputConfig keyConfig;keyConfig.key_aim_speed=120;
    std::array<bool,258> held{};
    const auto down=[&](int k){return held[static_cast<std::size_t>(k)];};
    jfg::PcKeyAim keyAim;
    check(keyAim.take(keyConfig,down,1.0/30.0)==jfg::PcLookDelta{},"no keys, no turn");
    held[0x66]=true;int total=0;
    for(int i=0;i<15;++i){const auto d=keyAim.take(keyConfig,down,1.0/30.0);total+=d.x;check(d.y==0,"right key is horizontal");}
    check(total>=10921 && total<=10923,"half a second at 120 deg/s turns 60 degrees");
    held[0x66]=false;
    check(keyAim.take(keyConfig,down,1.0/30.0)==jfg::PcLookDelta{},"aim stays where it is on release");
    held[0x68]=true;
    check(keyAim.take(keyConfig,down,1.0/30.0).y>0,"aim up is positive like the mouse");
    held[0x68]=false;held[0x62]=true;
    check(keyAim.take(keyConfig,down,1.0/30.0).y<0,"aim down is negative");
    held[0x62]=false;held[0x64]=true;
    check(keyAim.take(keyConfig,down,1.0/30.0).x<0,"aim left is negative");
    check(keyAim.take(keyConfig,down,5.0).x>=-2185,"a long stall is clamped to 0.1 s");
    held[0x66]=true;
    check(keyAim.take(keyConfig,down,1.0/30.0)==jfg::PcLookDelta{},"opposite keys cancel");
    keyConfig.keys[22]=0;held={};held[0]=true;
    check(keyAim.take(keyConfig,down,1.0/30.0)==jfg::PcLookDelta{},"unbound aim key is ignored");
    check(jfg::pc_add_look({16000,-16000},{1000,-1000})==jfg::PcLookDelta{16384,-16384},"look sources add with a bound");
    check(jfg::pc_add_look({10,20},{-3,5})==jfg::PcLookDelta{7,25},"look sources add");
  }
  {
    // RGO gyro aim settings and conversion.
    jfg::PcInputConfig gyroConfig;
    check(gyroConfig.gyro==0 && gyroConfig.gyro_mode==0 && gyroConfig.gyro_ratchet==-1,"gyro defaults: off, while aiming, no ratchet");
    gyroConfig.gyro=1;
    check(jfg::effective_pc_input(false,gyroConfig).gyro==1,"gyro works without the experimental switch");
    gyroConfig.dual_stick=1;
    check(jfg::effective_pc_input(false,gyroConfig).dual_stick==0,"other modern controls still need the switch");
    gyroConfig={};
    gyroConfig.gyro_sensitivity=300;gyroConfig.gyro_mode=1;gyroConfig.gyro_ratchet=jfg::kBindingTouchpad;gyroConfig.gyro_invert_y=1;
    check(jfg::parse_pc_input(jfg::serialize_pc_input(gyroConfig),roundtrip)&&roundtrip.gyro_sensitivity==300&&roundtrip.gyro_mode==1&&
      roundtrip.gyro_ratchet==27&&roundtrip.gyro_invert_y==1,"gyro settings roundtrip");
    const auto gyroText=jfg::serialize_pc_input(jfg::PcInputConfig{});
    for(const auto& bad:{std::pair{"gyro_ratchet=-1","gyro_ratchet=29"},std::pair{"gyro_ratchet=-1","gyro_ratchet=-2"},
        std::pair{"gyro_sensitivity=150","gyro_sensitivity=9"},std::pair{"gyro_mode=0","gyro_mode=2"},std::pair{"gyro=0","gyro=2"}}) {
      auto t=gyroText;t.replace(t.find(bad.first),std::string(bad.first).size(),bad.second);
      check(!jfg::parse_pc_input(t,roundtrip),"out-of-range gyro setting accepted");
    }
    gyroConfig={};gyroConfig.gyro_sensitivity=100;
    jfg::PcGyroAim gyro;
    constexpr double quarter=6.283185307179586/4.0;
    auto turn=gyro.apply(gyroConfig,-quarter,0.0);
    check(turn.x==16384 && turn.y==0,"turning the controller right 90 degrees turns the aim right 90 degrees");
    check(gyro.apply(gyroConfig,0.0,0.0)==jfg::PcLookDelta{},"a still controller does not turn the aim");
    turn=gyro.apply(gyroConfig,0.0,quarter/90.0);
    check(turn.y>=181 && turn.y<=182 && turn.x==0,"nose up aims up");
    gyroConfig.gyro_invert_y=1;
    check(gyro.apply(gyroConfig,0.0,quarter/90.0).y<0,"gyro Y invert");
    gyroConfig.gyro_invert_y=0;gyroConfig.gyro_sensitivity=200;
    check(gyro.apply(gyroConfig,quarter/90.0,0.0).x<=-363,"gyro sensitivity scales and left is negative");
    int small=0;for(int i=0;i<1000;++i)small+=gyro.apply(gyroConfig,-0.00001,0.0).x;
    check(small>=207 && small<=209,"tiny rotations accumulate across polls");
    check(gyro.apply(gyroConfig,std::nan(""),0.0)==jfg::PcLookDelta{},"invalid gyro data is ignored");
    gyroConfig={};
    check(!jfg::pc_gyro_active(gyroConfig,true,false),"gyro is off by default");
    gyroConfig.gyro=1;
    check(jfg::pc_gyro_active(gyroConfig,true,false) && !jfg::pc_gyro_active(gyroConfig,false,false),"default gyro only while aiming");
    check(!jfg::pc_gyro_active(gyroConfig,true,true),"ratchet pauses gyro");
    gyroConfig.gyro_mode=1;check(jfg::pc_gyro_active(gyroConfig,false,false),"always mode");
    gyroConfig.gyro=0;check(!jfg::pc_gyro_active(gyroConfig,true,false),"gyro off");
  }
  {
    jfg::PcInputConfig dualConfig;
    check(dualConfig.dual_input==0,"dual input defaults to last-used device");
    dualConfig.dual_input=1;
    check(jfg::parse_pc_input(jfg::serialize_pc_input(dualConfig),roundtrip)&&roundtrip.dual_input==1,"dual input roundtrips");
  }
  {
    jfg::PcAimLatch latch;
    check(latch.apply(true,false,true) && !latch.apply(false,false,true),"hold aim follows the button");
    check(latch.apply(true,true,true),"toggle press starts aiming");
    check(latch.apply(false,true,true),"toggle aim stays on after release");
    check(!latch.apply(true,true,true) && !latch.apply(false,true,true),"second press stops aiming");
    check(latch.apply(true,true,true),"toggle on again");
    check(!latch.apply(false,true,false),"menus release a toggled aim");
    check(!latch.apply(false,true,true),"aim stays off after returning to gameplay");
    check(latch.apply(true,true,false),"menus use plain hold");
    check(!latch.apply(true,true,true),"a button held across a menu does not toggle");
  }
  jfg::ControllerMapping guestMapping;
  jfg::StandardControllerSample menuPad;menuPad.axes[2]=32767;
  jfg::PcInputConfig dual;dual.dual_stick=dual.modern=1;
  check(jfg::map_pc_controller(guestMapping,menuPad,dual,false).buttons==jfg::map_controller(guestMapping,menuPad).buttons && jfg::map_controller(guestMapping,menuPad).buttons!=0,"unqualified menus and modes preserve original right-stick bindings");
  check(jfg::map_pc_controller(guestMapping,menuPad,dual,true).buttons==0,"qualified modern look does not also press C buttons");
  dual.modern=0;
  check(jfg::map_pc_controller(guestMapping,menuPad,dual,true).buttons!=0,"legacy aim mode preserves right-stick buttons outside aiming");
  menuPad.buttons[10]=true;
  check(jfg::map_pc_controller(guestMapping,menuPad,dual,true).buttons==0x10,"legacy aiming consumes look stick only while aiming");
  dual.dual_stick=0;
  check(jfg::map_pc_controller(guestMapping,menuPad,dual,true).buttons==jfg::map_controller(guestMapping,menuPad).buttons,"disabled dual stick retains original mapping");
  std::cout<<checks<<" PC input checks passed\n";
}
