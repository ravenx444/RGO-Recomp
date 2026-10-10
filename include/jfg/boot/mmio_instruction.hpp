#pragma once

// RGO: one-trap MMIO (docs/rgo/framerate.md, "MMIO without single-step").
//
// Guest device registers live on guarded host pages. A guest access faults;
// the handler presents the register value, single-steps the host instruction
// and applies a write afterwards: two exceptions per access. When the
// faulting instruction is a plain 32-bit move, the handler can instead do the
// move itself and skip the instruction, which saves the single-step
// exception. This decodes exactly those moves; anything else (other
// encodings, other widths, prefixes) is reported as unsupported and keeps
// the single-step path.

#include <cstdint>

namespace jfg::boot {

enum class MmioInstructionKind : std::uint8_t {
    unsupported,
    load_zero_extend,  // mov r32, [mem]       (8B /r)
    load_sign_extend,  // movsxd r64, [mem32]  (REX.W 63 /r)
    store_register,    // mov [mem], r32       (89 /r)
    store_immediate,   // mov dword [mem], imm (C7 /0 id)
};

struct MmioInstruction {
    MmioInstructionKind kind = MmioInstructionKind::unsupported;
    std::uint8_t length = 0U;    // bytes to skip
    std::uint8_t reg = 0U;       // x86-64 register number (rax=0 ... r15=15)
    std::uint32_t immediate = 0U;

    [[nodiscard]] constexpr bool load() const noexcept {
        return kind == MmioInstructionKind::load_zero_extend ||
               kind == MmioInstructionKind::load_sign_extend;
    }
    [[nodiscard]] constexpr bool store() const noexcept {
        return kind == MmioInstructionKind::store_register ||
               kind == MmioInstructionKind::store_immediate;
    }
};

// Longest encoding this accepts: REX, opcode, ModRM, SIB, disp32, imm32.
inline constexpr std::uint32_t kMmioInstructionMaxBytes = 12U;

// `code` points at the faulting instruction; kMmioInstructionMaxBytes bytes
// must be readable.
[[nodiscard]] constexpr MmioInstruction decode_mmio_instruction(
    const std::uint8_t *code) noexcept {
    std::uint32_t at = 0U;
    std::uint8_t rex = 0U;
    if ((code[at] & 0xF0U) == 0x40U)
        rex = code[at++];
    const bool wide = (rex & 0x08U) != 0U;
    const std::uint8_t opcode = code[at++];
    MmioInstructionKind kind = MmioInstructionKind::unsupported;
    if (opcode == 0x8BU && !wide)
        kind = MmioInstructionKind::load_zero_extend;
    else if (opcode == 0x63U && wide)
        kind = MmioInstructionKind::load_sign_extend;
    else if (opcode == 0x89U && !wide)
        kind = MmioInstructionKind::store_register;
    else if (opcode == 0xC7U && !wide)
        kind = MmioInstructionKind::store_immediate;
    else
        return {};
    const std::uint8_t modrm = code[at++];
    const auto mod = static_cast<std::uint8_t>(modrm >> 6U);
    const std::uint8_t reg = static_cast<std::uint8_t>(((modrm >> 3U) & 7U) |
                                                       ((rex & 0x04U) != 0U ? 8U : 0U));
    const auto rm = static_cast<std::uint8_t>(modrm & 7U);
    if (mod == 3U)
        return {};  // register operand: not a memory access
    if (kind == MmioInstructionKind::store_immediate && (modrm & 0x38U) != 0U)
        return {};  // C7 /0 only
    if (rm == 4U) {
        const std::uint8_t sib = code[at++];
        if (mod == 0U && (sib & 7U) == 5U)
            at += 4U;  // no base, disp32
    } else if (mod == 0U && rm == 5U) {
        at += 4U;  // RIP-relative disp32
    }
    if (mod == 1U)
        at += 1U;
    else if (mod == 2U)
        at += 4U;
    std::uint32_t immediate = 0U;
    if (kind == MmioInstructionKind::store_immediate) {
        immediate = static_cast<std::uint32_t>(code[at]) |
                    (static_cast<std::uint32_t>(code[at + 1U]) << 8U) |
                    (static_cast<std::uint32_t>(code[at + 2U]) << 16U) |
                    (static_cast<std::uint32_t>(code[at + 3U]) << 24U);
        at += 4U;
    }
    return {kind, static_cast<std::uint8_t>(at),
            kind == MmioInstructionKind::store_immediate ? std::uint8_t{0U} : reg, immediate};
}

// The register value a load leaves, from the 32-bit word read.
[[nodiscard]] constexpr std::uint64_t mmio_load_result(const MmioInstruction &instruction,
                                                       const std::uint32_t word) noexcept {
    return instruction.kind == MmioInstructionKind::load_sign_extend
               ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word)))
               : static_cast<std::uint64_t>(word);
}

// The 32-bit word a store writes, given the source register's value.
[[nodiscard]] constexpr std::uint32_t mmio_store_word(const MmioInstruction &instruction,
                                                      const std::uint64_t register_value) noexcept {
    return instruction.kind == MmioInstructionKind::store_immediate
               ? instruction.immediate
               : static_cast<std::uint32_t>(register_value);
}

}  // namespace jfg::boot
