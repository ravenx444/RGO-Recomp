#include "jfg/runtime/input_replay.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

class TemporaryReplay final {
public:
    explicit TemporaryReplay(const std::string_view contents) {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("jfg-input-replay-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1U)) + ".txt");
        std::ofstream stream(path_, std::ios::binary);
        stream.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error("failed to write input replay fixture");
        }
    }

    ~TemporaryReplay() noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_valid_replay() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v1\r\n"
        "1800,1810,1000,0,0\r\n"
        "2050,2060,8000,-12,34\r\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
              jfg::InputReplayError::none,
          "valid replay should load");
    check(replay.events().size() == 2U, "valid replay event count");
    check(replay.sample_at(1799U).neutral(), "pre-event sample is neutral");
    check(replay.sample_at(1800U).buttons == 0x1000U,
          "first boundary is inclusive");
    check(replay.sample_at(1809U).buttons == 0x1000U,
          "event remains active before end");
    check(replay.sample_at(1810U).neutral(), "end boundary is exclusive");
    check(replay.sample_at(2055U) ==
              jfg::ControllerReplaySample{true, 0x8000U, -12, 34},
          "later event preserves buttons and stick values");
}

void test_connection_transitions() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v2\n"
        "100,110,0,0000,0,0\n"
        "120,130,1,8000,12,-34\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
              jfg::InputReplayError::none,
          "connection replay should load");
    check(!replay.sample_at(105U).connected,
          "disconnect window is observable");
    check(replay.sample_at(110U).connected,
          "controller reconnects after disconnect window");
    check(replay.sample_at(125U) ==
              jfg::ControllerReplaySample{true, 0x8000U, 12, -34},
          "connected v2 sample preserves controls");
}

void test_poll_replay_eof_is_neutral() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v2\n"
        "0,1,1,0000,0,0\n"
        "1,100,1,8000,12,-34\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
              jfg::InputReplayError::none,
          "poll replay should load");
    check(replay.sample_by_poll(0U).neutral(), "first poll is neutral");
    check(replay.sample_by_poll(1U) ==
              jfg::ControllerReplaySample{true, 0x8000U, 12, -34},
          "last recorded poll preserves buttons and stick");
    check(replay.sample_by_poll(2U).neutral(),
          "first poll after EOF is neutral despite long final interval");
    check(replay.sample_by_poll(100U).neutral(),
          "later polls after EOF remain neutral");
}

void test_long_manual_recording() {
    std::string contents = "jfg-phase8-input-v2\n";
    for (std::size_t i = 0U; i < jfg::kMaximumInputReplayEvents; ++i) {
        contents += std::to_string(i * 2U) + "," +
                    std::to_string(i * 2U + 1U) + ",1,8000,12,-34\n";
    }
    check(contents.size() > 1024U * 1024U,
          "manual recording exceeds the former one MiB limit");
    jfg::DeterministicInputReplay replay;
    const TemporaryReplay valid(contents);
    check(jfg::DeterministicInputReplay::load(valid.path(), replay) ==
              jfg::InputReplayError::none, "long manual recording loads");
    check(replay.sample_by_poll(jfg::kMaximumInputReplayEvents - 1U) ==
              jfg::ControllerReplaySample{true, 0x8000U, 12, -34},
          "final input of long recording remains intact");
    contents += "900000,900001,1,0,0,0\n";
    const TemporaryReplay excess(contents);
    check(jfg::DeterministicInputReplay::load(excess.path(), replay) ==
              jfg::InputReplayError::invalid_record,
          "event limit still rejects oversized event counts");
    const TemporaryReplay too_large(std::string(8U * 1024U * 1024U + 1U, 'x'));
    check(jfg::DeterministicInputReplay::load(too_large.path(), replay) ==
              jfg::InputReplayError::too_large, "byte limit remains enforced");
}

