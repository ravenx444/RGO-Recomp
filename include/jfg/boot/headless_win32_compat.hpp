#pragma once

// RGO headless Linux build of the live runtime (JFG_HEADLESS_LIVE).
//
// The live runtime was written for Windows only. A headless Linux build runs
// the same guest CPU, OS, device clocks and host audio pacing without a
// window or a GPU renderer, so the game's timing can be measured in the cloud
// (docs/rgo/framerate.md, "Headless measurement"). This header gives the few
// Win32 calls the shared code makes a Linux meaning: environment lookups read
// the process environment, and window, cursor and message calls do nothing
// because there is no window. Windows builds never include it.

#if defined(_WIN32)
#error "headless_win32_compat.hpp is for non-Windows headless builds only"
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <string>

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

using UINT = unsigned int;
using DWORD = std::uint32_t;
using WORD = std::uint16_t;
using BOOL = int;
using LONG = std::int32_t;
using LONG_PTR = std::intptr_t;
using ULONGLONG = std::uint64_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using HANDLE = void *;
using HWND = void *;
using HMODULE = void *;
using HINSTANCE = void *;
using ATOM = std::uint16_t;
struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

inline constexpr UINT WM_APP = 0x8000U;
inline constexpr UINT GA_ROOT = 2U;
inline constexpr int VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04,
    VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06, VK_BACK = 0x08, VK_TAB = 0x09,
    VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26,
    VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_F1 = 0x70, VK_F11 = 0x7A, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1,
    VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3;

// Environment: the Microsoft CRT and Win32 lookups over getenv.
inline int _dupenv_s(char **value, std::size_t *length, const char *name) {
  *value = nullptr;
  if (length != nullptr) *length = 0U;
  const char *found = std::getenv(name);
  if (found == nullptr) return 0;
  const std::size_t size = std::strlen(found) + 1U;
  *value = static_cast<char *>(std::malloc(size));
  if (*value == nullptr) return ENOMEM;
  std::memcpy(*value, found, size);
  if (length != nullptr) *length = size;
  return 0;
}

inline std::string headless_narrow(const wchar_t *text) {
  std::string out;
  for (; *text != L'\0'; ++text) out.push_back(static_cast<char>(*text));
  return out;
}

inline int _wdupenv_s(wchar_t **value, std::size_t *length,
                      const wchar_t *name) {
  *value = nullptr;
  if (length != nullptr) *length = 0U;
  const char *found = std::getenv(headless_narrow(name).c_str());
  if (found == nullptr) return 0;
  // File paths only: UTF-8 bytes pass through std::filesystem unchanged
  // when each wchar_t carries one byte.
  const std::size_t size = std::strlen(found) + 1U;
  *value = static_cast<wchar_t *>(std::malloc(size * sizeof(wchar_t)));
  if (*value == nullptr) return ENOMEM;
  for (std::size_t index = 0U; index < size; ++index)
    (*value)[index] = static_cast<wchar_t>(static_cast<unsigned char>(found[index]));
  if (length != nullptr) *length = size;
  return 0;
}

inline DWORD GetEnvironmentVariableA(const char *name, char *buffer,
                                     DWORD size) {
  const char *found = std::getenv(name);
  if (found == nullptr) return 0U;
  const auto length = static_cast<DWORD>(std::strlen(found));
  if (length + 1U > size) return length + 1U;
  std::memcpy(buffer, found, length + 1U);
  return length;
}

inline DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *buffer,
                                     DWORD size) {
  const char *found = std::getenv(headless_narrow(name).c_str());
  if (found == nullptr) return 0U;
  const auto length = static_cast<DWORD>(std::strlen(found));
  if (length + 1U > size) return length + 1U;
  for (DWORD index = 0U; index <= length; ++index)
    buffer[index] = static_cast<wchar_t>(static_cast<unsigned char>(found[index]));
  return length;
}

inline DWORD GetCurrentThreadId() noexcept {
  return static_cast<DWORD>(::syscall(SYS_gettid));
}

// No window exists in a headless run.
inline BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) noexcept { return FALSE; }
inline HWND GetAncestor(HWND, UINT) noexcept { return nullptr; }
inline HWND GetParent(HWND) noexcept { return nullptr; }
inline BOOL IsWindow(HWND) noexcept { return FALSE; }
inline HANDLE GetPropW(HWND, const wchar_t *) noexcept { return nullptr; }
inline BOOL SetPropW(HWND, const wchar_t *, HANDLE) noexcept { return FALSE; }
inline HANDLE RemovePropW(HWND, const wchar_t *) noexcept { return nullptr; }
inline BOOL GetClientRect(HWND, RECT *rect) noexcept {
  if (rect != nullptr) *rect = {};
  return FALSE;
}
inline BOOL ClipCursor(const RECT *) noexcept { return TRUE; }
inline void *LoadCursorW(HINSTANCE, const wchar_t *) noexcept { return nullptr; }
inline void *SetCursor(void *) noexcept { return nullptr; }
#define MAKEINTRESOURCEW(value) (reinterpret_cast<const wchar_t *>(static_cast<std::uintptr_t>(value)))
inline HWND GetForegroundWindow() noexcept { return nullptr; }
inline int MapWindowPoints(HWND, HWND, POINT *, UINT) noexcept { return 0; }
inline std::int16_t GetAsyncKeyState(int) noexcept { return 0; }

// MSVC attribute spelling used by the shared code (noinline only).
#define __declspec(attribute) __attribute__((attribute))
