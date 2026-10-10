// RGO frame rate work (docs/rgo/framerate.md): the FPS counter message, the
// fast clock's event ordering, the one-trap MMIO decoder and the headless
// new-game autopilot.
#include "jfg/boot/mmio_instruction.hpp"
#include "jfg/boot/reference_event_commit.hpp"
#include "jfg/renderer/frame_stats.hpp"
#include "jfg/rgo/headless_autopilot.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace {

void require(const bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

// Sparse big-endian guest memory for the autopilot.
struct FakeMemory final : jfg::rgo::AutopilotMemory {
    std::map<std::uint32_t, std::uint8_t> bytes;
    void put32(std::uint32_t address, std::uint32_t value) {
        for (std::uint32_t index = 0U; index < 4U; ++index)
            bytes[address + index] = static_cast<std::uint8_t>(value >> (24U - index * 8U));
    }
    std::optional<std::uint8_t> u8(std::uint32_t address) const override {
        const auto found = bytes.find(address);
        if (found == bytes.end()) return std::uint8_t{0U};
        return found->second;
    }
    std::optional<std::uint32_t> u32(std::uint32_t address) const override {
        std::uint32_t value = 0U;
        for (std::uint32_t index = 0U; index < 4U; ++index)
            value = (value << 8U) | *u8(address + index);
        return value;
    }
};

void frame_stats() {
    using namespace jfg;
    require(frame_rate_tenths(30U, 500'000U) == 600U, "30 frames in 0.5 s is 60.0");
    require(frame_rate_tenths(0U, 500'000U) == 0U && frame_rate_tenths(5U, 0U) == 0U,
            "empty windows report 0");
    require(frame_time_tenth_ms(16'667U) == 167U && frame_time_tenth_ms(0U) == 0U,
            "frame times in tenths of a millisecond");
    require(frame_time_tenth_ms(10'000'000U) == 0xFFFFU, "frame times saturate");
    const FrameCounter stats{600U, 599U, 1440U, 69U, 71U};
    require(unpack_frame_counter(pack_frame_counter(stats)) == stats, "the message round-trips");
    const FrameCounter large{0xFFFFFFU, 0xFFFFFFU, 0xFFFFFFU, 0xFFFFU, 0xFFFFU};
    require(unpack_frame_counter(pack_frame_counter(large)) == large, "maximum values round-trip");
    require((pack_frame_counter(large).second >> 56U) == 0U,
            "the second word stays clear of an LPARAM's sign bit");
    require(format_frame_counter(stats) == "Game 60 | Render 60 | Display 144 | 6.9-7.1 ms",
            "counter text");
}

void event_ties() {
    using jfg::boot::ReferenceEvent;
    using jfg::boot::ReferenceEventCandidate;
    const std::array<ReferenceEventCandidate, 3> tied{{
        {ReferenceEvent::vi, 100U}, {ReferenceEvent::ai, 100U}, {ReferenceEvent::pi, 200U}}};
    const auto strict = jfg::boot::select_reference_event(tied, 150U);
    require(!strict.qualified, "the original clock refuses to guess a tie");
    const auto ordered = jfg::boot::select_reference_event(tied, 150U, true);
    require(ordered.qualified && ordered.source == ReferenceEvent::vi,
            "the fast clock takes the earlier-listed source");
    const std::array<ReferenceEventCandidate, 2> distinct{{
        {ReferenceEvent::ai, 90U}, {ReferenceEvent::vi, 100U}}};
    require(jfg::boot::select_reference_event(distinct, 150U, true).source == ReferenceEvent::ai,
            "different deadlines stay chronological");
    const std::array<ReferenceEventCandidate, 2> pair{{
        {ReferenceEvent::dp, 100U}, {ReferenceEvent::sp, 100U}}};
    const auto rsp = jfg::boot::select_reference_event(pair, 100U);
    require(rsp.qualified && rsp.source == ReferenceEvent::sp, "SP before DP is unchanged");
}

void mmio_moves() {
    using jfg::boot::MmioInstructionKind;
    struct Case {
        std::array<std::uint8_t, 16> code;
        MmioInstructionKind kind;
        std::uint8_t length, reg;
        std::uint32_t immediate;
        const char *name;
    };
    const Case cases[] = {
        // GCC: movslq 0x0(%rbp,%rdx,1),%rdx (a guest lw into a 64-bit GPR)
        {{0x48, 0x63, 0x54, 0x15, 0x00}, MmioInstructionKind::load_sign_extend, 5, 2, 0, "movsxd disp8"},
        // MSVC style: movsxd rax, dword ptr [rdx+rcx]
        {{0x48, 0x63, 0x04, 0x0A}, MmioInstructionKind::load_sign_extend, 4, 0, 0, "movsxd sib"},
        {{0x4C, 0x63, 0x04, 0x0A}, MmioInstructionKind::load_sign_extend, 4, 8, 0, "movsxd r8"},
        {{0x8B, 0x04, 0x0A}, MmioInstructionKind::load_zero_extend, 3, 0, 0, "mov eax, [rdx+rcx]"},
        {{0x44, 0x8B, 0x44, 0x15, 0x00}, MmioInstructionKind::load_zero_extend, 5, 8, 0, "mov r8d"},
        {{0x8B, 0x40, 0x10}, MmioInstructionKind::load_zero_extend, 3, 0, 0, "mov eax, [rax+16]"},
        {{0x8B, 0x05, 0x10, 0x20, 0x30, 0x40}, MmioInstructionKind::load_zero_extend, 6, 0, 0, "rip-relative"},
        {{0x8B, 0x04, 0x25, 0x00, 0x00, 0x30, 0x24}, MmioInstructionKind::load_zero_extend, 7, 0, 0, "sib without base"},
        {{0x8B, 0x84, 0x0A, 0x0C, 0x00, 0x30, 0x24}, MmioInstructionKind::load_zero_extend, 7, 0, 0, "sib disp32"},
        {{0x89, 0x0C, 0x10}, MmioInstructionKind::store_register, 3, 1, 0, "mov [rax+rdx], ecx"},
        {{0x45, 0x89, 0x14, 0x01}, MmioInstructionKind::store_register, 4, 10, 0, "mov [r9+rax], r10d"},
        {{0xC7, 0x04, 0x10, 0x78, 0x56, 0x34, 0x12}, MmioInstructionKind::store_immediate, 7, 0, 0x12345678U, "mov dword imm"},
        {{0xC7, 0x84, 0x10, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00},
         MmioInstructionKind::store_immediate, 11, 0, 1U, "mov dword imm disp32"},
        // Everything else keeps single-stepping.
        {{0x48, 0x8B, 0x04, 0x0A}, MmioInstructionKind::unsupported, 0, 0, 0, "64-bit load"},
        {{0x48, 0x89, 0x04, 0x0A}, MmioInstructionKind::unsupported, 0, 0, 0, "64-bit store"},
        {{0x63, 0x04, 0x0A}, MmioInstructionKind::unsupported, 0, 0, 0, "movsxd without REX.W"},
        {{0x8B, 0xC1}, MmioInstructionKind::unsupported, 0, 0, 0, "register operand"},
        {{0xC7, 0x0C, 0x10, 0, 0, 0, 0}, MmioInstructionKind::unsupported, 0, 0, 0, "C7 /1"},
        {{0x66, 0x8B, 0x04, 0x0A}, MmioInstructionKind::unsupported, 0, 0, 0, "16-bit"},
        {{0x0F, 0xB6, 0x04, 0x0A}, MmioInstructionKind::unsupported, 0, 0, 0, "movzx byte"},
        {{0x83, 0x3C, 0x08, 0x00}, MmioInstructionKind::unsupported, 0, 0, 0, "cmp dword"},
    };
    for (const Case &test : cases) {
        const auto decoded = jfg::boot::decode_mmio_instruction(test.code.data());
        if (decoded.kind != test.kind || decoded.length != test.length ||
            decoded.reg != test.reg || decoded.immediate != test.immediate) {
            std::fprintf(stderr, "FAILED: decode %s\n", test.name);
            std::exit(1);
        }
    }
    const jfg::boot::MmioInstruction sign{MmioInstructionKind::load_sign_extend, 4, 0, 0};
    const jfg::boot::MmioInstruction zero{MmioInstructionKind::load_zero_extend, 3, 0, 0};
    require(jfg::boot::mmio_load_result(sign, 0x80000000U) == 0xFFFFFFFF80000000ULL,
            "movsxd sign-extends like a guest lw");
    require(jfg::boot::mmio_load_result(zero, 0x80000000U) == 0x80000000ULL,
            "a 32-bit mov clears the upper half");
    const jfg::boot::MmioInstruction store{MmioInstructionKind::store_register, 3, 1, 0};
    const jfg::boot::MmioInstruction immediate{MmioInstructionKind::store_immediate, 7, 0, 7U};
    require(jfg::boot::mmio_store_word(store, 0xFFFFFFFF12345678ULL) == 0x12345678U,
            "a store writes the low word");
    require(jfg::boot::mmio_store_word(immediate, 0xFFFFFFFFULL) == 7U, "an immediate store");
}

void autopilot() {
    using namespace jfg::rgo;
    FakeMemory memory;
    NewGameAutopilot pilot;
    // Title (mode 3): A held for 6 polls, then released for 15.
    memory.bytes[0x80000000U | kFrontModeAddress] = 3U;
    for (int poll = 0; poll < 6; ++poll)
        require(pilot.sample(memory).buttons == kButtonA, "title press");
    for (int poll = 0; poll < 15; ++poll)
        require(pilot.sample(memory).buttons == 0U, "title release");
    // Name keyboard (mode 24) with the overlay present: overlay 42 is
    // relocated to 0x80200000 in RDRAM, its variables live at 0x02A0xxxx.
    memory.bytes[0x80000000U | kFrontModeAddress] = 24U;
    const std::uint32_t table = 0x80100000U, base = 0x80200000U;
    memory.put32(0x80000000U | kRunlinkTableAddress, table);
    memory.put32(table + kKeyboardOverlay * 32U, base);
    const auto pair = [&](std::uint32_t hi, std::uint32_t lo, std::uint32_t hi_op,
                          std::uint32_t lo_op, std::uint32_t target) {
        memory.put32(base + hi, (hi_op << 16U) | ((target + 0x8000U) >> 16U));
        memory.put32(base + lo, (lo_op << 16U) | (target & 0xFFFFU));
    };
    const std::uint32_t data = base + 0x2CA0U;  // relocated data section
    pair(0U, 4U, 0x3C02U, 0x2442U, data + 0x10U);        // initialized
    pair(0x84U, 0x8CU, 0x3C02U, 0x2442U, data + 0x48U);  // label cursor
    pair(0x88U, 0x90U, 0x3C08U, 0x2508U, data + 0x30U);  // label
    pair(0x9CU, 0xA0U, 0x3C01U, 0xA020U, data + 0x90U);  // submode
    pair(0x206CU, 0x2070U, 0x3C18U, 0x8F18U, data + 0x18U);  // selected key
    const auto linked = [](std::uint32_t offset) { return kKeyboardOverlayVram + 0x2CA0U + offset; };
    require(!read_keyboard(memory).has_value(), "an uninitialized keyboard is not read");
    memory.put32(linked(0x10U), 1U);
    memory.put32(linked(0x48U), linked(0x30U) + 3U);  // three letters typed
    memory.put32(linked(0x18U), 7U);
    memory.bytes[linked(0x90U)] = 1U;
    const auto keyboard = read_keyboard(memory);
    require(keyboard && keyboard->submode == 1U && keyboard->key == 7U &&
                keyboard->label_length == 3U,
            "keyboard state read from the overlay's linked addresses");
    // A name is typed and END (key 40) is not selected: move towards it.
    NewGameAutopilot typing;
    const auto move = typing.sample(memory);
    require(move.buttons == 0U && move.stick_x == -80, "move left from the first half");
    memory.put32(linked(0x18U), 40U);
    NewGameAutopilot confirm;
    require(confirm.sample(memory).buttons == kButtonA, "END selected: confirm with A");
    // Gameplay: walk forward, then turn, without pressing buttons.
    memory.bytes[0x80000000U | kFrontModeAddress] = 16U;
    NewGameAutopilot walker;
    const auto walk = walker.sample(memory);
    require(walk.buttons == 0U && walk.stick_y > 0 && walk.stick_x == 0, "gameplay walks forward");
    AutopilotInput turn{};
    for (int poll = 1; poll < 125; ++poll) turn = walker.sample(memory);
    require(turn.buttons == 0U && turn.stick_x > 0, "and turns now and then");
}

}  // namespace

int main() {
    frame_stats();
    event_ties();
    mmio_moves();
    autopilot();
    std::puts("rgo framerate tests passed");
    return 0;
}
