#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "jfg/runtime/custom_overlay_relocator.hpp"

struct recomp_context;

namespace jfg {

inline constexpr std::uint32_t kGeneratedCachedGuestBase = 0x80000000U;
inline constexpr std::size_t kMaximumGeneratedSectionCount = 65'500U;
inline constexpr std::size_t kMaximumGeneratedRelocationsPerSection =
    1U << 20U;
inline constexpr std::size_t kMaximumCustomOverlayRelocationsPerSection =
    1U << 20U;
inline constexpr std::uint32_t kNoGeneratedStaticSection = 0xFFFFFFFFU;

// A relocation target is an address, rather than an end-exclusive range.
// Consequently the one-past-32-bit endpoint must fail instead of wrapping to
// zero. This is shared by generated R32 and custom static/overlay bindings.
[[nodiscard]] constexpr bool try_add_generated_guest_address(
    const std::uint32_t base,
    const std::uint32_t offset,
    std::uint32_t& address) noexcept {
    const std::uint64_t wide_address =
        static_cast<std::uint64_t>(base) + offset;
    if (wide_address >= (std::uint64_t{1U} << 32U)) {
        return false;
    }
    address = static_cast<std::uint32_t>(wide_address);
    return true;
}

using GeneratedOverlayFunction =
    void (*)(std::uint8_t* rdram, ::recomp_context* context);

enum class GeneratedOverlayError;
struct GeneratedOverlayDispatchGate;
struct GeneratedOverlayExecutionAdmission;

// A generation- and dependency-epoch-bound callable. The generated function
// pointer never leaves this gate. invoke() acquires an execution lease before
// exposing it to the current call, so unload can revoke new calls and quiesce
// calls already in flight before changing code bytes or generated lifecycle
// state.
class GeneratedOverlayFunctionHandle final {
public:
    GeneratedOverlayFunctionHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] GeneratedOverlayError invoke(
        std::uint8_t* rdram,
        ::recomp_context* context) const noexcept;

    friend bool operator==(
        const GeneratedOverlayFunctionHandle&,
        const GeneratedOverlayFunctionHandle&) = default;

private:
    explicit GeneratedOverlayFunctionHandle(
        std::shared_ptr<GeneratedOverlayDispatchGate> gate) noexcept
        : gate_(std::move(gate)) {}

    std::shared_ptr<GeneratedOverlayDispatchGate> gate_;

    friend class GeneratedOverlayRuntime;
};

// Public-safe projection of one generated section. A private adapter may read
// additional generated metadata, but ROM positions, linked addresses, names,
// and generated bodies never cross this boundary.
struct GeneratedSectionExtents {
    std::uint32_t text_size = 0U;
    std::uint32_t data_size = 0U;
    std::uint32_t bss_size = 0U;
    bool is_overlay = false;
};

enum class GeneratedSectionLifecycle {
    load,
    unload,
};

struct GeneratedRelocationResult {
    bool applied = false;
    std::size_t applied_count = 0U;
};

// One generated R_MIPS_32 relocation.  target_section/target_offset identify
// a section-relative data or code address; they deliberately are not a
// function dependency and target_offset is therefore permitted in data/BSS.
struct GeneratedR32RelocationDescriptor {
    std::uint32_t site_offset = 0U;
    std::uint32_t target_section = 0U;
    std::uint32_t target_offset = 0U;
};

// Owned adapter surface for the generated tables. The production adapter is
// expected to bind the locally generated section-count/initializer, metadata,
// lifecycle, checked-relocation, and lookup functions. initialize_sections()
// also binds the supplied stable address storage for this object's lifetime.
class GeneratedOverlayTable {
public:
    virtual ~GeneratedOverlayTable() = default;

