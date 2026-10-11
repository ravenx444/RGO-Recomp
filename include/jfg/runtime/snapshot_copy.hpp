#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <cstdint>
#include <utility>
#include <vector>
namespace jfg {
// RGO host speed: the RDRAM window a present refreshes in the renderer's RAM
// copy: from the 4 KB page holding the displayed framebuffer for 640x480 at
// 32 bits (the largest picture the game sets up) plus a page, clamped to RAM.
inline constexpr std::uint32_t kPresentImportBytes = 640U * 480U * 4U + 4096U;
[[nodiscard]] constexpr std::pair<std::uint32_t, std::uint32_t> present_import_window(
    const std::uint32_t framebuffer, const std::uint32_t rdram_bytes) noexcept {
  const std::uint32_t begin = (framebuffer & 0x00FFFFFFU & ~0xFFFU) < rdram_bytes
      ? (framebuffer & 0x00FFFFFFU & ~0xFFFU) : 0U;
  const std::uint32_t end = rdram_bytes - begin < kPresentImportBytes
      ? rdram_bytes : begin + kPresentImportBytes;
  return {begin, end};
}
// RGO host speed: the 4 KB chunks in which a graphics snapshot differs from
// the snapshot before it, so the renderer imports only those.
inline constexpr std::size_t kSnapshotChunkBytes = 4096U;
class SnapshotChanges {
public:
  void reset(const std::size_t bytes) {
    flags_.assign(bytes / kSnapshotChunkBytes, 0U);
    chunks_.clear();
  }
  void mark_chunk(const std::size_t index) {
    if (index < flags_.size() && flags_[index] == 0U) {
      flags_[index] = 1U;
      chunks_.push_back(static_cast<std::uint32_t>(index));
    }
  }
  // Every chunk that [offset, offset + bytes) touches.
  void mark(const std::size_t offset, const std::size_t bytes) {
    if (bytes == 0U)
      return;
    for (std::size_t index = offset / kSnapshotChunkBytes;
         index <= (offset + bytes - 1U) / kSnapshotChunkBytes; ++index)
      mark_chunk(index);
  }
  [[nodiscard]] bool contains(const std::size_t index) const noexcept {
    return index < flags_.size() && flags_[index] != 0U;
  }
  [[nodiscard]] std::span<const std::uint32_t> chunks() const noexcept { return chunks_; }

private:
  std::vector<std::uint8_t> flags_;
  std::vector<std::uint32_t> chunks_;
};

inline void copy_changed_snapshot_bytes(const std::span<const std::byte> source,
                                        const std::span<std::byte> destination,
                                        SnapshotChanges *const changes = nullptr) {
  if(source.data()==destination.data())return;
  constexpr std::size_t chunk=kSnapshotChunkBytes;
  for(std::size_t offset=0;offset<source.size();offset+=chunk) {
    const auto count=(std::min)(chunk,source.size()-offset);
    if(std::memcmp(source.data()+offset,destination.data()+offset,count)!=0) {
      std::memcpy(destination.data()+offset,source.data()+offset,count);
      if(changes!=nullptr)changes->mark_chunk(offset/chunk);
    }
  }
}
}
