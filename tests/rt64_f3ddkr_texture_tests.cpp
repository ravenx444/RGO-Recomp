#define SDL_MAIN_HANDLED
#include "rt64_f3ddkr.hpp"

#include "hle/rt64_application.h"
#include "hle/rt64_interpreter.h"

#include <cstdint>
#include <chrono>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
void interrupt() {}
}

int main() {
    // Construct only the CPU command state: no window, GPU, ROM, or user files.
    std::vector<std::uint8_t> rdram(8U * 1024U * 1024U);
    std::vector<std::uint8_t> guest(rdram.size());
    std::uint32_t mi_interrupt = 0U;
    RT64::Application::Core core{};
    core.RDRAM = rdram.data();
    core.MI_INTR_REG = &mi_interrupt;
    core.checkInterrupts = &interrupt;
    RT64::ApplicationConfiguration config{};
    config.detectDataPath = false;
    config.useConfigurationFile = false;
    RT64::Application application(core, config);
    application.state = std::make_unique<RT64::State>(
        rdram.data(), &mi_interrupt, &interrupt);
    application.interpreter = std::make_unique<RT64::Interpreter>();
    application.interpreter->setup(application.state.get());
    constexpr std::uint32_t texture = 0x0002'0000U;
    {
    jfg::Rt64F3ddkr renderer(application);
    renderer.install();
    renderer.begin(guest);
    const auto command = [&](const std::uint32_t w0, const std::uint32_t w1) {
        RT64::DisplayList dl{};
        dl.w0 = w0;
        dl.w1 = w1;
        RT64::DisplayList* cursor = &dl;
        application.interpreter->hleGBI->map[w0 >> 24U](
            application.state.get(), &cursor);
        require(renderer.complete(), "F3DDKR command rejected");
    };
    // Two independent guest offset tables in the renderer's word-swapped
    // snapshot layout, containing shifts 0x100 and 0x200.
    guest[0x1000U ^ 3U] = 1U;
    guest[0x3000U ^ 3U] = 2U;
    command(0x0200'0010U, 0x1000U);
    command(0xFD10'0000U, texture);
    require(application.state->rdp->texture.address == texture + 0x100U,
        "first table must affect RGBA texture addressing");

    // The original microcode's BF handler disables texture offsets, even if
    // no LoadBlock has advanced the cursor. The next model uses its own address.
    command(0xBF00'0040U, 0x80U);
    command(0xFD10'0000U, texture);
    require(application.state->rdp->texture.address == texture,
        "DMA offsets must stop the previous model's texture shift");

    command(0x0200'0010U, 0x3000U);
    command(0xFD10'0000U, texture);
    require(application.state->rdp->texture.address == texture + 0x200U,
        "a new table must re-enable offsets after BF");

    // Also reset a table before its first texture command, then repeat BF.
    command(0x0200'0010U, 0x1000U);
    command(0xBF00'0080U, 0x100U);
    command(0xBF00'0100U, 0x200U);
    command(0xFD10'0000U, texture);
    require(application.state->rdp->texture.address == texture,
        "BF must clear an unused table and remain idempotent");
    command(0x0200'0010U, 0x1000U);
    command(0xFD10'0000U, texture);
    require(application.state->rdp->texture.address == texture + 0x100U,
        "later model must retain explicitly re-enabled texture offsets");
    require(renderer.stats().context_cache_hits > 0U,
        "repeated commands must use the same-context fast path");
    {
        RT64::Application second(core, config);
        second.state = std::make_unique<RT64::State>(rdram.data(), &mi_interrupt, &interrupt);
        second.interpreter = std::make_unique<RT64::Interpreter>();
        second.interpreter->setup(second.state.get());
        jfg::Rt64F3ddkr other(second);
        other.install(); other.begin(guest);
        RT64::DisplayList dl{};dl.w0=0xBF000040U;dl.w1=0x80U;
        auto* cursor=&dl;
        second.interpreter->hleGBI->map[0xBFU](second.state.get(),&cursor);
        command(0xFD100000U,texture);
        require(application.state->rdp->texture.address == texture + 0x100U,
            "switching contexts cannot share texture-offset state");
    }
    command(0xFD100000U,texture);
    require(application.state->rdp->texture.address == texture + 0x100U,
        "destroying another context must invalidate without losing this state");
    }
    // Reuse the same State address after destroying its previous renderer.
    // A cached pointer to the old renderer must not survive registration.
    {
        jfg::Rt64F3ddkr replacement(application);
        replacement.install(); replacement.begin(guest);
        RT64::DisplayList dl{};dl.w0=0xFD100000U;dl.w1=texture;
        auto* cursor=&dl;
        application.interpreter->hleGBI->map[0xFDU](application.state.get(),&cursor);
        require(application.state->rdp->texture.address==texture,
            "new renderer on the same State must start with fresh offsets");
        require(replacement.stats().context_registry_lookups>0U,
            "new context registration must invalidate a previous cached lookup");
    }
    // CPU-only comparison, with exactly the same decoder and commands. Timing
    // is reported, not asserted: background work must not make a test flaky.
    std::array<long long,3> uncached{},cached{};
    for(std::size_t trial=0;trial<3;++trial) {
        for(const bool enabled:{false,true}) {
            jfg::Rt64F3ddkr measured(application,enabled);
            measured.install();measured.begin(guest);
            const auto started=std::chrono::steady_clock::now();
            for(unsigned i=0;i<200000U;++i) {
                RT64::DisplayList dl{};dl.w0=0xFD100000U;dl.w1=texture;
                auto* cursor=&dl;
                application.interpreter->hleGBI->map[0xFDU](application.state.get(),&cursor);
            }
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-started).count();
            (enabled?cached:uncached)[trial]=elapsed;
            require(application.state->rdp->texture.address==texture,
                "cached and uncached decoding must produce the same texture address");
            const auto stats=measured.stats();
            require(enabled?stats.context_cache_hits>=200000U:stats.context_cache_hits==0U,
                "comparison must exercise the selected lookup path");
            require(enabled?stats.context_registry_lookups==1U:stats.context_registry_lookups>=200000U,
                "cache must remove command-level registry locks");
        }
    }
    std::sort(uncached.begin(),uncached.end());std::sort(cached.begin(),cached.end());
    std::printf("context lookup, 200000 commands: uncached %lld us, cached %lld us\n",
        uncached[1],cached[1]);
    return 0;
}
