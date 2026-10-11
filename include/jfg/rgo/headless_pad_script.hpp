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
// Optional extra fields drive the mouse and keyboard player: `k:<keys>`
// holds Windows virtual keys (comma-separated, e.g. k:87,65 for W and A;
// 1 and 2 are the left and right mouse buttons) and `m:<dx>,<dy>` moves the
// mouse that many counts every poll (only while the game captures it).

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
    std::vector<int> keys;
    int mouse_x = 0, mouse_y = 0;
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
        if (fields.size() < 8U) return std::nullopt;
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
        for (std::size_t extra = 8U; extra < fields.size(); ++extra) {
            const auto field = fields[extra];
            std::vector<int> values;
            auto list = field.size() > 2U ? field.substr(2U) : std::string_view{};
            while (!list.empty()) {
                const auto comma = list.find(',');
                int value = 0;
                if (!number(list.substr(0U, comma), value)) return std::nullopt;
                values.push_back(value);
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1U);
            }
            if (field.starts_with("k:") && !values.empty()) {
                for (const int key : values)
                    if (key <= 0 || key > 255) return std::nullopt;
                step.keys = values;
            } else if (field.starts_with("m:") && values.size() == 2U &&
                       values[0] >= -1000 && values[0] <= 1000 && values[1] >= -1000 && values[1] <= 1000) {
                step.mouse_x = values[0];
                step.mouse_y = values[1];
            } else
                return std::nullopt;
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
    // A parsed script is never empty (parse_pad_script).
    explicit PadScript(std::vector<PadScriptStep> steps) : steps_(std::move(steps)) {}
    [[nodiscard]] std::size_t step() const noexcept { return index_; }
    // Advances one poll and returns that poll's step.
    [[nodiscard]] const PadScriptStep &next() noexcept {
        if (used_ >= steps_[index_].polls && index_ + 1U < steps_.size()) {
            ++index_;
            used_ = 0U;
        }
        ++used_;
        return steps_[index_];
    }
};

}  // namespace jfg::rgo