    [[nodiscard]] virtual std::size_t section_count() const = 0;
    [[nodiscard]] virtual bool initialize_sections(
        std::span<std::int32_t> addresses) = 0;
    [[nodiscard]] virtual bool section_extents(
        std::uint32_t section,
        GeneratedSectionExtents& output) const = 0;
    [[nodiscard]] virtual bool update_section_lifecycle(
        GeneratedSectionLifecycle operation,
        std::uint32_t section,
        std::int32_t base) = 0;
    [[nodiscard]] virtual std::size_t relocation_count(
        std::uint32_t source_section) const = 0;
    // Required for every overlay image. Returning false refuses the image
    // rather than guessing generated R32 sites.
    [[nodiscard]] virtual bool relocation_sites(
        std::uint32_t source_section,
        std::span<const std::uint32_t>& output) const {
        (void)source_section;
        output = {};
        return false;
    }
    // The descriptor inventory must have exact one-for-one, ordered closure
    // with relocation_sites(). Returning false refuses the image: guessing
    // the target section from relocated bytes is not safe.
    [[nodiscard]] virtual bool relocation_descriptors(
        std::uint32_t source_section,
        std::span<const GeneratedR32RelocationDescriptor>& output) const {
        (void)source_section;
        output = {};
        return false;
    }
    [[nodiscard]] virtual GeneratedRelocationResult apply_relocations_checked(
        std::span<std::uint8_t> staged_rdram,
        std::uint32_t source_section,
        std::size_t expected_count) = 0;
    [[nodiscard]] virtual GeneratedOverlayFunction lookup_function(
        std::int32_t guest_address) const = 0;
};

struct GeneratedInstructionCacheRange {
    std::uint64_t opaque_module_id = 0U;
    std::uint32_t guest_base = 0U;
    std::size_t byte_count = 0U;
};

// prepare() may fail or throw but must not invalidate externally visible
// state. commit() is the no-fail invalidation point after relocated bytes have
// reached live guest memory and before the module becomes observable.
class GeneratedInstructionCacheInvalidator {
public:
    virtual ~GeneratedInstructionCacheInvalidator() = default;

    [[nodiscard]] virtual bool prepare(
        const GeneratedInstructionCacheRange& range) = 0;
    virtual void commit(
        const GeneratedInstructionCacheRange& range) noexcept = 0;
    // Called in reverse attempt order, including the rejecting/throwing range,
    // if a prepare or generated lifecycle callback rejects the transaction.
    // Implementations which do not retain prepared state may keep the default
    // no-op.
    virtual void abort(
        const GeneratedInstructionCacheRange& range) noexcept {
        (void)range;
    }
};

struct GeneratedOverlayDependency {
    // Both IDs are project-owned opaque identities. function_offset is
    // section-relative and contains no generated symbol or linked coordinate.
    std::uint64_t opaque_reference_id = 0U;
    std::uint64_t target_module_id = 0U;
    std::uint32_t function_offset = 0U;
};

// A project-owned opaque binding used by a custom relocation record.  No
// guest address is accepted from an adapter. Both external source forms may
// name either an active overlay module or a declared static generated section;
// source type 3 changes the patch-site domain, not the target domain.
struct GeneratedOverlayCustomBinding {
    std::uint32_t opaque_target_id = 0U;
    std::uint64_t target_module_id = 0U;
    std::uint32_t static_section_index = kNoGeneratedStaticSection;
    std::uint32_t target_offset = 0U;
};

struct GeneratedOverlayImage {
    std::uint64_t opaque_module_id = 0U;
    std::uint32_t section_index = 0U;
    // Exact private text+data bytes are supplied only at runtime. The runtime
    // owns this copy for deterministic same-input reload; it never logs it.
    std::vector<std::byte> initialized_bytes;
    std::vector<GeneratedOverlayDependency> dependencies;
    std::vector<CustomOverlayRelocationRecord> custom_relocations;
    std::vector<GeneratedOverlayCustomBinding> custom_bindings;
};

class GeneratedOverlayToken final {
public:
    GeneratedOverlayToken() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return runtime_identity_ != 0U && opaque_module_id_ != 0U &&
            generation_ != 0U;
    }

    friend bool operator==(
        const GeneratedOverlayToken&,
        const GeneratedOverlayToken&) = default;

private:
    GeneratedOverlayToken(
        std::uint64_t runtime_identity,
        std::uint64_t opaque_module_id,
        std::uint64_t generation) noexcept
        : runtime_identity_(runtime_identity),
          opaque_module_id_(opaque_module_id),
          generation_(generation) {}

    std::uint64_t runtime_identity_ = 0U;
    std::uint64_t opaque_module_id_ = 0U;
    std::uint64_t generation_ = 0U;

    friend class GeneratedOverlayRuntime;
};

