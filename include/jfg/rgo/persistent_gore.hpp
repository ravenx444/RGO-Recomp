#pragma once

// RGO persistent gore (docs/rgo/gore.md): severed limbs, collectible drone
// heads and blood splats stay instead of fading after a few seconds. This
// header holds the profile's gore.ini format, the game addresses and layouts
// the hooks in src/boot/native_boot.cpp use, and the pure bookkeeping that
// decides what to keep, so it can be unit tested without the ROM.

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace jfg::rgo {

// ---------------------------------------------------------------------------
// Game addresses (US). Overlay functions dispatch at overlay << 20 | offset.

// severedLimbControl(object, updateRate), spranim.c: every severed limb and
// drone head. Counts its timer down, then fades the object out and frees it;
// a player touching a collectible kind picks it up.
inline constexpr std::uint32_t kSeveredLimbControl = 0x80027844U;
inline constexpr std::array<std::uint32_t, 2> kSeveredLimbControlPrologue{
    0x27BDFF88U, 0xAFBF001CU};
// objFreeObject(object): queues an object for deletion (objects.c).
inline constexpr std::uint32_t kObjFreeObject = 0x800079B8U;
inline constexpr std::array<std::uint32_t, 2> kObjFreeObjectPrologue{
    0x27BDFFE8U, 0xAFBF0014U};
// objFreeAll(): frees every object (level unload).
inline constexpr std::uint32_t kObjFreeAll = 0x80004838U;
// mainChangeLevel(level, ...).
inline constexpr std::uint32_t kMainChangeLevel = 0x8004665CU;

// Object layout.
inline constexpr std::uint32_t kObjectAlpha = 0x39U;      // u8
inline constexpr std::uint32_t kObjectState = 0x68U;      // pointer
inline constexpr std::uint32_t kObjectFreeQueued = 0xA0U; // u8, set by objFreeObject

// Severed limb state (object + 0x68). severedLimbInit sets the timer to 950;
// severedLimbControl subtracts 2 * updateRate per frame (2 per 60 Hz tick, so
// about 7.9 s at either frame rate), then lowers the object's alpha by the
// same amount (about 2.1 s) and frees it.
inline constexpr std::uint32_t kLimbTimer = 0x14U; // s16
inline constexpr std::uint32_t kLimbKind = 0x17U;  // u8
inline constexpr std::int16_t kLimbTimerStart = 950;
// Kind 0/1: plain gore. Kind 2: drone head (counted towards the head
// totals that unlock extras at 100/200/300). Kind 3/4: pickups (ammo, a
// weapon). Every kind of 2 and above can be picked up.
inline constexpr std::uint8_t kLimbKindHead = 2U;

// A held limb's timer never drops below this. When it is let go, it has
// about 1 s left and then fades out as in the original game.
inline constexpr std::int16_t kLimbHoldTimer = 120;

// Blood: overlay 8 (blood.c). A fixed pool of 64 spurts in the overlay's BSS;
// bloodSpurtAdd silently drops new blood while all 64 are in use. A spurt
// flies (state 1), lands as a growing splat (state 2) and rests (state 3)
// with a timer of 300 ticks; the splat fades over its last 120 ticks.
inline constexpr std::uint32_t kBloodOverlay = 8U;
inline constexpr std::uint32_t kBloodSpurtUpdateAllOffset = 0x40CU;
inline constexpr std::uint32_t kBloodSpurtUpdateAll =
    (kBloodOverlay << 20U) | kBloodSpurtUpdateAllOffset;
inline constexpr std::array<std::uint32_t, 2> kBloodSpurtUpdateAllPrologue{
    0x27BDFEF0U, 0xAFB60068U};
inline constexpr std::uint32_t kBloodPoolBssOffset = 0x20U;
inline constexpr std::uint32_t kBloodOverlayBssSize = 0x15A0U;
inline constexpr std::size_t kBloodSlots = 64U;
inline constexpr std::uint32_t kBloodSlotSize = 0x54U;
inline constexpr std::uint32_t kBloodSlotTimer = 0x3CU;   // s16
inline constexpr std::uint32_t kBloodSlotState = 0x4AU;   // u8
inline constexpr std::uint32_t kBloodSlotSurface = 0x52U; // u8, landed on a surface
inline constexpr std::uint8_t kBloodStateResting = 3U;
inline constexpr std::int16_t kBloodFadeTicks = 120;
// A held splat's timer stays at or above this, so it keeps full colour
// (the timer drops by at most 6 per frame before the fade check).
inline constexpr std::int16_t kBloodHoldTimer = kBloodFadeTicks + 8;

// Memory manager (memory.c). FreeRAM is the main pool's free bytes; below
// 0x14000 the game starts unloading overlays. gMemoryPools[0] holds the main
// pool's slot limit (1600) and the slots in use. Persistent gore is let go,
// oldest first, well before either runs low.
inline constexpr std::uint32_t kFreeRam = 0x800FDD90U;
inline constexpr std::uint32_t kMainPoolMaxSlots = 0x800FD840U;
inline constexpr std::uint32_t kMainPoolUsedSlots = 0x800FD844U;
inline constexpr std::int32_t kLowFreeRam = 0x40000;   // 256 KiB
inline constexpr std::int32_t kLowFreeSlots = 256;

// ---------------------------------------------------------------------------
// Settings: the profile's gore.ini, next to video.ini.

inline constexpr char kGoreSettingsFileName[] = "gore.ini";
inline constexpr std::size_t kGoreSettingsMaximumBytes = 512U;
inline constexpr std::uint32_t kMaximumHeadLimit = 128U;
inline constexpr std::uint32_t kMaximumLimbLimit = 128U;
// Leaves a quarter of the pool for flying blood.
inline constexpr std::uint32_t kMaximumBloodLimit = 48U;

struct GoreSettings {
    bool heads = true;  // drone heads and other pickups stay until picked up
    bool limbs = true;  // severed limbs stay
    bool blood = true;  // blood splats stay
    // How many of each are kept at once; the oldest goes first.
    std::uint32_t head_limit = 64U;
    std::uint32_t limb_limit = 48U;
    std::uint32_t blood_limit = 40U;
    friend bool operator==(const GoreSettings&, const GoreSettings&) = default;
};

// The original game: everything fades (replays, captures).
[[nodiscard]] constexpr GoreSettings original_gore() noexcept {
    GoreSettings settings{};
    settings.heads = settings.limbs = settings.blood = false;
    return settings;
}

[[nodiscard]] inline std::string encode_gore_settings(const GoreSettings& s) {
    std::string text = "version=1";
    text += s.heads ? "\nheads=on" : "\nheads=off";
    text += s.limbs ? "\nlimbs=on" : "\nlimbs=off";
    text += s.blood ? "\nblood=on" : "\nblood=off";
    text += "\nhead_limit=" + std::to_string(s.head_limit);
    text += "\nlimb_limit=" + std::to_string(s.limb_limit);
    text += "\nblood_limit=" + std::to_string(s.blood_limit);
    text += "\n";
    return text;
}

// Reads what encode_gore_settings writes, LF or CRLF. Missing keys keep
// their defaults and unknown keys are ignored. A malformed value, a duplicate
// key or a missing final line break rejects the file, so a partial write
// keeps the last settings.
[[nodiscard]] inline std::optional<GoreSettings> parse_gore_settings(
    std::string_view text) noexcept {
    if (text.size() > kGoreSettingsMaximumBytes)
        return std::nullopt;
    GoreSettings settings{};
    std::uint32_t seen = 0U;
    bool first = true;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        if (end == std::string_view::npos)
            return std::nullopt;
        std::string_view line = text.substr(0U, end);
        text.remove_prefix(end + 1U);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1U);
        if (first) {
            if (line != "version=1")
                return std::nullopt;
            first = false;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || equals == 0U)
            return std::nullopt;
        const std::string_view key = line.substr(0U, equals);
        const std::string_view value = line.substr(equals + 1U);
        constexpr std::array<std::string_view, 6U> keys{
            "heads", "limbs", "blood", "head_limit", "limb_limit", "blood_limit"};
        std::size_t index = 0U;
        while (index < keys.size() && keys[index] != key)
            ++index;
        if (index == keys.size())
            continue;  // A newer setting this build does not know.
        if ((seen & (1U << index)) != 0U)
            return std::nullopt;
        seen |= 1U << index;
        bool ok = true;
        if (index < 3U) {
            bool& target = index == 0U ? settings.heads
                         : index == 1U ? settings.limbs
                                       : settings.blood;
            if (value == "on") target = true;
            else if (value == "off") target = false;
            else ok = false;
        } else {
            std::uint32_t number = 0U;
            const auto result =
                std::from_chars(value.data(), value.data() + value.size(), number);
            const std::uint32_t maximum = index == 3U ? kMaximumHeadLimit
                                        : index == 4U ? kMaximumLimbLimit
                                                      : kMaximumBloodLimit;
            ok = !value.empty() && result.ec == std::errc{} &&
                 result.ptr == value.data() + value.size() && number <= maximum;
            if (ok)
                (index == 3U ? settings.head_limit
                 : index == 4U ? settings.limb_limit
                               : settings.blood_limit) = number;
        }
        if (!ok)
            return std::nullopt;
    }
    if (first)
        return std::nullopt;
    return settings;
}

