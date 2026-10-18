#pragma once
#include "jfg/runtime/weapon_wheel.hpp"
#include "jfg/runtime/gamepad.hpp"
#include <atomic>
#include <memory>
#include <filesystem>
#include <objidl.h>
#include <gdiplus.h>
namespace jfg {
// UI thread owns the window, decoded private icons and all drawing resources.
// The game thread publishes an atomic presentation value, never guest memory.
class WeaponWheelWindow {
 HWND popup_=nullptr,game_=nullptr;
 std::atomic<std::uint64_t>& view_;
 std::uint64_t painted_=0;
 ULONG_PTR graphics_=0;
 std::array<std::unique_ptr<Gdiplus::Bitmap>,15> icons_{};
 static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b) {
  if(m==WM_NCCREATE)SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams));
  auto* self=reinterpret_cast<WeaponWheelWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
  if(self&&m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);self->paint(dc,r,self->painted_);EndPaint(w,&ps);return 0;}
  if(m==WM_NCHITTEST)return HTTRANSPARENT;
  if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
  if(m==WM_ERASEBKGND)return 1;
  return DefWindowProcW(w,m,a,b);
 }
public:
 explicit WeaponWheelWindow(std::atomic<std::uint64_t>& v):view_(v){}
 void load_icons(const std::filesystem::path& directory) {
  for(std::size_t i=0;i<icons_.size();++i) {
   const auto file=directory/(L"weapon-"+std::to_wstring(i)+L".png");
   std::error_code error;const auto size=std::filesystem::file_size(file,error);
   if(error||size>262144)continue;
   auto image=std::make_unique<Gdiplus::Bitmap>(file.c_str());
   if(image->GetLastStatus()==Gdiplus::Ok && image->GetWidth()>0 && image->GetWidth()<=128 && image->GetHeight()>0 && image->GetHeight()<=128)icons_[i]=std::move(image);
  }
 }
 void create(HWND game) {
  game_=game;Gdiplus::GdiplusStartupInput startup;
  if(Gdiplus::GdiplusStartup(&graphics_,&startup,nullptr)!=Gdiplus::Ok)return;
  wchar_t directory[32768]{};
  const auto length=GetEnvironmentVariableW(L"JFG_WEAPON_WHEEL_ICONS",directory,32768);
  if(length>0 && length<32768)load_icons(directory);
  WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"RgoWeaponWheel";
  RegisterClassW(&c);
  popup_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,c.lpszClassName,L"Weapon wheel",WS_POPUP,0,0,900,640,GetAncestor(game,GA_ROOT),nullptr,c.hInstance,this);
  if(popup_)SetLayeredWindowAttributes(popup_,RGB(0,0,0),245,LWA_COLORKEY|LWA_ALPHA);
 }
 void tick() {
  if(!popup_)return;
  const auto value=view_.load(std::memory_order_acquire);const auto root=GetAncestor(game_,GA_ROOT);
  if(!value||!IsWindowVisible(game_)||GetForegroundWindow()!=root||IsIconic(root)){ShowWindow(popup_,SW_HIDE);return;}
  RECT r{};GetClientRect(game_,&r);MapWindowPoints(game_,nullptr,reinterpret_cast<POINT*>(&r),2);
  const int size=static_cast<int>(std::min(640L,std::min((r.right-r.left)*2/3,r.bottom-r.top)-16L));
  if(size<160){ShowWindow(popup_,SW_HIDE);return;}
  const int width=size*3/2;painted_=value;
  SetWindowPos(popup_,HWND_TOP,(r.left+r.right-width)/2,(r.top+r.bottom-size)/2,width,size,SWP_NOACTIVATE|SWP_SHOWWINDOW);
  InvalidateRect(popup_,nullptr,FALSE);
 }
 void destroy(){if(popup_)DestroyWindow(popup_);popup_=nullptr;UnregisterClassW(L"RgoWeaponWheel",GetModuleHandleW(nullptr));for(auto& icon:icons_)icon.reset();if(graphics_)Gdiplus::GdiplusShutdown(graphics_);graphics_=0;}
 void paint(HDC dc,RECT r,std::uint64_t view) {
  using namespace Gdiplus;
  const int width=r.right-r.left,height=r.bottom-r.top;
  Bitmap buffer(width,height,PixelFormat32bppARGB);Graphics g(&buffer);
  g.Clear(Color(255,0,0,0));g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  const float size=static_cast<float>(std::min(height,width*2/3)),cx=width-size/2,cy=height/2.0F,outer=size*.47F,inner=size*.24F;
  SolidBrush dark(Color(255,12,25,36)),cyan(Color(255,92,222,235)),white(Color(255,234,245,247)),muted(Color(255,153,183,197));
  Pen border(Color(255,54,111,133),1.5F),accent(Color(255,92,222,235),2.0F);
  g.FillEllipse(&dark,cx-outer,cy-outer,outer*2,outer*2);
  const auto mask=static_cast<std::uint16_t>(view)&0x7FFFU;
  const int chosen=static_cast<int>((view>>16)&255U)-1,n=wheel_count(static_cast<std::uint16_t>(mask));
  const float step=n?360.0F/static_cast<float>(n):360.0F;
  for(int i=0;i<n;++i) {
   const auto item=wheel_item(static_cast<std::uint16_t>(mask),i);const float angle=-90.0F+i*step;
   GraphicsPath wedge;const float sweep=step-2.0F,start=angle-step/2+1;
   wedge.AddArc(cx-outer,cy-outer,outer*2,outer*2,start,sweep);
   wedge.AddArc(cx-inner,cy-inner,inner*2,inner*2,start+sweep,-sweep);wedge.CloseFigure();
   SolidBrush fill(item==chosen?Color(255,28,101,123):Color(255,20,42,56));g.FillPath(&fill,&wedge);g.DrawPath(item==chosen?&accent:&border,&wedge);
   const double a=angle*std::numbers::pi/180.0;const float radius=(outer+inner)/2;
   const float x=cx+static_cast<float>(std::cos(a))*radius,y=cy+static_cast<float>(std::sin(a))*radius;
   const float icon_size=std::min(size*.13F,size*2.0F/static_cast<float>(std::max(4,n)));
   if(icons_[static_cast<std::size_t>(item)]) {
    auto* icon=icons_[static_cast<std::size_t>(item)].get();const float aspect=static_cast<float>(icon->GetWidth())/icon->GetHeight();
    const float iw=aspect>1?icon_size:icon_size*aspect,ih=aspect>1?icon_size/aspect:icon_size;
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);g.DrawImage(icon,RectF(x-iw/2,y-ih/2,iw,ih));
   } else {
    Font f(L"Segoe UI",size*.022F,FontStyleRegular,UnitPixel);StringFormat format;format.SetAlignment(StringAlignmentCenter);format.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(kWheelLabels[static_cast<std::size_t>(item)],-1,&f,RectF(x-icon_size*.7F,y-icon_size/2,icon_size*1.4F,icon_size),&format,&white);
   }
  }
  StringFormat centered;centered.SetAlignment(StringAlignmentCenter);centered.SetLineAlignment(StringAlignmentCenter);
  Font title(L"Segoe UI",size*.033F,FontStyleBold,UnitPixel),detail_font(L"Segoe UI",size*.023F,FontStyleRegular,UnitPixel);
  g.DrawString(chosen<0?L"SELECT WEAPON":kWheelLabels[static_cast<std::size_t>(chosen)],-1,&title,RectF(cx-inner+8,cy-inner*.50F,inner*2-16,inner*.7F),&centered,&white);
  g.DrawString(L"Release to equip\nCentre to cancel",-1,&detail_font,RectF(cx-inner+8,cy+inner*.15F,inner*2-16,inner*.7F),&centered,&muted);
  if(view&(1ULL<<25)) {
   const float panel_width=size*.43F,left=4,top=cy-size*.13F;
   g.FillRectangle(&dark,left,top,panel_width,size*.26F);g.DrawLine(&accent,left,top,left,top+size*.26F);
   const auto code=static_cast<int>((view>>32)&255U)-1;
   std::wstring input;
   if(view&(1ULL<<26)) {
    if(code==1)input=L"Left mouse";else if(code==2)input=L"Right mouse";else {
     wchar_t name[80]{};GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(std::max(0,code)),MAPVK_VK_TO_VSC)<<16),name,80);input=name;
    }
   } else {const auto label=gamepad_binding_name(code,GamepadFamily::generic);input.assign(label.begin(),label.end());}
   input+=L"\n";input+=(view&(1ULL<<24))?L"SHOW FLOYD":L"HIDE FLOYD";
   g.DrawString(input.c_str(),-1,&detail_font,RectF(left+10,top,panel_width-16,size*.26F),&centered,&cyan);
  }
  Graphics screen(dc);screen.DrawImage(&buffer,0,0);
 }
};
} // namespace jfg
