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
  // game's last reported frame rates in tenths (-1: none yet): its own
  // frames and the frames presented to the display.
  jfg::VideoSettings video{};
  int viewWidth = 0, viewHeight = 0;
  int fpsTenths = -1, displayFpsTenths = -1;
  // FPS counter "Detailed" (jfg/renderer/frame_stats.hpp).
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