void test_independent_look() {
    const TemporaryReplay valid("jfg-phase8-input-v3\n100,120,1,2010,80,-40,3,180,-90\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,"v3 independent look loads");
    const auto sample=replay.sample_at(105);
    check(sample.stick_x==80 && sample.stick_y==-40 && sample.look_x==180 && sample.look_y==-90 && sample.pc_mode==3,"movement and mouse look stay independent");
    check(replay.sample_at(120).pc_mode==0 && replay.sample_at(120).neutral(),"look ends at event boundary");
    const auto before=replay.events();
    // RGO: mode 5 is direct mouse/key aim angles with the stock camera.
    const TemporaryReplay direct("jfg-phase8-input-v3\n100,120,1,10,0,0,5,900,-300\n");
    check(jfg::DeterministicInputReplay::load(direct.path(),replay)==jfg::InputReplayError::none &&
          replay.sample_at(105).pc_mode==5 && replay.sample_at(105).look_x==900,"mode 5 angle look loads");
    // RGO: mode 6 is the twin-stick look rate (1/16 angle units per tick).
    const TemporaryReplay twin("jfg-phase8-input-v3\n100,120,1,10,40,0,6,1820,-910\n");
    check(jfg::DeterministicInputReplay::load(twin.path(),replay)==jfg::InputReplayError::none &&
          replay.sample_at(105).pc_mode==6 && replay.sample_at(105).look_x==1820 && replay.sample_at(105).stick_x==40,"mode 6 twin-stick rate loads");
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,"v3 independent look reloads");
    for(const auto* row:{"1,2,1,0,0,0,7,0,0","1,2,1,0,0,0,0,1,0",
        "1,2,0,0,0,0,3,1,0","1,2,1,0,0,0,2,128,0","1,2,1,0,0,0,3,32768,0",
        "1,2,1,0,0,0,3,2","1,2,1,0,0,0,3,1,2,3"}) {
      const TemporaryReplay bad(std::string("jfg-phase8-input-v3\n")+row+"\n");
      check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record && replay.events()==before,"invalid v3 leaves active replay intact");
    }
}


void test_repeated_poll_records() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v3\n"
        "21,22,1,8000,0,0,3,12,0\n"
        "21,22,1,0000,0,0,3,-7,0\n"
        "22,23,1,0010,0,0,0,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
        jfg::InputReplayError::invalid_order, "retrace replay still rejects ambiguous repeated intervals");
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay,
        jfg::InputReplayOrder::recorded_poll) == jfg::InputReplayError::none,
        "original OS may poll twice in one retrace");
    check(replay.events().size() == 3 && replay.sample_by_poll(0).buttons == 0x8000 &&
        replay.sample_by_poll(0).look_x == 12 && replay.sample_by_poll(1).look_x == -7 &&
        replay.sample_by_poll(2).buttons == 0x0010 && replay.sample_by_poll(3).neutral(),
        "recorded poll order preserves independent transitions and EOF");
    const auto before = replay.events();
    for (const auto* rows : {
        "21,23,1,0,0,0\n22,23,1,0,0,0\n",
        "21,22,1,0,0,0\n20,21,1,0,0,0\n",
        "21,23,1,0,0,0\n21,23,1,0,0,0\n"}) {
        const TemporaryReplay invalid(std::string("jfg-phase8-input-v2\n")+rows);
        check(jfg::DeterministicInputReplay::load(invalid.path(), replay,
            jfg::InputReplayOrder::recorded_poll) == jfg::InputReplayError::invalid_order &&
            replay.events() == before, "poll mode rejects reordered or overlapping intervals atomically");
    }
}

void test_rejects_invalid_records() {
    for (const std::string_view contents : {
             "wrong-header\n1800,1810,1000,0,0\n",
             "jfg-phase8-input-v1\n1800,1800,1000,0,0\n",
             "jfg-phase8-input-v1\n1800,1810,10000,0,0\n",
             "jfg-phase8-input-v1\n1800,1810,1000,-129,0\n",
             "jfg-phase8-input-v1\n1800,1810,1000,0,0\n1809,1820,0,0,0\n",
             "jfg-phase8-input-v2\n1800,1810,2,0000,0,0\n",
             "jfg-phase8-input-v2\n1800,1810,0,8000,0,0\n",
         }) {
        const TemporaryReplay fixture(contents);
        jfg::DeterministicInputReplay replay;
        check(jfg::DeterministicInputReplay::load(fixture.path(), replay) !=
                  jfg::InputReplayError::none,
              "invalid replay should fail closed");
    }
}

}  // namespace

int main() {
    test_valid_replay();
    test_independent_look();
    test_long_manual_recording();
    test_connection_transitions();
    test_poll_replay_eof_is_neutral();
    test_repeated_poll_records();
    test_rejects_invalid_records();
    return failures == 0 ? 0 : 1;
}