enum class GeneratedOverlayError {
    none,
    identity_exhausted,
    invalid_table,
    table_already_installed,
    table_not_installed,
    invalid_section_count,
    section_initialization_failed,
    invalid_section_metadata,
    initial_overlay_published,
    invalid_image,
    invalid_mapping,
    overlapping_mapping,
    duplicate_module,
    reload_required,
    module_not_found,
    module_active,
    module_inactive,
    invalid_dependency,
    invalid_custom_relocation,
    invalid_custom_binding,
    custom_binding_unresolved,
    custom_relocation_failed,
    custom_relocation_overlap,
    relocation_site_mismatch,
    relocation_descriptor_mismatch,
    dependency_unresolved,
    dependency_invalidated,
    invalid_relocation_count,
    section_load_failed,
    section_unload_failed,
    relocation_failed,
    relocation_count_mismatch,
    relocation_wrote_outside_section,
    bss_not_zero,
    cache_invalidation_rejected,
    function_not_found,
    stale_token,
    cross_runtime_token,
    busy,
    runtime_poisoned,
    callback_exception,
    internal_failure,
};

struct GeneratedOverlayInstallResult {
    GeneratedOverlayError error = GeneratedOverlayError::none;

    [[nodiscard]] bool ok() const noexcept {
        return error == GeneratedOverlayError::none;
    }
};

struct GeneratedOverlayLoadResult {
    GeneratedOverlayError error = GeneratedOverlayError::none;
    GeneratedOverlayToken token;

    [[nodiscard]] bool ok() const noexcept {
        return error == GeneratedOverlayError::none && token.valid();
    }
};

struct GeneratedOverlayFunctionResult {
    GeneratedOverlayError error = GeneratedOverlayError::none;
    GeneratedOverlayFunctionHandle function;

    [[nodiscard]] bool ok() const noexcept {
        return error == GeneratedOverlayError::none && function.valid();
    }
};

// RGO: the running overlay invocations are tracked per host thread
// (thread_local). With guest threads as fibers on one host thread, a guest
// thread that yields inside an overlay call would leak that state to the next
// guest thread; the fiber executor swaps it per guest thread through this
// (jfg::boot::register_fiber_local). Installs `context`, returns the previous.
void* swap_overlay_invocation_context(void* context) noexcept;

// Synchronous project-owned overlay registry and generated-table adapter. All
// potentially failing work targets a private staging copy. Live guest memory,
// cache visibility, dependency bindings, and active publication are committed
// only after every validation/callback succeeds. Recursive callback entry sees
// busy and other threads serialize on the recursive mutex.
class GeneratedOverlayRuntime final {
public:
    explicit GeneratedOverlayRuntime(
        std::span<std::byte> guest_memory);
    ~GeneratedOverlayRuntime() noexcept;

    GeneratedOverlayRuntime(const GeneratedOverlayRuntime&) = delete;
    GeneratedOverlayRuntime& operator=(const GeneratedOverlayRuntime&) = delete;
    GeneratedOverlayRuntime(GeneratedOverlayRuntime&&) = delete;
    GeneratedOverlayRuntime& operator=(GeneratedOverlayRuntime&&) = delete;

