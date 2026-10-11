#pragma once

// RGO headless checks: a scripted virtual controller.
//
// The headless Linux build (docs/rgo/framerate.md) has no controller. With
// JFG_RGO_PAD_SCRIPT=<file> the new-game autopilot still drives the title
// screens, then, once gameplay has run for a while, this script takes over
// as a connected SDL-layout controller in slot 0. It goes through the same
// controller mapping, controller scheme and look hooks as a real pad, so the
// input path can be checked with JFG_RGO_PLAYER_TRACE without a player.
//
// One step per line: `<polls> <lx> <ly> <rx> <ry> <lt> <rt> <buttons>`.
// Axes are SDL values (-32768..32767, Y down), buttons a comma-separated
// list of SDL button indices (0 A/Cross ... 14 D-pad right) or `-` for none.
// `#` starts a comment. The last step holds once the script ends.

#include "jfg/runtime/controller_mapping.hpp"
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace jfg::rgo {

struct PadScriptStep {
    std::uint32_t polls = 0U;
    StandardControllerSample pad{};
};

[[nodiscard]] inline std::optional<std::vector<PadScriptStep>> parse_pad_script(std::string_view text) {
    std::vector<PadScriptStep> steps;
    const auto number = [](std::string_view field, int &value) {
        const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size();
    };
    while (!text.empty()) {
        const auto newline = text.find('\n');
        auto line = text.substr(0U, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1U);
        if (const auto comment = line.find('#'); comment != std::string_view::npos) line = line.substr(0U, comment);
        std::vector<std::string_view> fields;
        while (!line.empty()) {
            const auto start = line.find_first_not_of(" \t\r");
            if (start == std::string_view::npos) break;
            line = line.substr(start);
            const auto end = line.find_first_of(" \t\r");
            fields.push_back(line.substr(0U, end));
            line = end == std::string_view::npos ? std::string_view{} : line.substr(end);
        }
        if (fields.empty()) continue;
        if (fields.size() != 8U) return std::nullopt;
        PadScriptStep step;
        int polls = 0;
        if (!number(fields[0], polls) || polls <= 0) return std::nullopt;
        step.polls = static_cast<std::uint32_t>(polls);
        for (std::size_t axis = 0U; axis < 6U; ++axis) {
            int value = 0;
            if (!number(fields[axis + 1U], value) || value < -32768 || value > 32767) return std::nullopt;
            step.pad.axes[axis] = value;
        }
        auto buttons = fields[7];
        if (buttons != "-") {
            while (!buttons.empty()) {
                const auto comma = buttons.find(',');
                int button = 0;
                if (!number(buttons.substr(0U, comma), button) || button < 0 || button > 16) return std::nullopt;
                step.pad.buttons[static_cast<std::size_t>(button)] = true;
                buttons = comma == std::string_view::npos ? std::string_view{} : buttons.substr(comma + 1U);
            }
        }
        steps.push_back(step);
    }
    if (steps.empty()) return std::nullopt;
    return steps;
}

// Steps through a parsed script, one call per controller poll.
class PadScript {
    std::vector<PadScriptStep> steps_;
    std::size_t index_ = 0U;
    std::uint32_t used_ = 0U;

public:
    explicit PadScript(std::vector<PadScriptStep> steps) : steps_(std::move(steps)) {}
    [[nodiscard]] std::size_t step() const noexcept { return index_; }
    [[nodiscard]] StandardControllerSample next() noexcept {
        if (steps_.empty()) return {};
        if (used_ >= steps_[index_].polls && index_ + 1U < steps_.size()) {
            ++index_;
            used_ = 0U;
        }
        ++used_;
        return steps_[index_].pad;
    }
};

}  // namespace jfg::rgo