// ---------------------------------------------------------------------------
// Guest RAM as the runtime keeps it: big-endian, stored as native 32-bit
// words. Accesses outside KSEG0/KSEG1 RAM fail instead of throwing.

class GoreMemory {
public:
    GoreMemory(std::uint8_t* rdram, std::size_t size) noexcept
        : rdram_(rdram), size_(size & ~std::size_t{3U}) {}
    [[nodiscard]] bool valid(std::uint32_t address, std::uint32_t bytes) const noexcept {
        const std::uint32_t segment = address & 0xE0000000U;
        const std::size_t offset = address & 0x1FFFFFFFU;
        return rdram_ != nullptr && (segment == 0x80000000U || segment == 0xA0000000U) &&
               offset <= size_ && bytes <= size_ - offset;
    }
    [[nodiscard]] std::optional<std::uint8_t> u8(std::uint32_t address) const noexcept {
        if (!valid(address, 1U))
            return std::nullopt;
        return rdram_[byte(address)];
    }
    [[nodiscard]] std::optional<std::int16_t> s16(std::uint32_t address) const noexcept {
        if ((address & 1U) != 0U || !valid(address, 2U))
            return std::nullopt;
        return static_cast<std::int16_t>(
            static_cast<std::uint16_t>((rdram_[byte(address)] << 8U) | rdram_[byte(address + 1U)]));
    }
    [[nodiscard]] std::optional<std::uint32_t> u32(std::uint32_t address) const noexcept {
        if ((address & 3U) != 0U || !valid(address, 4U))
            return std::nullopt;
        std::uint32_t value = 0U;
        for (std::uint32_t index = 0U; index < 4U; ++index)
            value = (value << 8U) | rdram_[byte(address + index)];
        return value;
    }
    bool put_s16(std::uint32_t address, std::int16_t value) noexcept {
        if ((address & 1U) != 0U || !valid(address, 2U))
            return false;
        const auto bits = static_cast<std::uint16_t>(value);
        rdram_[byte(address)] = static_cast<std::uint8_t>(bits >> 8U);
        rdram_[byte(address + 1U)] = static_cast<std::uint8_t>(bits);
        return true;
    }

private:
    [[nodiscard]] static std::size_t byte(std::uint32_t address) noexcept {
        return (address & 0x1FFFFFFFU) ^ 3U;
    }
    std::uint8_t* rdram_;
    std::size_t size_;
};