    [[nodiscard]] GeneratedOverlayInstallResult install_table(
        std::unique_ptr<GeneratedOverlayTable> table) noexcept;
    [[nodiscard]] GeneratedOverlayLoadResult load(
        GeneratedOverlayImage image,
        std::uint32_t guest_base,
        GeneratedInstructionCacheInvalidator& cache) noexcept;
    [[nodiscard]] GeneratedOverlayLoadResult reload_same_base(
        GeneratedOverlayToken prior_lifetime,
        GeneratedInstructionCacheInvalidator& cache) noexcept;
    // Compatibility overload: unloading executable bytes without a cache
    // transaction cannot meet the runtime contract and therefore fails
    // closed. Callers must use the cache-backed overload.
    [[nodiscard]] GeneratedOverlayError unload(
        GeneratedOverlayToken lifetime) noexcept;
    [[nodiscard]] GeneratedOverlayError unload(
        GeneratedOverlayToken lifetime,
        GeneratedInstructionCacheInvalidator& cache) noexcept;
    [[nodiscard]] GeneratedOverlayFunctionResult lookup_function(
        GeneratedOverlayToken lifetime,
        std::uint32_t function_offset) noexcept;
    // Bridge for the generated get_function(vram) ABI. Static sections and
    // active overlay text ranges are checked before the generated table is
    // queried; inactive and dependency-invalidated overlays fail closed.
    [[nodiscard]] GeneratedOverlayFunctionResult lookup_function_by_guest_address(
        std::uint32_t guest_address) noexcept;
    // Dispatch-and-call bridge for the generated get_function(vram) ABI. It
    // succeeds only inside an existing outer handle invocation lease and
    // never exposes the resolved raw function pointer.
    [[nodiscard]] GeneratedOverlayError invoke_leased_function_by_guest_address(
        std::uint32_t guest_address,
        std::uint8_t* rdram,
        ::recomp_context* context) noexcept;
    [[nodiscard]] GeneratedOverlayFunctionResult resolve_dependency(
        GeneratedOverlayToken owner_lifetime,
        std::uint64_t opaque_reference_id) noexcept;

private:
    struct FunctionBinding {
        std::uint64_t target_module_id = 0U;
        std::uint64_t target_generation = 0U;
        GeneratedOverlayFunction function = nullptr;
    };

    struct ModuleRecord {
        GeneratedOverlayImage image;
        GeneratedSectionExtents extents;
        std::uint32_t guest_base = 0U;
        std::size_t memory_offset = 0U;
        std::uint64_t generation = 0U;
        bool active = false;
        bool custom_bindings_valid = true;
        // A sorted private copy of the generated R32 sites checked against
        // custom sites at image admission. It is re-read and compared just
        // before the generated relocation callback runs.
        std::vector<std::uint32_t> validated_generated_relocation_sites;
        std::vector<GeneratedR32RelocationDescriptor>
            validated_generated_relocation_descriptors;
        // Overlay modules named by generated R32 descriptors. These are kept
        // apart from function bindings because their target offsets may name
        // text, initialized data, or BSS.
        std::vector<std::uint64_t> r32_provider_module_ids;
        bool r32_bindings_valid = true;
        bool execution_valid = true;
        std::map<std::uint64_t, FunctionBinding> bindings;
        std::map<std::uint64_t,
            std::shared_ptr<GeneratedOverlayDispatchGate>> dispatch_gates;
        std::map<std::uint64_t,
            std::shared_ptr<GeneratedOverlayDispatchGate>>
                dependency_dispatch_gates;
    };

    struct PreparedDependentBindings {
        ModuleRecord* module = nullptr;
        std::map<std::uint64_t, FunctionBinding> bindings;
    };

    struct PreparedCustomRebind {
        ModuleRecord* module = nullptr;
        std::vector<std::byte> staged_bytes;
        GeneratedInstructionCacheRange cache_range{};
    };

    struct PreparedGeneratedR32Rebind {
        ModuleRecord* module = nullptr;
        std::vector<std::byte> staged_bytes;
        GeneratedInstructionCacheRange cache_range{};
    };

    struct PreparedUnloadModule {
        ModuleRecord* module = nullptr;
        std::vector<std::byte> staged_bytes;
        GeneratedInstructionCacheRange cache_range{};
        bool target = false;
        bool invalidate_function_bindings = false;
        bool invalidate_custom_bindings = false;
        bool invalidate_r32_bindings = false;
    };

