#include "jfg/runtime/input_replay.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace jfg {
namespace {

constexpr std::uintmax_t kMaximumReplayBytes = 8U * 1024U * 1024U;

template <typename Value>
[[nodiscard]] bool parse_integer(
    const std::string_view text,
    Value& output,
    const int base = 10) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), output, base);
    return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] bool parse_record(
    const std::string_view line,
    const bool has_connection,
    const bool has_pc,
    InputReplayEvent& output) noexcept {
    std::array<std::string_view, 9U> fields{};
    std::size_t field = 0U;
    std::size_t start = 0U;
    for (std::size_t index = 0U; index <= line.size(); ++index) {
        if (index != line.size() && line[index] != ',') {
            continue;
        }
        if (field == fields.size()) {
            return false;
        }
        fields[field++] = line.substr(start, index - start);
        start = index + 1U;
    }
    const std::size_t expected_fields = has_pc ? 9U : has_connection ? 6U : 5U;
    if (field != expected_fields) {
        return false;
    }
    unsigned connected = 1U;
    unsigned buttons = 0U;
    int stick_x = 0;
    int stick_y = 0;
    if (!parse_integer(fields[0], output.first_retrace) ||
        !parse_integer(fields[1], output.end_retrace) ||
        (has_connection && !parse_integer(fields[2], connected)) ||
        !parse_integer(fields[has_connection ? 3U : 2U], buttons, 16) ||
        !parse_integer(fields[has_connection ? 4U : 3U], stick_x) ||
        !parse_integer(fields[has_connection ? 5U : 4U], stick_y) ||
        output.first_retrace >= output.end_retrace || connected > 1U ||
        buttons > 0xFFFFU ||
        stick_x < (std::numeric_limits<std::int8_t>::min)() ||
        stick_x > (std::numeric_limits<std::int8_t>::max)() ||
        stick_y < (std::numeric_limits<std::int8_t>::min)() ||
        stick_y > (std::numeric_limits<std::int8_t>::max)() ||
        (connected == 0U && (buttons != 0U || stick_x != 0 || stick_y != 0))) {
        return false;
    }
    unsigned mode=0;int look_x=0,look_y=0;
    if(has_pc && (!parse_integer(fields[6],mode) || mode>5 ||
       !parse_integer(fields[7],look_x) || !parse_integer(fields[8],look_y) ||
       look_x < -32768 || look_x>32767 || look_y < -32768 || look_y>32767 ||
       ((mode==0 || !connected) && (look_x || look_y || mode)) ||
       (mode!=3 && mode!=5 && (look_x < -127 || look_x>127 || look_y < -127 || look_y>127))))
      return false;
    output.sample.pc_mode=static_cast<std::uint8_t>(mode);
    output.sample.look_x=static_cast<std::int16_t>(look_x);
    output.sample.look_y=static_cast<std::int16_t>(look_y);
    output.sample.connected = connected != 0U;
    output.sample.buttons = static_cast<std::uint16_t>(buttons);
    output.sample.stick_x = static_cast<std::int8_t>(stick_x);
    output.sample.stick_y = static_cast<std::int8_t>(stick_y);
    return true;
}

}  // namespace

InputReplayError DeterministicInputReplay::load(
    const std::filesystem::path& path,
    DeterministicInputReplay& output,
    const InputReplayOrder order) {
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return InputReplayError::io_error;
    }
    if (size > kMaximumReplayBytes) {
        return InputReplayError::too_large;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return InputReplayError::io_error;
    }
    std::string line;
    if (!std::getline(stream, line)) {
        return InputReplayError::invalid_header;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const bool has_pc = line == kInputReplayHeaderV3;
    const bool has_connection = has_pc || line == kInputReplayHeader;
    if (!has_connection && line != kInputReplayHeaderV1) {
        return InputReplayError::invalid_header;
    }
    std::vector<InputReplayEvent> parsed;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || parsed.size() == kMaximumInputReplayEvents) {
            return InputReplayError::invalid_record;
        }
        InputReplayEvent event;
        if (!parse_record(line, has_connection, has_pc, event)) {
            return InputReplayError::invalid_record;
        }
        if (!parsed.empty() && event.first_retrace < parsed.back().end_retrace) {
            const auto& previous = parsed.back();
            const bool repeated_poll = order == InputReplayOrder::recorded_poll &&
                event.first_retrace == previous.first_retrace &&
                event.end_retrace == previous.end_retrace &&
                event.end_retrace - event.first_retrace == 1U;
            if (!repeated_poll) return InputReplayError::invalid_order;
        }
        parsed.push_back(event);
    }
    if (!stream.eof()) {
        return InputReplayError::io_error;
    }
    output.events_ = std::move(parsed);
    return InputReplayError::none;
}

ControllerReplaySample DeterministicInputReplay::sample_at(
    const std::uint64_t retrace) const noexcept {
    const auto event = std::upper_bound(
        events_.begin(), events_.end(), retrace,
        [](const std::uint64_t value, const InputReplayEvent& candidate) {
            return value < candidate.first_retrace;
        });
    if (event == events_.begin()) {
        return {};
    }
    const InputReplayEvent& candidate = *std::prev(event);
    return retrace < candidate.end_retrace ? candidate.sample
                                           : ControllerReplaySample{};
}

ControllerReplaySample DeterministicInputReplay::sample_by_poll(
    const std::uint64_t poll) const noexcept {
    return poll < events_.size()
               ? events_[static_cast<std::size_t>(poll)].sample
               : ControllerReplaySample{};
}

const std::vector<InputReplayEvent>& DeterministicInputReplay::events()
    const noexcept {
    return events_;
}

}  // namespace jfg