// ---------------------------------------------------------------------------
// Bookkeeping.

enum class GoreKind : std::uint8_t { limb, head };

[[nodiscard]] constexpr GoreKind limb_kind(std::uint8_t kind) noexcept {
    return kind >= kLimbKindHead ? GoreKind::head : GoreKind::limb;
}

// Severed limbs and heads the game would fade out, by object address, in the
// order they were first seen. A held object's timer is kept from running
// out; past the limit the oldest is let go and runs its original timer.
class PersistentObjects {
public:
    // Called each frame from the object's control function. Returns whether
    // to hold it this frame.
    bool hold(std::uint32_t object, GoreKind kind, std::uint32_t limit) {
        auto found = entries_.find(object);
        if (found == entries_.end()) {
            found = entries_.emplace(object, Entry{next_++, kind, true}).first;
            ++held_[index(kind)];
        } else if (found->second.kind != kind) {
            // The address now holds another limb: start over.
            forget(object);
            return hold(object, kind, limit);
        }
        while (held_[index(kind)] > limit && release_oldest(kind)) {
        }
        return found->second.held;
    }
    // The object was freed (picked up, faded out or unloaded).
    void forget(std::uint32_t object) {
        const auto found = entries_.find(object);
        if (found == entries_.end())
            return;
        if (found->second.held)
            --held_[index(found->second.kind)];
        entries_.erase(found);
    }
    void clear() {
        entries_.clear();
        held_ = {};
    }
    // Lets the oldest held object of this kind go. False when none is held.
    bool release_oldest(GoreKind kind) {
        Entry* oldest = nullptr;
        for (auto& [address, entry] : entries_)
            if (entry.held && entry.kind == kind &&
                (oldest == nullptr || entry.order < oldest->order))
                oldest = &entry;
        if (oldest == nullptr)
            return false;
        oldest->held = false;
        --held_[index(kind)];
        return true;
    }
    // Memory is low: plain gore goes first, then heads.
    bool release_oldest() {
        return release_oldest(GoreKind::limb) || release_oldest(GoreKind::head);
    }
    [[nodiscard]] std::uint32_t held(GoreKind kind) const noexcept {
        return held_[index(kind)];
    }
    [[nodiscard]] std::size_t tracked() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::uint64_t order = 0U;
        GoreKind kind = GoreKind::limb;
        bool held = true;
    };
    static constexpr std::size_t index(GoreKind kind) noexcept {
        return kind == GoreKind::head ? 1U : 0U;
    }
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::array<std::uint32_t, 2> held_{};
    std::uint64_t next_ = 1U;
};

