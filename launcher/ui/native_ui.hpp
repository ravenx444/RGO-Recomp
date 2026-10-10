#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <windows.h>
#include "jfg/renderer/frame_stats.hpp"
#include "jfg/renderer/video_settings.hpp"
struct FrontendUiState {
  bool playing = false, busy = false, fullscreen = false, mods = false;
  std::wstring rom, runtime, status;
  bool pauseInactive = false;
  // RGO display settings (video.ini), the game area size in pixels and the
  // game's last reported FPS counter and its Detailed figures
  // (frame_stats.hpp; none yet or stale when frameStatsValid is false).
  jfg::VideoSettings video{};
  int viewWidth = 0, viewHeight = 0;
  bool frameStatsValid = false;
  jfg::FrameCounter frameCounter{};
  jfg::FrameStats frameStats{};
};
bool FrontendUiInit(HWND, const std::filesystem::path &,
                    std::function<void(const std::string &)>, const std::string &tool = "");
int FrontendLiveToolEntry(HINSTANCE, int);
void FrontendUiShutdown();
void FrontendUiFrame(const FrontendUiState &);
void FrontendUiResize();
void FrontendUiMessage(UINT, WPARAM, LPARAM);
void FrontendUiPanel(const std::string &);
bool FrontendUiCapturing();
void FrontendUiToggleMute();

bool FrontendUiModalOpen();
