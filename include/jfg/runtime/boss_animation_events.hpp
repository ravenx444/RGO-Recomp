#pragma once
#include <array>
#include <cstdint>

namespace jfg {
// Fat Bob's AI advances animation every two ticks. Event scanning runs every
// displayed game frame, so its unchanged interval must only be consumed once.
// Track the interval, not the event number: several events may share a frame.
class BossAnimationEvents {
public:
    using Interval = std::array<std::uint32_t, 9>;
    [[nodiscard]] bool consume(const Interval& interval) noexcept {
        if (valid_ && interval == previous_)
            return false;
        previous_ = interval;
        valid_ = true;
        return true;
    }
    void reset() noexcept { valid_ = false; }
private:
    Interval previous_{};
    bool valid_ = false;
};
}
