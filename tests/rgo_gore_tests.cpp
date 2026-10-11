// RGO persistent gore (docs/rgo/gore.md): the gore.ini format and the
// bookkeeping that decides which limbs, heads and blood splats are kept.
#include "jfg/rgo/persistent_gore.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void require(const bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

using jfg::rgo::GoreKind;
using jfg::rgo::GoreSettings;

void settings_round_trip() {
    const GoreSettings defaults{};
    require(defaults.heads && defaults.limbs && defaults.blood, "everything stays by default");
    const auto parsed = jfg::rgo::parse_gore_settings(jfg::rgo::encode_gore_settings(defaults));
    require(parsed && *parsed == defaults, "defaults round trip");

    GoreSettings custom{};
    custom.heads = false;
    custom.blood = false;
    custom.head_limit = 10U;
    custom.limb_limit = 0U;
    custom.blood_limit = 48U;
    const auto again = jfg::rgo::parse_gore_settings(jfg::rgo::encode_gore_settings(custom));
    require(again && *again == custom, "custom settings round trip");

    const auto crlf = jfg::rgo::parse_gore_settings("version=1\r\nheads=off\r\n");
    require(crlf && !crlf->heads && crlf->limbs && crlf->blood, "CRLF and missing keys keep defaults");
    require(jfg::rgo::parse_gore_settings("version=1\nfuture=1\n").has_value(),
            "unknown keys are ignored");
}

void settings_rejects_bad_files() {
    require(!jfg::rgo::parse_gore_settings(""), "empty file");
    require(!jfg::rgo::parse_gore_settings("version=2\n"), "unknown version");
    require(!jfg::rgo::parse_gore_settings("version=1\nheads=on"), "unterminated last line");
    require(!jfg::rgo::parse_gore_settings("version=1\nheads=maybe\n"), "bad on/off");
    require(!jfg::rgo::parse_gore_settings("version=1\nheads=on\nheads=off\n"), "duplicate key");
    require(!jfg::rgo::parse_gore_settings("version=1\nblood_limit=49\n"), "blood limit leaves room for new blood");
    require(!jfg::rgo::parse_gore_settings("version=1\nhead_limit=129\n"), "head limit maximum");
    require(!jfg::rgo::parse_gore_settings("version=1\nlimb_limit=-1\n"), "negative limit");
    require(!jfg::rgo::parse_gore_settings("version=1\nlimb_limit=\n"), "empty limit");
    require(!jfg::rgo::parse_gore_settings("version=1\nlimb_limit=4x\n"), "trailing characters");
}

void limb_kinds() {
    require(jfg::rgo::limb_kind(0U) == GoreKind::limb, "kind 0 is gore");
    require(jfg::rgo::limb_kind(1U) == GoreKind::limb, "kind 1 is gore");
    require(jfg::rgo::limb_kind(2U) == GoreKind::head, "kind 2 is a drone head");
    require(jfg::rgo::limb_kind(4U) == GoreKind::head, "pickups are kept like heads");
}

void objects_keep_newest_within_limit() {
    jfg::rgo::PersistentObjects objects;
    for (std::uint32_t index = 0U; index < 3U; ++index)
        require(objects.hold(0x80200000U + index * 0x100U, GoreKind::head, 3U), "within the limit");
    require(objects.held(GoreKind::head) == 3U, "three heads held");
    // A fourth head lets the oldest go; the oldest stays let go.
    require(objects.hold(0x80200300U, GoreKind::head, 3U), "newest head is held");
    require(!objects.hold(0x80200000U, GoreKind::head, 3U), "oldest head let go");
    require(objects.hold(0x80200100U, GoreKind::head, 3U), "second head still held");
    require(objects.held(GoreKind::head) == 3U, "still three heads held");
    // Limbs have their own limit.
    require(objects.hold(0x80300000U, GoreKind::limb, 1U), "limb held");
    require(objects.held(GoreKind::head) == 3U, "limbs do not push heads out");
    // Picking up a held head frees room, but a let-go head is not taken back.
    objects.forget(0x80200100U);
    require(objects.held(GoreKind::head) == 2U, "picked up head is forgotten");
    require(!objects.hold(0x80200000U, GoreKind::head, 3U), "let-go head stays let go");
    objects.forget(0x80200000U);
    require(objects.tracked() == 3U, "two heads and a limb tracked");
}

