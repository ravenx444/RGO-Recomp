#include "recomp.h"
#include "jfg/runtime/cpu_operation_bridge.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

extern "C" std::size_t jfg_generated_section_count(void);
extern "C" int jfg_generated_initialize_sections(
    int32_t* addresses,
    std::size_t capacity
);

namespace {

constexpr std::size_t minimal_section_capacity = 4096;
int32_t minimal_section_addresses[minimal_section_capacity] = {0};
gpr minimal_cop0_status = 0;
bool minimal_sections_initialized = false;

// Generated bodies call the C ABI get_function() directly.  This hook is the
// sole route from that ABI into the project-owned overlay runtime; it must not
// fall back to the generated table, because doing so bypasses module lifetime
// and dependency checks.
using dispatch_call_callback = int (*)(
    void*, int32_t, uint8_t*, recomp_context*);

struct DispatchBinding {
    dispatch_call_callback callback = nullptr;
    void* opaque = nullptr;
    std::size_t active_lookups = 0U;
    std::uint64_t generation = 0U;
};

DispatchBinding dispatch_binding{};
struct CpuBinding {
    JfgCpuOperationCallback callback = nullptr;
    void* opaque = nullptr;
    std::size_t active_calls = 0U;
};
CpuBinding cpu_binding{};
std::uint64_t next_dispatch_generation = 1U;

struct PendingDispatch {
    dispatch_call_callback callback = nullptr;
    void* opaque = nullptr;
    int32_t guest_address = 0;
    std::uint64_t generation = 0U;
    bool active = false;
};

thread_local PendingDispatch pending_dispatch{};

#if defined(_MSC_VER)
volatile long dispatch_lock_word = 0;
#elif defined(__clang__) || defined(__GNUC__)
volatile unsigned char dispatch_lock_word = 0U;
#else
#error "Minimal generated dispatch requires supported atomic intrinsics"
#endif

void acquire_dispatch_lock() noexcept {
#if defined(_MSC_VER)
    while (_InterlockedExchange(&dispatch_lock_word, 1L) != 0L) {
    }
#else
    while (__atomic_test_and_set(&dispatch_lock_word, __ATOMIC_ACQUIRE)) {
    }
#endif
}

void release_dispatch_lock() noexcept {
#if defined(_MSC_VER)
    (void)_InterlockedExchange(&dispatch_lock_word, 0L);
#else
    __atomic_clear(&dispatch_lock_word, __ATOMIC_RELEASE);
#endif
}

class DispatchLock final {
public:
    DispatchLock() noexcept {
        acquire_dispatch_lock();
    }

    ~DispatchLock() noexcept {
        release_dispatch_lock();
    }