    [[nodiscard]] GeneratedOverlayError validate_token(
        const GeneratedOverlayToken& token,
        ModuleRecord*& output,
        bool require_active) noexcept;
    [[nodiscard]] GeneratedOverlayLoadResult activate(
        ModuleRecord& module,
        GeneratedInstructionCacheInvalidator& cache) noexcept;
    [[nodiscard]] GeneratedOverlayError prepare_bindings(
        ModuleRecord& module,
        std::uint64_t candidate_generation,
        const std::set<std::uint64_t>& locally_recoverable,
        std::map<std::uint64_t, FunctionBinding>& own_bindings,
        std::vector<PreparedDependentBindings>& dependent_bindings);
    [[nodiscard]] GeneratedOverlayError validate_custom_image(
        const GeneratedOverlayImage& image,
        const GeneratedSectionExtents& extents,
        std::vector<std::uint32_t>& validated_generated_relocation_sites,
        std::vector<GeneratedR32RelocationDescriptor>&
            validated_generated_relocation_descriptors) const;
    [[nodiscard]] GeneratedOverlayError validate_generated_relocation_sites_for_apply(
        const ModuleRecord& module,
        std::size_t expected_relocations) const;
    [[nodiscard]] GeneratedOverlayError validate_generated_r32_dependencies(
        ModuleRecord& module);
    [[nodiscard]] GeneratedOverlayError prepare_generated_r32_rebinds(
        ModuleRecord& provider,
        std::vector<PreparedGeneratedR32Rebind>& rebinds) const;
    [[nodiscard]] GeneratedOverlayError reapply_generated_r32_relocations(
        ModuleRecord& module,
        std::vector<std::byte>& staged,
        const std::set<std::uint64_t>* pending_providers = nullptr) const;
    [[nodiscard]] GeneratedOverlayError apply_custom_relocations(
        ModuleRecord& module,
        std::span<std::byte> staged_module_bytes,
        const std::set<std::uint64_t>* pending_providers = nullptr) const;
    [[nodiscard]] GeneratedOverlayError prepare_custom_rebinds(
        ModuleRecord& provider,
        std::vector<PreparedCustomRebind>& rebinds) const;
    [[nodiscard]] GeneratedOverlayFunction lookup_active_offset(
        const ModuleRecord& module,
        std::uint32_t function_offset) const;
    [[nodiscard]] GeneratedOverlayFunctionHandle make_dispatch_handle(
        ModuleRecord* module,
        std::uint64_t function_identity,
        GeneratedOverlayFunction function);
    [[nodiscard]] GeneratedOverlayFunctionHandle make_dependency_dispatch_handle(
        ModuleRecord& owner,
        std::uint64_t opaque_reference_id,
        GeneratedOverlayFunction function);
    [[nodiscard]] bool module_has_direct_reference_to(
        const ModuleRecord& module,
        const std::set<std::uint64_t>& unavailable,
        bool& function_reference,
        bool& custom_reference,
        bool& r32_reference) const noexcept;
    void recompute_execution_validity() noexcept;
    void suspend_dispatch_gates(
        const std::vector<PreparedUnloadModule>& prepared) noexcept;
    void resume_dispatch_gates(
        const std::vector<PreparedUnloadModule>& prepared) noexcept;
    void revoke_dispatch_gates(
        const std::vector<PreparedUnloadModule>& prepared) noexcept;
    [[nodiscard]] bool rollback_section_load(ModuleRecord& module) noexcept;

    std::span<std::byte> guest_memory_;
    std::unique_ptr<GeneratedOverlayTable> table_;
    std::vector<std::int32_t> section_addresses_;
    std::vector<std::int32_t> initial_section_addresses_;
    std::vector<GeneratedSectionExtents> section_extents_;
    std::map<std::uint64_t, std::unique_ptr<ModuleRecord>> modules_;
    std::map<std::uint64_t,
        std::shared_ptr<GeneratedOverlayDispatchGate>> static_dispatch_gates_;
    std::shared_ptr<GeneratedOverlayExecutionAdmission> execution_admission_;
    std::uint64_t runtime_identity_ = 0U;
    std::uint64_t next_generation_ = 1U;
    bool busy_ = false;
    // Sticky diagnostic flag. Read-only preparation paths can discover a
    // published-address mismatch and must be able to record it.
    mutable bool poisoned_ = false;
    std::recursive_mutex mutex_;
};

[[nodiscard]] const char* generated_overlay_error_message(
    GeneratedOverlayError error) noexcept;

}  // namespace jfg