// The blood pool's resting splats, by slot, in the order they came to rest.
class PersistentSplats {
public:
    // resting[i]: slot i is a splat at rest on a surface. Fills hold[i] with
    // whether to keep slot i's timer from running out; past the limit the
    // oldest resting splats are let go and fade as in the original game.
    void update(const std::array<bool, kBloodSlots>& resting, std::uint32_t limit,
                std::array<bool, kBloodSlots>& hold) {
        std::uint32_t held = 0U;
        for (std::size_t slot = 0U; slot < kBloodSlots; ++slot) {
            if (!resting[slot]) {
                order_[slot] = 0U;
                released_[slot] = false;
            } else if (order_[slot] == 0U) {
                order_[slot] = next_++;
            }
            if (resting[slot] && !released_[slot])
                ++held;
        }
        while (held > limit) {
            std::size_t oldest = kBloodSlots;
            for (std::size_t slot = 0U; slot < kBloodSlots; ++slot)
                if (order_[slot] != 0U && !released_[slot] &&
                    (oldest == kBloodSlots || order_[slot] < order_[oldest]))
                    oldest = slot;
            released_[oldest] = true;
            --held;
        }
        for (std::size_t slot = 0U; slot < kBloodSlots; ++slot)
            hold[slot] = order_[slot] != 0U && !released_[slot];
    }
    void clear() {
        order_ = {};
        released_ = {};
    }

private:
    std::array<std::uint64_t, kBloodSlots> order_{};
    std::array<bool, kBloodSlots> released_{};
    std::uint64_t next_ = 1U;
};

} // namespace jfg::rgo