    DispatchLock(const DispatchLock&) = delete;
    DispatchLock& operator=(const DispatchLock&) = delete;
};

class CpuCall final {
public:
    CpuCall() {
        const DispatchLock lock;
        callback = cpu_binding.callback;
        opaque = cpu_binding.opaque;
        if (callback != nullptr) ++cpu_binding.active_calls;
    }
    ~CpuCall() {
        if (callback != nullptr) {
            const DispatchLock lock;
            --cpu_binding.active_calls;
        }
    }
    CpuCall(const CpuCall&) = delete;
    CpuCall& operator=(const CpuCall&) = delete;
    JfgCpuOperationCallback callback = nullptr;
    void* opaque = nullptr;
};

bool cpu_operation(recomp_context* context, std::uint32_t operation,
                   std::uint32_t selector, std::uint64_t& value) {
    const CpuCall call;
    return call.callback != nullptr &&
        call.callback(call.opaque, context, operation, selector, &value) == 1;
}

[[nodiscard]] int initialize_locked() {
    if (minimal_sections_initialized) {
        return 1;
    }
    const std::size_t required = jfg_generated_section_count();
    if (required == 0 || required > minimal_section_capacity) {
        return 0;
    }
    for (std::size_t index = 0; index < minimal_section_capacity; ++index) {
        minimal_section_addresses[index] = 0;
    }
    if (jfg_generated_initialize_sections(
            minimal_section_addresses,
            minimal_section_capacity) == 0) {
        return 0;
    }
    // Once generated section storage is published it is immutable for the
    // process lifetime. This makes concurrent get_function/initialize calls
    // idempotent and prevents a later caller from rewriting live addresses.
    minimal_sections_initialized = true;
    return 1;
}

enum class TrapSite : std::uint8_t {
    one_shot_dispatch,
    cop0_write,
    cop0_read,
    cop0_eret,
    cache_op,
    cop0_tlb_op,
    reserved_instruction,
    switch_error,
    do_break,
    get_function,
    recomp_syscall_handler,
    pause_self,
    count,
};

// Every unsupported CPU-level operation terminates the process. Name the
// bridge on stderr first so a trap in a headless replay can be attributed
// without a debugger. The Phase 4 object-shape audit pins this object to a
// single external data symbol, so the text lives in function-local static
// arrays rather than string literals, which MSVC and clang-cl emit as
// external COMDAT data.
[[noreturn]] void fail_closed(const TrapSite site) {
    static constexpr char kPrefix[] =
        "jfg minimal runtime: unsupported operation in ";
    static constexpr char kSuffix[] = "; failing closed\n";
    static constexpr char kNames[][24] = {
        "one_shot_dispatch",
        "cop0_write",
        "cop0_read",
        "cop0_eret",
        "cache_op",
        "cop0_tlb_op",
        "reserved_instruction",
        "switch_error",
        "do_break",
        "get_function",
        "recomp_syscall_handler",
        "pause_self",
    };
    std::fputs(kPrefix, stderr);
    const auto index = static_cast<std::size_t>(site);
    if (index < static_cast<std::size_t>(TrapSite::count)) {
        std::fputs(kNames[index], stderr);
    }
    std::fputs(kSuffix, stderr);
    std::fflush(stderr);
    std::abort();
}

template <typename Context>
concept FloatModeContext = requires(Context& ctx) {
    ctx.status_reg;
    ctx.f_odd;
    ctx.f0.u32h;
    ctx.f1.u32l;
    ctx.mips3_float_mode;
};

template <typename Context>
void write_status(Context* ctx, gpr value) {
    if constexpr (FloatModeContext<Context>) {
        constexpr std::uint32_t fr_mask = 0x04000000U;
        const auto status = static_cast<std::uint32_t>(value);
        ctx->status_reg = status;
        if ((status & fr_mask) != 0U) {
            ctx->f_odd = &ctx->f1.u32l;
            ctx->mips3_float_mode = 1U;
        }
        else {
            ctx->f_odd = &ctx->f0.u32h;
            ctx->mips3_float_mode = 0U;
        }
    }
    else {
        (void)ctx;
    }
}

template <typename Context>
[[nodiscard]] gpr read_status(Context* ctx) {
    if constexpr (FloatModeContext<Context>) {
        return static_cast<gpr>(static_cast<std::int32_t>(ctx->status_reg));
    }
    else {
        (void)ctx;
        const DispatchLock lock;
        return minimal_cop0_status;
    }
}

void one_shot_dispatch(uint8_t* rdram, recomp_context* ctx) {
    dispatch_call_callback callback = nullptr;
    void* opaque = nullptr;
    int32_t guest_address = 0;
    std::uint64_t generation = 0U;
    {
        const DispatchLock lock;
        if (!pending_dispatch.active || pending_dispatch.callback == nullptr) {
            fail_closed(TrapSite::one_shot_dispatch);
        }
        callback = pending_dispatch.callback;
        opaque = pending_dispatch.opaque;
        guest_address = pending_dispatch.guest_address;
        generation = pending_dispatch.generation;
        // Consume before dispatch so a nested generated get_function call can
        // install its own one-shot target, while a second unresolved lookup
        // or a stored/replayed trampoline fails closed.
        pending_dispatch = {};
        if (dispatch_binding.callback != callback ||
            dispatch_binding.opaque != opaque ||
            dispatch_binding.generation != generation) {
            fail_closed(TrapSite::one_shot_dispatch);
        }
        ++dispatch_binding.active_lookups;
    }

    int accepted = 0;
    try {
        accepted = callback(opaque, guest_address, rdram, ctx);
    }
    catch (...) {
        const DispatchLock lock;
        if (dispatch_binding.callback != callback ||
            dispatch_binding.opaque != opaque ||
            dispatch_binding.generation != generation ||
            dispatch_binding.active_lookups == 0U) {
            fail_closed(TrapSite::one_shot_dispatch);
        }
        --dispatch_binding.active_lookups;
        fail_closed(TrapSite::one_shot_dispatch);
    }
    {
        const DispatchLock lock;
        if (dispatch_binding.callback != callback ||
            dispatch_binding.opaque != opaque ||
            dispatch_binding.generation != generation ||
            dispatch_binding.active_lookups == 0U) {
            fail_closed(TrapSite::one_shot_dispatch);
        }
        --dispatch_binding.active_lookups;
    }
    if (accepted == 0) {
        fail_closed(TrapSite::one_shot_dispatch);
    }
}

} // namespace

