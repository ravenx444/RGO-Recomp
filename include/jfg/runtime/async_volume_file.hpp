#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace jfg {
class AsyncVolumeFile {
public:
  // Files larger than maximum_bytes are ignored (the last good value stays).
  explicit AsyncVolumeFile(std::size_t maximum_bytes = 128U) : maximum_bytes_(maximum_bytes) {}
  ~AsyncVolumeFile() { finish(); }
  void start(std::filesystem::path path) {
    path_ = std::move(path);
    refresh(); // Called before the first audio buffer can play.
    worker_ = std::thread([this] {
      std::unique_lock lock(mutex_);
      while (!stop_) {
        if (changed_.wait_for(lock,std::chrono::milliseconds(100),[this]{return stop_;})) break;
        lock.unlock(); refresh(); lock.lock();
      }
    });
  }
  std::shared_ptr<const std::string> latest() {
    std::lock_guard lock(mutex_); return latest_;
  }
  void finish() noexcept {
    if (!worker_.joinable()) return;
    {std::lock_guard lock(mutex_);stop_=true;changed_.notify_one();}
    worker_.join();
  }
private:
  void refresh() noexcept {
    try {
      const HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ,
          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL,nullptr);
      if (file == INVALID_HANDLE_VALUE) return;
      std::vector<char> buffer(maximum_bytes_+1U); DWORD count=0;
      const bool read=ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr)!=0;
      CloseHandle(file); if (!read || count>maximum_bytes_) return;
      auto value=std::make_shared<const std::string>(buffer.data(),count);
      std::lock_guard lock(mutex_); latest_=std::move(value);
    } catch (...) {}
  }
  std::size_t maximum_bytes_;
  std::filesystem::path path_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::shared_ptr<const std::string> latest_;
  bool stop_=false;
  std::thread worker_;
};
}
