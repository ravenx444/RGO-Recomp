#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <windows.h>
struct FrontendUiState {
  bool playing = false, busy = false, fullscreen = false, mods = false;
  std::wstring rom, runtime, status;
  bool pauseInactive = false;
  bool widescreen = true;
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