extern "C" {

int32_t* section_addresses = minimal_section_addresses;

std::size_t jfg_minimal_runtime_section_capacity(void) {
    return minimal_section_capacity;
}

int jfg_minimal_runtime_initialize(void) {
    const DispatchLock lock;
    return initialize_locked();
}

int jfg_minimal_runtime_bind_dispatch(
    int (*callback)(void*, int32_t, uint8_t*, recomp_context*),
    void* opaque
) {
    if (callback == nullptr) {
        return 0;
    }

    const DispatchLock lock;
    // A second owner, even one presenting the same values, is refused.  The
    // first owner must explicitly release its lifetime before another can
    // install a gate.
    if (dispatch_binding.callback != nullptr || dispatch_binding.active_lookups != 0U) {
        return 0;
    }
    if (next_dispatch_generation == ~std::uint64_t{0U}) {
        return 0;
    }
    dispatch_binding.callback = callback;
    dispatch_binding.opaque = opaque;
    dispatch_binding.generation = next_dispatch_generation++;
    return 1;
}

int jfg_minimal_runtime_unbind_dispatch(
    int (*callback)(void*, int32_t, uint8_t*, recomp_context*),
    void* opaque
) {
    if (callback == nullptr) {
        return 0;
    }

    const DispatchLock lock;
    // Refusing an in-flight teardown keeps the opaque context valid until its
    // callback returns.  This intentionally also refuses a callback trying to
    // destroy its own binding.
    if (dispatch_binding.callback != callback ||
        dispatch_binding.opaque != opaque ||
        dispatch_binding.active_lookups != 0U) {
        return 0;
    }
    dispatch_binding.callback = nullptr;
    dispatch_binding.opaque = nullptr;
    dispatch_binding.generation = 0U;
    return 1;
}

void cop0_status_write(recomp_context* ctx, gpr value) {
    write_status(ctx, value);
    const DispatchLock lock;
    minimal_cop0_status = value;
}

int jfg_minimal_runtime_bind_cpu(JfgCpuOperationCallback callback, void* opaque) {
    const DispatchLock lock;
    if (callback == nullptr || cpu_binding.callback != nullptr || cpu_binding.active_calls != 0U)
        return 0;
    cpu_binding.callback = callback;
    cpu_binding.opaque = opaque;
    return 1;
}

int jfg_minimal_runtime_unbind_cpu(JfgCpuOperationCallback callback, void* opaque) {
    const DispatchLock lock;
    if (callback == nullptr || cpu_binding.callback != callback ||
        cpu_binding.opaque != opaque || cpu_binding.active_calls != 0U) return 0;
    cpu_binding.callback = nullptr;
    cpu_binding.opaque = nullptr;
    return 1;
}

gpr cop0_status_read(recomp_context* ctx) {
    return read_status(ctx);
}

void cop0_write(recomp_context* ctx, uint32_t cop0_reg, gpr value) {
    if (cop0_reg == 12U) {
        cop0_status_write(ctx, value);
        return;
    }
    std::uint64_t bridged = static_cast<std::uint64_t>(value);
    if (cpu_operation(ctx, JFG_CPU_WRITE_REGISTER, cop0_reg, bridged)) return;
    fail_closed(TrapSite::cop0_write);
}

gpr cop0_read(recomp_context* ctx, uint32_t cop0_reg) {
    if (cop0_reg == 12U) {
        return cop0_status_read(ctx);
    }
    std::uint64_t value = 0;
    if (cpu_operation(ctx, JFG_CPU_READ_REGISTER, cop0_reg, value))
        return static_cast<gpr>(value);
    fail_closed(TrapSite::cop0_read);
}

void cop0_eret(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    std::uint64_t value = 0;
    if (cpu_operation(ctx, JFG_CPU_EXCEPTION_RETURN, 0, value)) return;
    fail_closed(TrapSite::cop0_eret);
}

void cache_op(uint8_t* rdram, recomp_context* ctx, uint32_t operation, gpr address) {
    (void)rdram;
    std::uint64_t value = static_cast<std::uint64_t>(address);
    if (cpu_operation(ctx, JFG_CPU_CACHE_OPERATION, operation, value)) return;
    fail_closed(TrapSite::cache_op);
}

void cop0_tlb_op(recomp_context* ctx, uint32_t operation) {
    std::uint64_t value = 0;
    if (cpu_operation(ctx, JFG_CPU_TLB_OPERATION, operation, value)) return;
    fail_closed(TrapSite::cop0_tlb_op);
}

#if !defined(JFG_G2_TRAP_PROBE_BRIDGES)
void reserved_instruction(uint8_t* rdram, recomp_context* ctx, uint32_t vram, uint32_t word) {
    (void)rdram;
    (void)ctx;
    (void)vram;
    (void)word;
    fail_closed(TrapSite::reserved_instruction);
}

void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    (void)func;
    (void)vram;
    (void)jtbl;
    fail_closed(TrapSite::switch_error);
}