void objects_follow_limit_changes() {
    jfg::rgo::PersistentObjects objects;
    for (std::uint32_t index = 0U; index < 4U; ++index)
        (void)objects.hold(0x80200000U + index * 0x100U, GoreKind::limb, 8U);
    // Lowering the limit lets the oldest go.
    require(objects.hold(0x80200300U, GoreKind::limb, 2U), "newest still held");
    require(objects.held(GoreKind::limb) == 2U, "lowered limit applies");
    require(!objects.hold(0x80200000U, GoreKind::limb, 2U), "oldest let go");
    require(!objects.hold(0x80200100U, GoreKind::limb, 2U), "second oldest let go");
    // A limit of zero holds nothing, not even a new limb.
    require(!objects.hold(0x80209000U, GoreKind::limb, 0U), "limit zero holds nothing");
    require(objects.held(GoreKind::limb) == 0U, "nothing held at limit zero");
}

void objects_reused_address_and_memory_pressure() {
    jfg::rgo::PersistentObjects objects;
    (void)objects.hold(0x80200000U, GoreKind::limb, 8U);
    (void)objects.hold(0x80200100U, GoreKind::head, 8U);
    (void)objects.hold(0x80200200U, GoreKind::limb, 8U);
    // Low memory: plain gore goes first, oldest first, then heads.
    require(objects.release_oldest(), "first release");
    require(!objects.hold(0x80200000U, GoreKind::limb, 8U), "oldest limb let go first");
    require(objects.release_oldest(), "second release");
    require(!objects.hold(0x80200200U, GoreKind::limb, 8U), "then the next limb");
    require(objects.held(GoreKind::head) == 1U, "head kept while limbs remain");
    require(objects.release_oldest(), "third release");
    require(objects.held(GoreKind::head) == 0U, "then the head");
    require(!objects.release_oldest(), "nothing left to release");
    // An address reused by another kind of limb starts over.
    require(objects.hold(0x80200000U, GoreKind::head, 8U), "reused address is new");
    objects.clear();
    require(objects.tracked() == 0U && objects.held(GoreKind::head) == 0U, "clear");
}

void splats_keep_newest_within_limit() {
    jfg::rgo::PersistentSplats splats;
    std::array<bool, jfg::rgo::kBloodSlots> resting{}, hold{};
    resting[5] = true;
    splats.update(resting, 2U, hold);
    require(hold[5], "first splat held");
    resting[9] = true;
    splats.update(resting, 2U, hold);
    resting[1] = true;  // Third splat comes to rest in a lower slot.
    splats.update(resting, 2U, hold);
    require(!hold[5] && hold[9] && hold[1], "oldest splat let go, by age not slot");
    require(!hold[0] && !hold[63], "empty slots are not held");
    // The let-go splat stays let go until its slot is reused.
    splats.update(resting, 2U, hold);
    require(!hold[5], "let-go splat stays let go");
    resting[5] = false;
    splats.update(resting, 2U, hold);
    resting[5] = true;
    resting[9] = false;
    splats.update(resting, 2U, hold);
    require(hold[5] && hold[1], "reused slot is a new splat");
    // Limit zero holds nothing.
    splats.update(resting, 0U, hold);
    require(!hold[5] && !hold[1], "limit zero");
}

void guest_memory_layout() {
    // Big-endian guest words stored as native words: guest byte A sits at
    // host offset (A - 0x80000000) ^ 3.
    std::array<std::uint8_t, 16> host{};
    jfg::rgo::GoreMemory memory(host.data(), host.size());
    host[0] = 0x44U; host[1] = 0x33U; host[2] = 0x22U; host[3] = 0x11U;
    require(memory.u32(0x80000000U) == 0x11223344U, "word read");
    require(memory.u8(0x80000000U) == 0x11U && memory.u8(0x80000003U) == 0x44U, "byte read");
    require(memory.s16(0x80000002U) == 0x3344, "halfword read");
    require(memory.put_s16(0x80000004U, -2) && memory.s16(0x80000004U) == -2, "halfword write");
    require(host[7] == 0xFFU && host[6] == 0xFEU, "halfword stored big-endian in native words");
    require(!memory.u32(0x80000010U) && !memory.u8(0x80000010U), "past the span");
    require(!memory.u32(0x8000000EU) && !memory.s16(0x80000001U), "misaligned");
    require(!memory.u8(0x7FFFFFFFU), "below the base");
    require(memory.valid(0x8000000CU, 4U) && !memory.valid(0x8000000CU, 5U), "range check");
    // Normalized overlay addresses wrap to offsets above RDRAM.
    jfg::rgo::GoreMemory wide(host.data(), 0x8A000000U);
    require(wide.valid(0x01000000U, 4U) && !wide.valid(0x0A000000U, 1U), "overlay window range");
}

} // namespace

int main() {
    settings_round_trip();
    settings_rejects_bad_files();
    limb_kinds();
    objects_keep_newest_within_limit();
    objects_follow_limit_changes();
    objects_reused_address_and_memory_pressure();
    splats_keep_newest_within_limit();
    guest_memory_layout();
    std::puts("rgo gore tests passed");
    return 0;
}
