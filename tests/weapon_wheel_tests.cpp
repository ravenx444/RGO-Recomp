#include "jfg/runtime/weapon_wheel.hpp"
#include <cstdlib>
#include <limits>
static void check(bool ok){if(!ok)std::abort();}
int main(){
 using namespace jfg;
 check(hide_roaming_floyd(true,false,false,true,false,false));
 check(!hide_roaming_floyd(false,false,false,true,false,false));
 check(!hide_roaming_floyd(true,true,false,true,false,false));
 check(!hide_roaming_floyd(true,false,true,true,false,false));
 check(!hide_roaming_floyd(true,false,false,false,false,false));
 check(!hide_roaming_floyd(true,false,false,true,true,false));
 check(!hide_roaming_floyd(true,false,false,true,false,true));
 WeaponWheel side;
 check(!side.side_action(true,true));
 side.open=true;check(!side.side_action(true,true));
 check(!side.side_action(false,true));check(side.side_action(true,true));
 check(!side.side_action(true,true));
 side.side_action(false,true);check(!side.side_action(true,false));
 const std::uint16_t mask=0x8007;
 check(wheel_count(mask)==4 && wheel_item(mask,3)==kWheelFloyd);
 check(wheel_pick(mask,0,-1)==0);check(wheel_pick(mask,1,0)==1);
 check(wheel_pick(mask,0,1)==2);check(wheel_pick(mask,-1,0)==kWheelFloyd);
 check(wheel_pick(mask,0.1,0.1)==-1);check(wheel_pick(0,1,0)==-1);
 check(wheel_pick(mask,std::numeric_limits<double>::infinity(),0)==-1);
 WeaponWheel w;
 check(w.update(true,true,10,1,mask,0,-1,false)==-1 && !w.open);
 check(w.update(false,true,10,1,mask,0,0,false)==-1);
 check(w.update(true,true,10,1,mask,1,0,false)==-1 && w.open && w.selected==1);
 check(w.update(false,true,10,1,mask,0,0,false)==1 && !w.open);
 w.update(true,true,10,1,mask,-1,0,false);
 check(w.selected==kWheelFloyd);
 check(w.update(false,false,10,1,mask,0,0,false)==-1 && !w.open);
 w.update(true,true,10,1,mask,-1,0,false);check(!w.open);
 w.update(false,true,10,1,mask,0,0,false);
 w.update(true,true,10,1,mask,0,-1,false);
 check(w.update(false,true,11,2,mask,0,0,false)==-1);
 w.update(false,true,11,2,mask,0,0,false);
 w.update(true,true,11,2,mask,0,-0.5,true);check(w.selected==0);
 w.update(true,true,11,2,mask,0,0.5,true);check(w.selected==-1);
 check(w.update(false,true,11,2,mask,0,0,true)==-1);
 w.update(true,true,11,2,mask,0,-1,false);
 check((w.presentation(true)&(1ULL<<24))!=0);
 check(w.update(false,true,11,2,1,0,0,false)==-1); // ownership changed
}