void do_break(uint32_t vram) {
    // Keep the bridge's exported symbol set independent of UCRT printf inlines.
    char message[] = "guest break at 0x00000000\n";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8U; ++i)
        message[17U + i] = digits[(vram >> ((7U - i) * 4U)) & 0xfU];
    std::fputs(message, stderr);
    fail_closed(TrapSite::do_break);
}
#endif

recomp_func_t* get_function(int32_t vram) {
    {
        const DispatchLock lock;
        // Calling the public initializer here would recursively acquire the
        // same spin lock. Keep initialization and dispatch publication in one
        // critical section instead.
        if (initialize_locked() == 0) {
            fail_closed(TrapSite::get_function);
        }
        if (dispatch_binding.callback == nullptr || pending_dispatch.active) {
            fail_closed(TrapSite::get_function);
        }
        pending_dispatch.callback = dispatch_binding.callback;
        pending_dispatch.opaque = dispatch_binding.opaque;
        pending_dispatch.guest_address = vram;
        pending_dispatch.generation = dispatch_binding.generation;
        pending_dispatch.active = true;
    }
    return one_shot_dispatch;
}

#if !defined(JFG_G2_TRAP_PROBE_BRIDGES)
void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    (void)rdram;
    (void)ctx;
    (void)instruction_vram;
    fail_closed(TrapSite::recomp_syscall_handler);
}
#endif

void pause_self(uint8_t* rdram) {
    dispatch_call_callback callback = nullptr;
    void* opaque = nullptr;
    {
        const DispatchLock lock;
        if (dispatch_binding.callback == nullptr) {
            fail_closed(TrapSite::pause_self);
        }
        callback = dispatch_binding.callback;
        opaque = dispatch_binding.opaque;
    }
    recomp_context context{};
    constexpr int32_t pause_dispatch = -2;
    if (callback(opaque, pause_dispatch, rdram, &context) == 0) {
        fail_closed(TrapSite::pause_self);
    }
    // A modeled branch-to-self never returns during normal execution.
    fail_closed(TrapSite::pause_self);
}

} // extern "C"
