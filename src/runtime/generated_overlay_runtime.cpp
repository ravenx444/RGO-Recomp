#include "jfg/runtime/generated_overlay_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace jfg {

struct GeneratedOverlayExecutionAdmission final {
    std::mutex mutex;
    std::condition_variable idle;
    std::size_t active_invocations = 0U;
    bool accepting = true;
};

struct GeneratedOverlayDispatchGate final {
    std::mutex mutex;
    GeneratedOverlayRuntime* runtime = nullptr;
    std::shared_ptr<GeneratedOverlayExecutionAdmission> admission;
    GeneratedOverlayFunction target = nullptr;
    std::uint8_t* expected_rdram = nullptr;
    std::size_t active_calls = 0U;
    bool accepting = true;
    bool revoked = false;
};

namespace {

std::atomic<std::uint64_t> next_runtime_identity{1U};

struct InvocationLeaseContext {
    GeneratedOverlayRuntime* runtime = nullptr;
    InvocationLeaseContext* previous = nullptr;
};

thread_local InvocationLeaseContext* active_invocation = nullptr;

void release_gate(
    const std::shared_ptr<GeneratedOverlayDispatchGate>& gate) noexcept {
    const std::lock_guard lock(gate->mutex);
    if (gate->active_calls == 0U) {
        std::terminate();
    }
    --gate->active_calls;
}

[[nodiscard]] std::uint64_t allocate_runtime_identity() noexcept {
    std::uint64_t current = next_runtime_identity.load(std::memory_order_relaxed);
    while (current != 0U) {
        const std::uint64_t next = current + 1U;
        if (next_runtime_identity.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0U;
}

[[nodiscard]] bool add_u32_extent(
    const std::uint32_t base,
    const std::size_t extent,
    std::uint32_t& end) noexcept {
    const std::uint64_t wide_end =
        static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(extent);
    if (wide_end > (std::uint64_t{1U} << 32U)) {
        return false;
    }
    end = static_cast<std::uint32_t>(wide_end & 0xFFFFFFFFU);
    return true;
}

[[nodiscard]] bool ranges_overlap(
    const std::size_t first_offset,
    const std::size_t first_size,
    const std::size_t second_offset,
    const std::size_t second_size) noexcept {
    return first_offset < second_offset + second_size &&
        second_offset < first_offset + first_size;
}

[[nodiscard]] bool valid_extents(
    const GeneratedSectionExtents& extents) noexcept {
    if (extents.text_size == 0U || (extents.text_size & 3U) != 0U ||
        (extents.data_size & 3U) != 0U || (extents.bss_size & 3U) != 0U) {
        return false;
    }
    const std::uint64_t total = static_cast<std::uint64_t>(extents.text_size) +
        extents.data_size + extents.bss_size;
    return total <= std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool same_extents(
    const GeneratedSectionExtents& first,
    const GeneratedSectionExtents& second) noexcept {
    return first.text_size == second.text_size &&
        first.data_size == second.data_size &&
        first.bss_size == second.bss_size &&
        first.is_overlay == second.is_overlay;
}

[[nodiscard]] std::size_t initialized_size(
    const GeneratedSectionExtents& extents) noexcept {
    return static_cast<std::size_t>(extents.text_size) + extents.data_size;
}

[[nodiscard]] std::size_t mapped_size(
    const GeneratedSectionExtents& extents) noexcept {
    return initialized_size(extents) + extents.bss_size;
}

[[nodiscard]] std::uint32_t read_be32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
        std::to_integer<std::uint32_t>(bytes[offset + 3U]);
}

}  // namespace

bool GeneratedOverlayFunctionHandle::valid() const noexcept {
    if (gate_ == nullptr) {
        return false;
    }
    const std::lock_guard lock(gate_->mutex);
    return gate_->accepting && !gate_->revoked && gate_->target != nullptr &&
        gate_->runtime != nullptr;
}

GeneratedOverlayError GeneratedOverlayFunctionHandle::invoke(
    std::uint8_t* const rdram,
    ::recomp_context* const context) const noexcept {
    if (gate_ == nullptr) {
        return GeneratedOverlayError::stale_token;
    }

    const std::shared_ptr<GeneratedOverlayExecutionAdmission> admission =
        gate_->admission;
    if (admission == nullptr || rdram == nullptr || context == nullptr) {
        return GeneratedOverlayError::invalid_mapping;
    }
    {
        const std::lock_guard admission_lock(admission->mutex);
        if (!admission->accepting) {
            return GeneratedOverlayError::busy;
        }
        ++admission->active_invocations;
    }

    GeneratedOverlayFunction target = nullptr;
    GeneratedOverlayRuntime* runtime = nullptr;
    {
        const std::lock_guard lock(gate_->mutex);
        if (!gate_->accepting || gate_->revoked || gate_->target == nullptr ||
            gate_->runtime == nullptr || gate_->expected_rdram != rdram) {
            const std::lock_guard admission_lock(admission->mutex);
            --admission->active_invocations;
            if (admission->active_invocations == 0U) {
                admission->idle.notify_all();
            }
            return GeneratedOverlayError::stale_token;
        }
        ++gate_->active_calls;
        target = gate_->target;
        runtime = gate_->runtime;
    }

    InvocationLeaseContext invocation{};
    invocation.runtime = runtime;
    invocation.previous = active_invocation;
    active_invocation = &invocation;
    try {
        target(rdram, context);
    }
    catch (...) {
        active_invocation = invocation.previous;
        release_gate(gate_);
        const std::lock_guard admission_lock(admission->mutex);
        --admission->active_invocations;
        if (admission->active_invocations == 0U) {
            admission->idle.notify_all();
        }
        return GeneratedOverlayError::callback_exception;
    }
    active_invocation = invocation.previous;
    release_gate(gate_);
    {
        const std::lock_guard admission_lock(admission->mutex);
        --admission->active_invocations;
        if (admission->active_invocations == 0U) {
            admission->idle.notify_all();
        }
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayRuntime::GeneratedOverlayRuntime(
    const std::span<std::byte> guest_memory)
    : guest_memory_(guest_memory),
      execution_admission_(
          std::make_shared<GeneratedOverlayExecutionAdmission>()),
      runtime_identity_(allocate_runtime_identity()) {}

GeneratedOverlayRuntime::~GeneratedOverlayRuntime() noexcept {
    // As with every C++ object, callers must finish concurrent operations
    // before destruction. Avoid a potentially throwing mutex acquisition in
    // this noexcept cleanup path.
    busy_ = true;
    {
        const std::lock_guard admission_lock(execution_admission_->mutex);
        execution_admission_->accepting = false;
    }
    for (auto& [ignored_id, module] : modules_) {
        (void)ignored_id;
        for (auto& [ignored_offset, gate] : module->dispatch_gates) {
            (void)ignored_offset;
            const std::lock_guard gate_lock(gate->mutex);
            gate->accepting = false;
            gate->revoked = true;
            gate->target = nullptr;
            gate->runtime = nullptr;
        }
        for (auto& [ignored_reference, gate] :
             module->dependency_dispatch_gates) {
            (void)ignored_reference;
            const std::lock_guard gate_lock(gate->mutex);
            gate->accepting = false;
            gate->revoked = true;
            gate->target = nullptr;
            gate->runtime = nullptr;
        }
        if (module->active) {
            std::fill(
                guest_memory_.begin() +
                    static_cast<std::ptrdiff_t>(module->memory_offset),
                guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                    module->memory_offset + mapped_size(module->extents)),
                std::byte{0});
            module->active = false;
        }
    }
    for (auto& [ignored_address, gate] : static_dispatch_gates_) {
        (void)ignored_address;
        const std::lock_guard gate_lock(gate->mutex);
        gate->accepting = false;
        gate->revoked = true;
        gate->target = nullptr;
        gate->runtime = nullptr;
    }
    // Destruction cannot safely invoke user/generated callbacks because their
    // captured dependencies may already be gone. The owned adapter releases
    // its generated global binding while section_addresses_ is still alive.
    table_.reset();
}

GeneratedOverlayInstallResult GeneratedOverlayRuntime::install_table(
    std::unique_ptr<GeneratedOverlayTable> table) noexcept {
    const std::lock_guard lock(mutex_);
    GeneratedOverlayInstallResult result{};
    if (busy_) {
        result.error = GeneratedOverlayError::busy;
        return result;
    }
    if (poisoned_) {
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    if (runtime_identity_ == 0U) {
        result.error = GeneratedOverlayError::identity_exhausted;
        return result;
    }
    if (table_ != nullptr) {
        result.error = GeneratedOverlayError::table_already_installed;
        return result;
    }
    if (table == nullptr) {
        result.error = GeneratedOverlayError::invalid_table;
        return result;
    }

    std::vector<std::int32_t> addresses;
    std::vector<GeneratedSectionExtents> extents;
    busy_ = true;
    try {
        const std::size_t count = table->section_count();
        if (count == 0U || count > kMaximumGeneratedSectionCount ||
            count > std::numeric_limits<std::uint32_t>::max() ||
            table->section_count() != count) {
            result.error = GeneratedOverlayError::invalid_section_count;
            table.reset();
            busy_ = false;
            return result;
        }

        addresses.assign(count, 0);
        extents.resize(count);
        if (!table->initialize_sections(addresses)) {
            result.error = GeneratedOverlayError::section_initialization_failed;
            table.reset();
            busy_ = false;
            return result;
        }
        for (std::size_t index = 0U; index < count; ++index) {
            GeneratedSectionExtents current{};
            if (!table->section_extents(
                    static_cast<std::uint32_t>(index), current) ||
                !valid_extents(current)) {
                result.error = GeneratedOverlayError::invalid_section_metadata;
                table.reset();
                busy_ = false;
                return result;
            }
            if (current.is_overlay && addresses[index] != 0) {
                result.error = GeneratedOverlayError::initial_overlay_published;
                table.reset();
                busy_ = false;
                return result;
            }
            if (!current.is_overlay && addresses[index] == 0) {
                result.error = GeneratedOverlayError::section_initialization_failed;
                table.reset();
                busy_ = false;
                return result;
            }
            if (!current.is_overlay) {
                const std::uint32_t base =
                    static_cast<std::uint32_t>(addresses[index]);
                const std::size_t extent = mapped_size(current);
                if (base < kGeneratedCachedGuestBase) {
                    result.error =
                        GeneratedOverlayError::section_initialization_failed;
                    table.reset();
                    busy_ = false;
                    return result;
                }
                const std::size_t offset = static_cast<std::size_t>(
                    base - kGeneratedCachedGuestBase);
                if (offset > guest_memory_.size() ||
                    extent > guest_memory_.size() - offset) {
                    result.error =
                        GeneratedOverlayError::section_initialization_failed;
                    table.reset();
                    busy_ = false;
                    return result;
                }
            }
            extents[index] = current;
        }
        for (std::size_t first = 0U; first < count; ++first) {
            if (extents[first].is_overlay) {
                continue;
            }
            const std::size_t first_offset = static_cast<std::size_t>(
                static_cast<std::uint32_t>(addresses[first]) -
                kGeneratedCachedGuestBase);
            for (std::size_t second = first + 1U; second < count; ++second) {
                if (extents[second].is_overlay) {
                    continue;
                }
                const std::size_t second_offset = static_cast<std::size_t>(
                    static_cast<std::uint32_t>(addresses[second]) -
                    kGeneratedCachedGuestBase);
                if (ranges_overlap(
                        first_offset,
                        mapped_size(extents[first]),
                        second_offset,
                        mapped_size(extents[second]))) {
                    result.error =
                        GeneratedOverlayError::invalid_section_metadata;
                    table.reset();
                    busy_ = false;
                    return result;
                }
            }
        }
        if (table->section_count() != count) {
            result.error = GeneratedOverlayError::invalid_section_count;
            table.reset();
            busy_ = false;
            return result;
        }

        section_addresses_ = std::move(addresses);
        section_extents_ = std::move(extents);
        initial_section_addresses_ = section_addresses_;
        table_ = std::move(table);
        busy_ = false;
        return result;
    }
    catch (const std::bad_alloc&) {
        table.reset();
        section_addresses_.clear();
        initial_section_addresses_.clear();
        section_extents_.clear();
        busy_ = false;
        result.error = GeneratedOverlayError::internal_failure;
        return result;
    }
    catch (...) {
        // If initialization bound the adapter to addresses, release that
        // binding before the backing vector leaves scope.
        table.reset();
        busy_ = false;
        result.error = GeneratedOverlayError::callback_exception;
        return result;
    }
}

GeneratedOverlayLoadResult GeneratedOverlayRuntime::load(
    GeneratedOverlayImage image,
    const std::uint32_t guest_base,
    GeneratedInstructionCacheInvalidator& cache) noexcept {
    const std::lock_guard lock(mutex_);
    GeneratedOverlayLoadResult result{};
    if (busy_) {
        result.error = GeneratedOverlayError::busy;
        return result;
    }
    if (poisoned_) {
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    if (table_ == nullptr) {
        result.error = GeneratedOverlayError::table_not_installed;
        return result;
    }
    if (runtime_identity_ == 0U || next_generation_ == 0U) {
        result.error = GeneratedOverlayError::identity_exhausted;
        return result;
    }
    if (image.opaque_module_id == 0U ||
        image.section_index >= section_extents_.size()) {
        result.error = GeneratedOverlayError::invalid_image;
        return result;
    }

    const auto existing = modules_.find(image.opaque_module_id);
    if (existing != modules_.end()) {
        result.error = existing->second->active
            ? GeneratedOverlayError::duplicate_module
            : GeneratedOverlayError::reload_required;
        return result;
    }
    for (const auto& [ignored_id, module] : modules_) {
        (void)ignored_id;
        if (module->image.section_index == image.section_index) {
            result.error = GeneratedOverlayError::duplicate_module;
            return result;
        }
    }

    const GeneratedSectionExtents extents =
        section_extents_[image.section_index];
    if (!extents.is_overlay ||
        image.initialized_bytes.size() != initialized_size(extents)) {
        result.error = GeneratedOverlayError::invalid_image;
        return result;
    }
    std::vector<std::uint32_t> validated_generated_relocation_sites;
    std::vector<GeneratedR32RelocationDescriptor>
        validated_generated_relocation_descriptors;
    result.error = validate_custom_image(
        image, extents, validated_generated_relocation_sites,
        validated_generated_relocation_descriptors);
    if (result.error != GeneratedOverlayError::none) {
        return result;
    }

    std::set<std::uint64_t> reference_ids;
    try {
        for (const GeneratedOverlayDependency& dependency : image.dependencies) {
            if (dependency.opaque_reference_id == 0U ||
                dependency.target_module_id == 0U ||
                dependency.target_module_id == image.opaque_module_id ||
                (dependency.function_offset & 3U) != 0U ||
                !reference_ids.insert(dependency.opaque_reference_id).second) {
                result.error = GeneratedOverlayError::invalid_dependency;
                return result;
            }
        }
    }
    catch (const std::bad_alloc&) {
        result.error = GeneratedOverlayError::internal_failure;
        return result;
    }
    catch (...) {
        result.error = GeneratedOverlayError::internal_failure;
        return result;
    }

    const std::size_t extent = mapped_size(extents);
    std::uint32_t guest_end = 0U;
    if ((guest_base & 3U) != 0U || guest_base < kGeneratedCachedGuestBase ||
        !add_u32_extent(guest_base, extent, guest_end)) {
        result.error = GeneratedOverlayError::invalid_mapping;
        return result;
    }
    (void)guest_end;
    const std::size_t memory_offset =
        static_cast<std::size_t>(guest_base - kGeneratedCachedGuestBase);
    if (memory_offset > guest_memory_.size() ||
        extent > guest_memory_.size() - memory_offset) {
        result.error = GeneratedOverlayError::invalid_mapping;
        return result;
    }
    for (const auto& [ignored_id, module] : modules_) {
        (void)ignored_id;
        if (module->active && ranges_overlap(
                memory_offset,
                extent,
                module->memory_offset,
                mapped_size(module->extents))) {
            result.error = GeneratedOverlayError::overlapping_mapping;
            return result;
        }
    }
    for (std::size_t index = 0U; index < section_extents_.size(); ++index) {
        if (section_extents_[index].is_overlay) {
            continue;
        }
        const std::uint32_t static_base =
            static_cast<std::uint32_t>(section_addresses_[index]);
        const std::size_t static_offset = static_cast<std::size_t>(
            static_base - kGeneratedCachedGuestBase);
        if (ranges_overlap(
                memory_offset,
                extent,
                static_offset,
                mapped_size(section_extents_[index]))) {
            result.error = GeneratedOverlayError::overlapping_mapping;
            return result;
        }
    }

    try {
        auto module = std::make_unique<ModuleRecord>();
        module->image = std::move(image);
        module->extents = extents;
        module->guest_base = guest_base;
        module->memory_offset = memory_offset;
        module->validated_generated_relocation_sites =
            std::move(validated_generated_relocation_sites);
        module->validated_generated_relocation_descriptors =
            std::move(validated_generated_relocation_descriptors);
        const std::uint64_t module_id = module->image.opaque_module_id;
        ModuleRecord* const module_pointer = module.get();
        const auto [iterator, inserted] = modules_.emplace(
            module_id,
            std::move(module));
        (void)iterator;
        if (!inserted) {
            result.error = GeneratedOverlayError::duplicate_module;
            return result;
        }

        result = activate(*module_pointer, cache);
        if (!result.ok()) {
            modules_.erase(module_id);
        }
        return result;
    }
    catch (const std::bad_alloc&) {
        result.error = GeneratedOverlayError::internal_failure;
        return result;
    }
    catch (...) {
        result.error = GeneratedOverlayError::internal_failure;
        return result;
    }
}

GeneratedOverlayLoadResult GeneratedOverlayRuntime::reload_same_base(
    const GeneratedOverlayToken prior_lifetime,
    GeneratedInstructionCacheInvalidator& cache) noexcept {
    const std::lock_guard lock(mutex_);
    GeneratedOverlayLoadResult result{};
    if (busy_) {
        result.error = GeneratedOverlayError::busy;
        return result;
    }
    if (poisoned_) {
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    ModuleRecord* module = nullptr;
    result.error = validate_token(prior_lifetime, module, false);
    if (result.error != GeneratedOverlayError::none) {
        return result;
    }
    if (table_ == nullptr) {
        result.error = GeneratedOverlayError::table_not_installed;
        return result;
    }
    if (next_generation_ == 0U) {
        result.error = GeneratedOverlayError::identity_exhausted;
        return result;
    }
    for (const auto& [ignored_id, current] : modules_) {
        (void)ignored_id;
        if (current.get() != module && current->active && ranges_overlap(
                module->memory_offset,
                mapped_size(module->extents),
                current->memory_offset,
                mapped_size(current->extents))) {
            result.error = GeneratedOverlayError::overlapping_mapping;
            return result;
        }
    }
    return activate(*module, cache);
}

GeneratedOverlayError GeneratedOverlayRuntime::validate_custom_image(
    const GeneratedOverlayImage& image,
    const GeneratedSectionExtents& extents,
    std::vector<std::uint32_t>& validated_generated_relocation_sites,
    std::vector<GeneratedR32RelocationDescriptor>&
        validated_generated_relocation_descriptors) const {
    validated_generated_relocation_sites.clear();
    validated_generated_relocation_descriptors.clear();
    if (image.custom_relocations.size() >
            kMaximumCustomOverlayRelocationsPerSection ||
        image.custom_bindings.size() >
            kMaximumCustomOverlayRelocationsPerSection) {
        return GeneratedOverlayError::invalid_custom_relocation;
    }
    std::set<std::uint32_t> required;
    std::set<std::uint32_t> bindings;
    std::set<std::uint32_t> sites;
    try {
        for (const CustomOverlayRelocationRecord& record :
             image.custom_relocations) {
            const bool source_valid =
                record.source == CustomOverlayRelocationSource::external ||
                record.source == CustomOverlayRelocationSource::local_offset ||
                record.source == CustomOverlayRelocationSource::local_jump ||
                record.source == CustomOverlayRelocationSource::external_data;
            const bool patch_valid =
                record.patch == CustomOverlayRelocationPatch::full_word ||
                record.patch == CustomOverlayRelocationPatch::jump_target ||
                record.patch == CustomOverlayRelocationPatch::hi16 ||
                record.patch == CustomOverlayRelocationPatch::lo16;
            if (!source_valid || !patch_valid || (record.site_offset & 3U) != 0U ||
                record.site_offset > initialized_size(extents) ||
                initialized_size(extents) - record.site_offset < 4U ||
                !sites.insert(record.site_offset).second ||
                (record.source == CustomOverlayRelocationSource::external_data &&
                 record.site_offset < extents.text_size) ||
                (record.source != CustomOverlayRelocationSource::external_data &&
                 record.patch != CustomOverlayRelocationPatch::full_word &&
                 record.site_offset >= extents.text_size)) {
                return GeneratedOverlayError::invalid_custom_relocation;
            }
            if (record.source == CustomOverlayRelocationSource::external ||
                record.source == CustomOverlayRelocationSource::external_data) {
                if (record.target == 0U) {
                    return GeneratedOverlayError::invalid_custom_relocation;
                }
                required.insert(record.target);
            }
            else if (record.source == CustomOverlayRelocationSource::local_jump &&
                     record.target != 0U) {
                return GeneratedOverlayError::invalid_custom_relocation;
            }
            else if (record.source == CustomOverlayRelocationSource::local_offset &&
                     ((record.target & 3U) != 0U ||
                      record.target >= mapped_size(extents))) {
                return GeneratedOverlayError::invalid_custom_relocation;
            }
        }
        for (const GeneratedOverlayCustomBinding& binding :
             image.custom_bindings) {
            if (binding.opaque_target_id == 0U ||
                !bindings.insert(binding.opaque_target_id).second ||
                required.find(binding.opaque_target_id) == required.end()) {
                return GeneratedOverlayError::invalid_custom_binding;
            }
            if (binding.target_module_id != 0U) {
                if (binding.static_section_index != kNoGeneratedStaticSection) {
                    return GeneratedOverlayError::invalid_custom_binding;
                }
            }
            else if (binding.static_section_index >= section_extents_.size() ||
                     section_extents_[binding.static_section_index].is_overlay ||
                     binding.target_offset >= mapped_size(
                         section_extents_[binding.static_section_index])) {
                return GeneratedOverlayError::invalid_custom_binding;
            }
        }
        if (required.size() != bindings.size()) {
            return GeneratedOverlayError::invalid_custom_binding;
        }
        if (table_ == nullptr) {
            return GeneratedOverlayError::table_not_installed;
        }
        std::span<const std::uint32_t> generated_sites;
        std::span<const GeneratedR32RelocationDescriptor> descriptors;
        const std::size_t expected = table_->relocation_count(image.section_index);
        if (expected > kMaximumGeneratedRelocationsPerSection ||
            expected > initialized_size(extents) / sizeof(std::uint32_t)) {
            return GeneratedOverlayError::invalid_relocation_count;
        }
        if (!table_->relocation_sites(image.section_index, generated_sites) ||
            !table_->relocation_descriptors(image.section_index, descriptors) ||
            generated_sites.size() != expected || descriptors.size() != expected) {
            return GeneratedOverlayError::relocation_descriptor_mismatch;
        }
        validated_generated_relocation_sites.assign(
            generated_sites.begin(), generated_sites.end());
        validated_generated_relocation_descriptors.assign(
            descriptors.begin(), descriptors.end());
        for (std::size_t index = 0U; index < expected; ++index) {
            const std::uint32_t site = generated_sites[index];
            const GeneratedR32RelocationDescriptor& descriptor = descriptors[index];
            if ((site & 3U) != 0U || site > initialized_size(extents) ||
                initialized_size(extents) - site < sizeof(std::uint32_t) ||
                (index != 0U && generated_sites[index - 1U] >= site) ||
                descriptor.site_offset != site ||
                descriptor.target_section >= section_extents_.size() ||
                descriptor.target_offset >= mapped_size(
                    section_extents_[descriptor.target_section])) {
                return GeneratedOverlayError::relocation_descriptor_mismatch;
            }
        }
        for (const CustomOverlayRelocationRecord& record : image.custom_relocations) {
            if (std::binary_search(
                    validated_generated_relocation_sites.begin(),
                    validated_generated_relocation_sites.end(),
                    record.site_offset)) {
                return GeneratedOverlayError::custom_relocation_overlap;
            }
        }
    }
    catch (const std::bad_alloc&) {
        return GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        // The only externally supplied operations in this validation path are
        // generated-table callbacks. Surface their failure consistently with
        // lifecycle, relocation, and lookup callbacks.
        return GeneratedOverlayError::callback_exception;
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayError
GeneratedOverlayRuntime::validate_generated_relocation_sites_for_apply(
    const ModuleRecord& module,
    const std::size_t expected_relocations) const {
    try {
        std::span<const std::uint32_t> current_sites;
        std::span<const GeneratedR32RelocationDescriptor> current_descriptors;
        if (!table_->relocation_sites(module.image.section_index, current_sites) ||
            !table_->relocation_descriptors(
                module.image.section_index, current_descriptors) ||
            current_sites.size() != expected_relocations ||
            current_descriptors.size() != expected_relocations) {
            return GeneratedOverlayError::relocation_site_mismatch;
        }
        std::vector<std::uint32_t> canonical(
            current_sites.begin(), current_sites.end());
        if (canonical != module.validated_generated_relocation_sites ||
            !std::equal(
                current_descriptors.begin(), current_descriptors.end(),
                module.validated_generated_relocation_descriptors.begin(),
                module.validated_generated_relocation_descriptors.end(),
                [](const GeneratedR32RelocationDescriptor& first,
                   const GeneratedR32RelocationDescriptor& second) {
                    return first.site_offset == second.site_offset &&
                        first.target_section == second.target_section &&
                        first.target_offset == second.target_offset;
                })) {
            return GeneratedOverlayError::relocation_site_mismatch;
        }
    }
    catch (const std::bad_alloc&) {
        return GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        return GeneratedOverlayError::callback_exception;
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayError GeneratedOverlayRuntime::validate_generated_r32_dependencies(
    ModuleRecord& module) {
    try {
        std::vector<std::uint64_t> providers;
        for (const GeneratedR32RelocationDescriptor& descriptor :
             module.validated_generated_relocation_descriptors) {
            if (descriptor.target_section >= section_extents_.size() ||
                descriptor.target_offset >= mapped_size(
                    section_extents_[descriptor.target_section])) {
                return GeneratedOverlayError::relocation_descriptor_mismatch;
            }
            if (!section_extents_[descriptor.target_section].is_overlay) {
                if (section_addresses_[descriptor.target_section] !=
                    initial_section_addresses_[descriptor.target_section]) {
                    poisoned_ = true;
                    return GeneratedOverlayError::runtime_poisoned;
                }
                continue;
            }
            ModuleRecord* target = nullptr;
            if (descriptor.target_section == module.image.section_index) {
                target = &module;
            }
            else {
                for (auto& [ignored_id, candidate] : modules_) {
                    (void)ignored_id;
                    if (candidate->image.section_index == descriptor.target_section) {
                        target = candidate.get();
                        break;
                    }
                }
            }
            if (target == nullptr || (!target->active && target != &module) ||
                section_addresses_[descriptor.target_section] !=
                    static_cast<std::int32_t>(target->guest_base)) {
                return GeneratedOverlayError::dependency_unresolved;
            }
            if (target != &module) {
                providers.push_back(target->image.opaque_module_id);
            }
        }
        std::sort(providers.begin(), providers.end());
        providers.erase(std::unique(providers.begin(), providers.end()), providers.end());
        module.r32_provider_module_ids.swap(providers);
        module.r32_bindings_valid = true;
    }
    catch (const std::bad_alloc&) {
        return GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        return GeneratedOverlayError::callback_exception;
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayError GeneratedOverlayRuntime::reapply_generated_r32_relocations(
    ModuleRecord& module,
    std::vector<std::byte>& staged,
    const std::set<std::uint64_t>* const pending_providers) const {
    try {
        if (staged.size() != guest_memory_.size() || table_ == nullptr) {
            return GeneratedOverlayError::internal_failure;
        }
        const std::size_t extent = mapped_size(module.extents);
        const std::size_t initialized = initialized_size(module.extents);
        if (module.memory_offset > staged.size() || extent > staged.size() - module.memory_offset) {
            return GeneratedOverlayError::invalid_mapping;
        }
        // Both callers stage a copy of live guest memory and rewrite only this
        // module's window before calling, so live memory itself is the
        // reference for every byte outside the window. Copying all of RDRAM
        // twice more per activation (original and pre-relocation snapshots)
        // was the dominant cost of an overlay load.
        for (const GeneratedR32RelocationDescriptor& descriptor :
             module.validated_generated_relocation_descriptors) {
            if (descriptor.site_offset > module.image.initialized_bytes.size() ||
                module.image.initialized_bytes.size() - descriptor.site_offset < 4U) {
                return GeneratedOverlayError::relocation_descriptor_mismatch;
            }
            std::copy_n(
                module.image.initialized_bytes.begin() +
                    static_cast<std::ptrdiff_t>(descriptor.site_offset),
                4U,
                staged.begin() + static_cast<std::ptrdiff_t>(
                    module.memory_offset + descriptor.site_offset));
        }
        const std::vector<std::byte> pre_relocation_window(
            staged.begin() + static_cast<std::ptrdiff_t>(module.memory_offset),
            staged.begin() + static_cast<std::ptrdiff_t>(
                module.memory_offset + initialized));
        // One flag per initialized byte: declared relocation-site bytes may
        // change, every other byte must not. Building the mask once replaces
        // a per-byte scan over every descriptor.
        std::vector<bool> declared_site_byte(initialized, false);
        for (const GeneratedR32RelocationDescriptor& descriptor :
             module.validated_generated_relocation_descriptors) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                const std::size_t offset = descriptor.site_offset + byte;
                if (offset < initialized) {
                    declared_site_byte[offset] = true;
                }
            }
        }
        const std::size_t expected =
            module.validated_generated_relocation_descriptors.size();
        const GeneratedOverlayError sites =
            validate_generated_relocation_sites_for_apply(module, expected);
        if (sites != GeneratedOverlayError::none) {
            return sites;
        }
        const GeneratedRelocationResult result = table_->apply_relocations_checked(
            std::span<std::uint8_t>(
                reinterpret_cast<std::uint8_t*>(staged.data()), staged.size()),
            module.image.section_index,
            expected);
        if (!result.applied) {
            return GeneratedOverlayError::relocation_failed;
        }
        if (result.applied_count != expected ||
            table_->relocation_count(module.image.section_index) != expected) {
            return GeneratedOverlayError::relocation_count_mismatch;
        }
        if (!std::equal(
                staged.begin(),
                staged.begin() + static_cast<std::ptrdiff_t>(module.memory_offset),
                guest_memory_.begin()) ||
            !std::equal(
                staged.begin() + static_cast<std::ptrdiff_t>(
                    module.memory_offset + extent),
                staged.end(),
                guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                    module.memory_offset + extent))) {
            return GeneratedOverlayError::relocation_wrote_outside_section;
        }
        for (std::size_t offset = 0U; offset < initialized; ++offset) {
            if (!declared_site_byte[offset] &&
                staged[module.memory_offset + offset] !=
                    pre_relocation_window[offset]) {
                return GeneratedOverlayError::relocation_wrote_outside_section;
            }
        }
        if (!std::all_of(
                staged.begin() + static_cast<std::ptrdiff_t>(
                    module.memory_offset + initialized),
                staged.begin() + static_cast<std::ptrdiff_t>(
                    module.memory_offset + extent),
                [](const std::byte value) { return value == std::byte{0}; })) {
            return GeneratedOverlayError::bss_not_zero;
        }
        for (const GeneratedR32RelocationDescriptor& descriptor :
             module.validated_generated_relocation_descriptors) {
            std::uint32_t base = 0U;
            if (!section_extents_[descriptor.target_section].is_overlay) {
                base = static_cast<std::uint32_t>(
                    section_addresses_[descriptor.target_section]);
            }
            else {
                const ModuleRecord* target = descriptor.target_section ==
                        module.image.section_index ? &module : nullptr;
                if (target == nullptr) {
                    for (const auto& [ignored_id, candidate] : modules_) {
                        (void)ignored_id;
                        if (candidate->image.section_index == descriptor.target_section) {
                            target = candidate.get();
                            break;
                        }
                    }
                }
                const bool pending = target != nullptr &&
                    pending_providers != nullptr &&
                    pending_providers->contains(
                        target->image.opaque_module_id);
                if (target == nullptr ||
                    (!target->active && target != &module && !pending) ||
                    (target != &module && !target->execution_valid && !pending)) {
                    return GeneratedOverlayError::dependency_unresolved;
                }
                base = target->guest_base;
            }
            std::uint32_t expected_value = 0U;
            if (!try_add_generated_guest_address(
                    base, descriptor.target_offset, expected_value) ||
                read_be32(std::span<const std::byte>(staged).subspan(
                    module.memory_offset), descriptor.site_offset) != expected_value) {
                return GeneratedOverlayError::relocation_descriptor_mismatch;
            }
        }
    }
    catch (const std::bad_alloc&) {
        return GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        return GeneratedOverlayError::callback_exception;
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayError GeneratedOverlayRuntime::prepare_generated_r32_rebinds(
    ModuleRecord& provider,
    std::vector<PreparedGeneratedR32Rebind>& rebinds) const {
    try {
        std::set<std::uint64_t> pending_providers{
            provider.image.opaque_module_id};
        for (const auto& [module_id, candidate] : modules_) {
            if (candidate->active) {
                pending_providers.insert(module_id);
            }
        }
        std::set<std::uint64_t> staged_modules;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [module_id, owner] : modules_) {
                if (!owner->active || owner.get() == &provider ||
                    owner->r32_bindings_valid ||
                    staged_modules.contains(module_id)) {
                    continue;
                }
                const bool every_provider_available = std::all_of(
                    owner->r32_provider_module_ids.begin(),
                    owner->r32_provider_module_ids.end(),
                    [this, &pending_providers](const std::uint64_t provider_id) {
                        const auto found = modules_.find(provider_id);
                        return found != modules_.end() &&
                            (found->second->execution_valid ||
                             pending_providers.contains(provider_id));
                    });
                if (!every_provider_available) {
                    continue;
                }
                PreparedGeneratedR32Rebind prepared{};
                prepared.module = owner.get();
                prepared.staged_bytes.assign(
                    guest_memory_.begin(), guest_memory_.end());
                const GeneratedOverlayError applied =
                    reapply_generated_r32_relocations(
                        *prepared.module, prepared.staged_bytes,
                        &pending_providers);
                if (applied != GeneratedOverlayError::none) {
                    return applied;
                }
                prepared.cache_range = {
                    owner->image.opaque_module_id,
                    owner->guest_base,
                    owner->extents.text_size,
                };
                rebinds.emplace_back(std::move(prepared));
                staged_modules.insert(module_id);
                changed = true;
            }
        }
    }
    catch (const std::bad_alloc&) {
        return GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        return GeneratedOverlayError::callback_exception;
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayError GeneratedOverlayRuntime::apply_custom_relocations(
    ModuleRecord& module,
    const std::span<std::byte> staged_module_bytes,
    const std::set<std::uint64_t>* const pending_providers) const {
    if (module.image.custom_relocations.empty()) {
        return GeneratedOverlayError::none;
    }
    class Resolver final : public CustomOverlayRelocationResolver {
    public:
        Resolver(
            const GeneratedOverlayRuntime& runtime,
            const ModuleRecord& source,
            const std::set<std::uint64_t>* pending) noexcept
            : runtime_(runtime), source_(source), pending_(pending) {}
        [[nodiscard]] bool resolve_external(
            const CustomOverlayRelocationSource, const std::uint32_t,
            const std::uint32_t opaque,
            std::uint32_t& address) override {
            const auto binding = std::find_if(
                source_.image.custom_bindings.begin(),
                source_.image.custom_bindings.end(),
                [opaque](const GeneratedOverlayCustomBinding& value) {
                    return value.opaque_target_id == opaque;
                });
            if (binding == source_.image.custom_bindings.end()) {
                return false;
            }
            if (binding->target_module_id == 0U) {
                const auto index = binding->static_section_index;
                if (index >= runtime_.section_extents_.size() ||
                    runtime_.section_extents_[index].is_overlay ||
                    runtime_.section_addresses_[index] !=
                        runtime_.initial_section_addresses_[index] ||
                    binding->target_offset >= mapped_size(
                        runtime_.section_extents_[index])) {
                    return false;
                }
                return try_add_generated_guest_address(
                    static_cast<std::uint32_t>(runtime_.section_addresses_[index]),
                    binding->target_offset,
                    address);
            }
            const auto found = runtime_.modules_.find(binding->target_module_id);
            const ModuleRecord* target = found == runtime_.modules_.end()
                ? nullptr : found->second.get();
            const bool pending = target != nullptr && pending_ != nullptr &&
                pending_->contains(target->image.opaque_module_id);
            if (target == nullptr || (!target->active && !pending &&
                                      target != &source_) ||
                (target != &source_ && !target->execution_valid && !pending) ||
                target->image.section_index >= runtime_.section_addresses_.size() ||
                runtime_.section_addresses_[target->image.section_index] !=
                    static_cast<std::int32_t>(target->guest_base) ||
                binding->target_offset >= mapped_size(target->extents)) {
                return false;
            }
            return try_add_generated_guest_address(
                target->guest_base, binding->target_offset, address);
        }
        [[nodiscard]] bool resolve_local(
            const CustomOverlayRelocationSource source,
            const std::uint32_t,
            const std::uint32_t target_offset,
            std::uint32_t& address) override {
            return (source == CustomOverlayRelocationSource::local_jump ||
                    target_offset < mapped_size(source_.extents)) &&
                try_add_generated_guest_address(
                    source_.guest_base, target_offset, address);
        }
    private:
        const GeneratedOverlayRuntime& runtime_;
        const ModuleRecord& source_;
        const std::set<std::uint64_t>* pending_;
    };

    Resolver resolver(*this, module, pending_providers);
    const CustomOverlayRelocationSection source{
        module.image.section_index,
        module.guest_base,
        module.extents.text_size,
        static_cast<std::uint32_t>(initialized_size(module.extents)),
        static_cast<std::uint32_t>(mapped_size(module.extents)),
    };
    const CustomOverlayRelocationResult applied =
        jfg::apply_custom_overlay_relocations(
            staged_module_bytes, source, module.image.custom_relocations, resolver);
    return applied.applied() ? GeneratedOverlayError::none
                             : GeneratedOverlayError::custom_relocation_failed;
}

GeneratedOverlayError GeneratedOverlayRuntime::prepare_custom_rebinds(
    ModuleRecord& provider,
    std::vector<PreparedCustomRebind>& rebinds) const {
    try {
        std::set<std::uint64_t> pending_providers{
            provider.image.opaque_module_id};
        for (const auto& [module_id, candidate] : modules_) {
            if (candidate->active) {
                pending_providers.insert(module_id);
            }
        }
        std::set<std::uint64_t> staged_modules;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [module_id, owner] : modules_) {
                if (!owner->active || owner.get() == &provider ||
                    owner->custom_bindings_valid ||
                    staged_modules.contains(module_id)) {
                    continue;
                }
                const bool every_provider_available = std::all_of(
                    owner->image.custom_bindings.begin(),
                    owner->image.custom_bindings.end(),
                    [this, &pending_providers, &owner](
                        const GeneratedOverlayCustomBinding& binding) {
                        if (binding.target_module_id == 0U ||
                            binding.target_module_id ==
                                owner->image.opaque_module_id) {
                            return true;
                        }
                        const auto found = modules_.find(binding.target_module_id);
                        return found != modules_.end() &&
                            (found->second->execution_valid ||
                             pending_providers.contains(binding.target_module_id));
                    });
                if (!every_provider_available) {
                    continue;
                }
                if (section_addresses_[owner->image.section_index] !=
                    static_cast<std::int32_t>(owner->guest_base)) {
                    // Every other published-address mismatch poisons the
                    // runtime; reporting poison without recording it would
                    // let later operations proceed on the inconsistent table.
                    poisoned_ = true;
                    return GeneratedOverlayError::runtime_poisoned;
                }
                const std::size_t extent = mapped_size(owner->extents);
                PreparedCustomRebind prepared{};
                prepared.module = owner.get();
                prepared.staged_bytes.assign(
                    guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                        owner->memory_offset),
                    guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                        owner->memory_offset + extent));
                for (const CustomOverlayRelocationRecord& record :
                     owner->image.custom_relocations) {
                    std::copy_n(
                        owner->image.initialized_bytes.begin() +
                            static_cast<std::ptrdiff_t>(record.site_offset),
                        4U,
                        prepared.staged_bytes.begin() +
                            static_cast<std::ptrdiff_t>(record.site_offset));
                }
                const GeneratedOverlayError applied = apply_custom_relocations(
                    *owner, prepared.staged_bytes, &pending_providers);
                if (applied != GeneratedOverlayError::none) {
                    return applied;
                }
                prepared.cache_range = {
                    owner->image.opaque_module_id,
                    owner->guest_base,
                    owner->extents.text_size,
                };
                rebinds.emplace_back(std::move(prepared));
                staged_modules.insert(module_id);
                changed = true;
            }
        }
    }
    catch (...) {
        return GeneratedOverlayError::internal_failure;
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayLoadResult GeneratedOverlayRuntime::activate(
    ModuleRecord& module,
    GeneratedInstructionCacheInvalidator& cache) noexcept {
    GeneratedOverlayLoadResult result{};
    if (table_ == nullptr) {
        result.error = GeneratedOverlayError::table_not_installed;
        return result;
    }
    if (busy_) {
        result.error = GeneratedOverlayError::busy;
        return result;
    }
    if (module.active) {
        result.error = GeneratedOverlayError::module_active;
        return result;
    }

    busy_ = true;
    bool lifecycle_loaded = false;
    std::vector<GeneratedInstructionCacheRange> prepared_cache_ranges;
    const auto abort_prepared = [&cache, &prepared_cache_ranges]() noexcept {
        for (auto iterator = prepared_cache_ranges.rbegin();
             iterator != prepared_cache_ranges.rend(); ++iterator) {
            cache.abort(*iterator);
        }
        prepared_cache_ranges.clear();
    };
    try {
        if (table_->section_count() != section_extents_.size() ||
            initial_section_addresses_.size() != section_extents_.size()) {
            result.error = GeneratedOverlayError::invalid_section_metadata;
            busy_ = false;
            return result;
        }
        for (std::size_t index = 0U; index < section_extents_.size(); ++index) {
            GeneratedSectionExtents current_extents{};
            if (!table_->section_extents(
                    static_cast<std::uint32_t>(index), current_extents) ||
                !same_extents(current_extents, section_extents_[index])) {
                result.error = GeneratedOverlayError::invalid_section_metadata;
                busy_ = false;
                return result;
            }
            if (!section_extents_[index].is_overlay &&
                section_addresses_[index] != initial_section_addresses_[index]) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
                busy_ = false;
                return result;
            }
        }
        if (section_addresses_[module.image.section_index] != 0) {
            poisoned_ = true;
            result.error = GeneratedOverlayError::runtime_poisoned;
            busy_ = false;
            return result;
        }

        const std::size_t extent = mapped_size(module.extents);
        std::vector<std::byte> staged(
            guest_memory_.begin(), guest_memory_.end());
        const std::size_t initialized = initialized_size(module.extents);
        std::copy(
            module.image.initialized_bytes.begin(),
            module.image.initialized_bytes.end(),
            staged.begin() + static_cast<std::ptrdiff_t>(module.memory_offset));
        std::fill(
            staged.begin() + static_cast<std::ptrdiff_t>(
                module.memory_offset + initialized),
            staged.begin() + static_cast<std::ptrdiff_t>(
                module.memory_offset + extent),
            std::byte{0});

        const GeneratedInstructionCacheRange cache_range{
            module.image.opaque_module_id,
            module.guest_base,
            module.extents.text_size,
        };

        const std::size_t expected_relocations =
            table_->relocation_count(module.image.section_index);
        if (expected_relocations > kMaximumGeneratedRelocationsPerSection ||
            expected_relocations > initialized / sizeof(std::uint32_t)) {
            result.error = GeneratedOverlayError::invalid_relocation_count;
            busy_ = false;
            return result;
        }

        const std::int32_t signed_base =
            static_cast<std::int32_t>(module.guest_base);
        if (!table_->update_section_lifecycle(
                GeneratedSectionLifecycle::load,
                module.image.section_index,
                signed_base)) {
            result.error = GeneratedOverlayError::section_load_failed;
            if (section_addresses_[module.image.section_index] != 0 &&
                !rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }
        lifecycle_loaded = true;
        if (section_addresses_[module.image.section_index] != signed_base) {
            result.error = GeneratedOverlayError::section_load_failed;
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }

        result.error = validate_generated_r32_dependencies(module);
        if (result.error != GeneratedOverlayError::none) {
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }

        const GeneratedOverlayError custom_error = apply_custom_relocations(
            module,
            std::span<std::byte>(
                staged.data() + static_cast<std::ptrdiff_t>(module.memory_offset),
                extent));
        if (custom_error != GeneratedOverlayError::none) {
            result.error = custom_error;
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }

        const GeneratedOverlayError r32_error =
            reapply_generated_r32_relocations(module, staged);
        if (r32_error != GeneratedOverlayError::none) {
            result.error = r32_error;
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }

        std::vector<PreparedCustomRebind> custom_rebinds;
        result.error = prepare_custom_rebinds(module, custom_rebinds);
        if (result.error != GeneratedOverlayError::none) {
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }
        std::vector<PreparedGeneratedR32Rebind> r32_rebinds;
        result.error = prepare_generated_r32_rebinds(module, r32_rebinds);
        if (result.error != GeneratedOverlayError::none) {
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }
        std::set<std::uint64_t> locally_recoverable{
            module.image.opaque_module_id};
        for (const auto& [module_id, owner] : modules_) {
            if (!owner->active) {
                continue;
            }
            const bool custom_ready = owner->custom_bindings_valid ||
                std::any_of(
                    custom_rebinds.begin(), custom_rebinds.end(),
                    [&owner](const PreparedCustomRebind& prepared) {
                        return prepared.module == owner.get();
                    });
            const bool r32_ready = owner->r32_bindings_valid ||
                std::any_of(
                    r32_rebinds.begin(), r32_rebinds.end(),
                    [&owner](const PreparedGeneratedR32Rebind& prepared) {
                        return prepared.module == owner.get();
                    });
            if (custom_ready && r32_ready) {
                locally_recoverable.insert(module_id);
            }
        }
        std::map<std::uint64_t, FunctionBinding> own_bindings;
        std::vector<PreparedDependentBindings> dependent_bindings;
        result.error = prepare_bindings(
            module,
            next_generation_,
            locally_recoverable,
            own_bindings,
            dependent_bindings);
        if (result.error != GeneratedOverlayError::none) {
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }

        prepared_cache_ranges.reserve(
            1U + custom_rebinds.size() + r32_rebinds.size());

        // Cache preparation is deliberately the final fallible operation. A
        // successful prepare is followed only by no-fail live-memory, cache,
        // binding, and publication commits.
        prepared_cache_ranges.push_back(cache_range);
        if (!cache.prepare(cache_range)) {
            result.error = GeneratedOverlayError::cache_invalidation_rejected;
            abort_prepared();
            if (!rollback_section_load(module)) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
            }
            busy_ = false;
            return result;
        }
        for (const PreparedCustomRebind& prepared : custom_rebinds) {
            prepared_cache_ranges.push_back(prepared.cache_range);
            if (!cache.prepare(prepared.cache_range)) {
                result.error = GeneratedOverlayError::cache_invalidation_rejected;
                abort_prepared();
                if (!rollback_section_load(module)) {
                    poisoned_ = true;
                    result.error = GeneratedOverlayError::runtime_poisoned;
                }
                busy_ = false;
                return result;
            }
        }
        for (const PreparedGeneratedR32Rebind& prepared : r32_rebinds) {
            prepared_cache_ranges.push_back(prepared.cache_range);
            if (!cache.prepare(prepared.cache_range)) {
                result.error = GeneratedOverlayError::cache_invalidation_rejected;
                abort_prepared();
                if (!rollback_section_load(module)) {
                    poisoned_ = true;
                    result.error = GeneratedOverlayError::runtime_poisoned;
                }
                busy_ = false;
                return result;
            }
        }

        // No potentially failing operation remains beyond this point.
        std::copy(
            staged.begin() + static_cast<std::ptrdiff_t>(module.memory_offset),
            staged.begin() + static_cast<std::ptrdiff_t>(
                module.memory_offset + extent),
            guest_memory_.begin() +
                static_cast<std::ptrdiff_t>(module.memory_offset));
        cache.commit(cache_range);
        for (const PreparedCustomRebind& prepared : custom_rebinds) {
            std::copy(
                prepared.staged_bytes.begin(),
                prepared.staged_bytes.end(),
                guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                    prepared.module->memory_offset));
            cache.commit(prepared.cache_range);
            prepared.module->custom_bindings_valid = true;
        }
        for (const PreparedGeneratedR32Rebind& prepared : r32_rebinds) {
            for (const GeneratedR32RelocationDescriptor& descriptor :
                 prepared.module->validated_generated_relocation_descriptors) {
                std::copy_n(
                    prepared.staged_bytes.begin() + static_cast<std::ptrdiff_t>(
                        prepared.module->memory_offset + descriptor.site_offset),
                    4U,
                    guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                        prepared.module->memory_offset + descriptor.site_offset));
            }
            cache.commit(prepared.cache_range);
            prepared.module->r32_bindings_valid = true;
        }
        module.generation = next_generation_;
        ++next_generation_;
        module.bindings.swap(own_bindings);
        module.active = true;
        for (PreparedDependentBindings& prepared : dependent_bindings) {
            prepared.module->bindings.swap(prepared.bindings);
        }
        recompute_execution_validity();
        prepared_cache_ranges.clear();
        lifecycle_loaded = false;
        result.token = GeneratedOverlayToken(
            runtime_identity_,
            module.image.opaque_module_id,
            module.generation);
        busy_ = false;
        return result;
    }
    catch (const std::bad_alloc&) {
        abort_prepared();
        result.error = GeneratedOverlayError::internal_failure;
        const bool section_may_be_loaded = lifecycle_loaded ||
            (module.image.section_index < section_addresses_.size() &&
             section_addresses_[module.image.section_index] != 0);
        if (section_may_be_loaded && !rollback_section_load(module)) {
            poisoned_ = true;
            result.error = GeneratedOverlayError::runtime_poisoned;
        }
        busy_ = false;
        return result;
    }
    catch (...) {
        abort_prepared();
        result.error = GeneratedOverlayError::callback_exception;
        const bool section_may_be_loaded = lifecycle_loaded ||
            (module.image.section_index < section_addresses_.size() &&
             section_addresses_[module.image.section_index] != 0);
        if (section_may_be_loaded && !rollback_section_load(module)) {
            poisoned_ = true;
            result.error = GeneratedOverlayError::runtime_poisoned;
        }
        busy_ = false;
        return result;
    }
}

GeneratedOverlayError GeneratedOverlayRuntime::prepare_bindings(
    ModuleRecord& module,
    const std::uint64_t candidate_generation,
    const std::set<std::uint64_t>& locally_recoverable,
    std::map<std::uint64_t, FunctionBinding>& own_bindings,
    std::vector<PreparedDependentBindings>& dependent_bindings) {
    for (const GeneratedOverlayDependency& dependency :
         module.image.dependencies) {
        const auto target = modules_.find(dependency.target_module_id);
        if (target == modules_.end() || !target->second->active ||
            !target->second->execution_valid) {
            return GeneratedOverlayError::dependency_unresolved;
        }
        if (section_addresses_[target->second->image.section_index] !=
            static_cast<std::int32_t>(target->second->guest_base)) {
            poisoned_ = true;
            return GeneratedOverlayError::runtime_poisoned;
        }
        if (target->second->bindings.size() !=
            target->second->image.dependencies.size()) {
            return GeneratedOverlayError::dependency_invalidated;
        }
        GeneratedOverlayFunction function = lookup_active_offset(
            *target->second,
            dependency.function_offset);
        if (function == nullptr) {
            return GeneratedOverlayError::function_not_found;
        }
        own_bindings.emplace(
            dependency.opaque_reference_id,
            FunctionBinding{
                dependency.target_module_id,
                target->second->generation,
                function,
            });
    }

    std::set<std::uint64_t> function_available;
    function_available.insert(module.image.opaque_module_id);
    for (const auto& [module_id, candidate] : modules_) {
        if (candidate->active && candidate->execution_valid) {
            function_available.insert(module_id);
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [module_id, owner] : modules_) {
            if (owner.get() == &module || !owner->active ||
                function_available.contains(module_id) ||
                !locally_recoverable.contains(module_id)) {
                continue;
            }
            if (section_addresses_[owner->image.section_index] !=
                static_cast<std::int32_t>(owner->guest_base)) {
                poisoned_ = true;
                return GeneratedOverlayError::runtime_poisoned;
            }
            const bool dependencies_available = std::all_of(
                owner->image.dependencies.begin(),
                owner->image.dependencies.end(),
                [&function_available](
                    const GeneratedOverlayDependency& dependency) {
                    return function_available.contains(
                        dependency.target_module_id);
                });
            if (!dependencies_available) {
                continue;
            }

            PreparedDependentBindings prepared{};
            prepared.module = owner.get();
            for (const GeneratedOverlayDependency& dependency :
                 owner->image.dependencies) {
                const auto target = modules_.find(dependency.target_module_id);
                if (target == modules_.end()) {
                    return GeneratedOverlayError::dependency_unresolved;
                }
                const GeneratedOverlayFunction function = lookup_active_offset(
                    *target->second, dependency.function_offset);
                if (function == nullptr) {
                    return GeneratedOverlayError::function_not_found;
                }
                const std::uint64_t generation = target->second.get() == &module
                    ? candidate_generation
                    : target->second->generation;
                prepared.bindings.emplace(
                    dependency.opaque_reference_id,
                    FunctionBinding{
                        dependency.target_module_id, generation, function});
            }
            dependent_bindings.emplace_back(std::move(prepared));
            function_available.insert(module_id);
            changed = true;
        }
    }
    return GeneratedOverlayError::none;
}

GeneratedOverlayFunction GeneratedOverlayRuntime::lookup_active_offset(
    const ModuleRecord& module,
    const std::uint32_t function_offset) const {
    if (table_ == nullptr || (function_offset & 3U) != 0U ||
        function_offset >= module.extents.text_size) {
        return nullptr;
    }
    const std::uint64_t wide_address =
        static_cast<std::uint64_t>(module.guest_base) + function_offset;
    if (wide_address > std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    return table_->lookup_function(
        static_cast<std::int32_t>(static_cast<std::uint32_t>(wide_address)));
}

bool GeneratedOverlayRuntime::rollback_section_load(
    ModuleRecord& module) noexcept {
    if (table_ == nullptr) {
        return false;
    }
    try {
        if (!table_->update_section_lifecycle(
                GeneratedSectionLifecycle::unload,
                module.image.section_index,
                0)) {
            return false;
        }
        return section_addresses_[module.image.section_index] == 0;
    }
    catch (...) {
        return false;
    }
}

GeneratedOverlayFunctionHandle GeneratedOverlayRuntime::make_dispatch_handle(
    ModuleRecord* const module,
    const std::uint64_t function_identity,
    const GeneratedOverlayFunction function) {
    if (function == nullptr) {
        return {};
    }
    auto& gates = module == nullptr
        ? static_dispatch_gates_
        : module->dispatch_gates;
    const auto existing = gates.find(function_identity);
    if (existing != gates.end()) {
        const std::lock_guard gate_lock(existing->second->mutex);
        if (existing->second->accepting && !existing->second->revoked &&
            existing->second->runtime == this &&
            existing->second->target == function) {
            return GeneratedOverlayFunctionHandle(existing->second);
        }
        existing->second->accepting = false;
        existing->second->revoked = true;
        existing->second->target = nullptr;
        existing->second->runtime = nullptr;
        poisoned_ = true;
        return {};
    }

    auto gate = std::make_shared<GeneratedOverlayDispatchGate>();
    gate->runtime = this;
    gate->admission = execution_admission_;
    gate->target = function;
    gate->expected_rdram = reinterpret_cast<std::uint8_t*>(
        guest_memory_.data());
    gates[function_identity] = gate;
    return GeneratedOverlayFunctionHandle(std::move(gate));
}

GeneratedOverlayFunctionHandle
GeneratedOverlayRuntime::make_dependency_dispatch_handle(
    ModuleRecord& owner,
    const std::uint64_t opaque_reference_id,
    const GeneratedOverlayFunction function) {
    if (function == nullptr) {
        return {};
    }
    const auto existing = owner.dependency_dispatch_gates.find(
        opaque_reference_id);
    if (existing != owner.dependency_dispatch_gates.end()) {
        const std::lock_guard gate_lock(existing->second->mutex);
        if (existing->second->accepting && !existing->second->revoked &&
            existing->second->runtime == this &&
            existing->second->target == function) {
            return GeneratedOverlayFunctionHandle(existing->second);
        }
        existing->second->accepting = false;
        existing->second->revoked = true;
        existing->second->target = nullptr;
        existing->second->runtime = nullptr;
        poisoned_ = true;
        return {};
    }
    auto gate = std::make_shared<GeneratedOverlayDispatchGate>();
    gate->runtime = this;
    gate->admission = execution_admission_;
    gate->target = function;
    gate->expected_rdram = reinterpret_cast<std::uint8_t*>(
        guest_memory_.data());
    owner.dependency_dispatch_gates[opaque_reference_id] = gate;
    return GeneratedOverlayFunctionHandle(std::move(gate));
}

bool GeneratedOverlayRuntime::module_has_direct_reference_to(
    const ModuleRecord& module,
    const std::set<std::uint64_t>& unavailable,
    bool& function_reference,
    bool& custom_reference,
    bool& r32_reference) const noexcept {
    function_reference = std::any_of(
        module.image.dependencies.begin(), module.image.dependencies.end(),
        [&unavailable](const GeneratedOverlayDependency& dependency) {
            return unavailable.contains(dependency.target_module_id);
        });
    custom_reference = std::any_of(
        module.image.custom_bindings.begin(), module.image.custom_bindings.end(),
        [&unavailable](const GeneratedOverlayCustomBinding& binding) {
            return binding.target_module_id != 0U &&
                unavailable.contains(binding.target_module_id);
        });
    r32_reference = std::any_of(
        module.r32_provider_module_ids.begin(),
        module.r32_provider_module_ids.end(),
        [&unavailable](const std::uint64_t provider) {
            return unavailable.contains(provider);
        });
    return function_reference || custom_reference || r32_reference;
}

void GeneratedOverlayRuntime::recompute_execution_validity() noexcept {
    for (auto& [ignored_id, module] : modules_) {
        (void)ignored_id;
        if (module->active) {
            module->execution_valid = false;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [ignored_id, module] : modules_) {
            (void)ignored_id;
            if (!module->active || module->execution_valid ||
                !module->custom_bindings_valid || !module->r32_bindings_valid ||
                module->bindings.size() != module->image.dependencies.size()) {
                continue;
            }
            bool ready = true;
            for (const GeneratedOverlayDependency& dependency :
                 module->image.dependencies) {
                const auto binding = module->bindings.find(
                    dependency.opaque_reference_id);
                const auto target = modules_.find(dependency.target_module_id);
                if (binding == module->bindings.end() || target == modules_.end() ||
                    !target->second->active || !target->second->execution_valid ||
                    binding->second.target_module_id != dependency.target_module_id ||
                    binding->second.target_generation != target->second->generation ||
                    binding->second.function == nullptr) {
                    ready = false;
                    break;
                }
            }
            if (!ready) {
                continue;
            }
            for (const GeneratedOverlayCustomBinding& binding :
                 module->image.custom_bindings) {
                if (binding.target_module_id == 0U ||
                    binding.target_module_id == module->image.opaque_module_id) {
                    continue;
                }
                const auto target = modules_.find(binding.target_module_id);
                if (target == modules_.end() || !target->second->active ||
                    !target->second->execution_valid) {
                    ready = false;
                    break;
                }
            }
            if (!ready) {
                continue;
            }
            for (const std::uint64_t provider : module->r32_provider_module_ids) {
                const auto target = modules_.find(provider);
                if (target == modules_.end() || !target->second->active ||
                    !target->second->execution_valid) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                module->execution_valid = true;
                changed = true;
            }
        }
    }
}

void GeneratedOverlayRuntime::suspend_dispatch_gates(
    const std::vector<PreparedUnloadModule>& prepared) noexcept {
    for (const PreparedUnloadModule& item : prepared) {
        for (auto& [ignored_offset, gate] : item.module->dispatch_gates) {
            (void)ignored_offset;
            const std::lock_guard gate_lock(gate->mutex);
            gate->accepting = false;
        }
        for (auto& [ignored_reference, gate] :
             item.module->dependency_dispatch_gates) {
            (void)ignored_reference;
            const std::lock_guard gate_lock(gate->mutex);
            gate->accepting = false;
        }
    }
}

void GeneratedOverlayRuntime::resume_dispatch_gates(
    const std::vector<PreparedUnloadModule>& prepared) noexcept {
    for (const PreparedUnloadModule& item : prepared) {
        for (auto& [ignored_offset, gate] : item.module->dispatch_gates) {
            (void)ignored_offset;
            const std::lock_guard gate_lock(gate->mutex);
            if (!gate->revoked && gate->target != nullptr &&
                gate->runtime == this) {
                gate->accepting = true;
            }
        }
        for (auto& [ignored_reference, gate] :
             item.module->dependency_dispatch_gates) {
            (void)ignored_reference;
            const std::lock_guard gate_lock(gate->mutex);
            if (!gate->revoked && gate->target != nullptr &&
                gate->runtime == this) {
                gate->accepting = true;
            }
        }
    }
}

void GeneratedOverlayRuntime::revoke_dispatch_gates(
    const std::vector<PreparedUnloadModule>& prepared) noexcept {
    for (const PreparedUnloadModule& item : prepared) {
        for (auto& [ignored_offset, gate] : item.module->dispatch_gates) {
            (void)ignored_offset;
            const std::lock_guard gate_lock(gate->mutex);
            gate->accepting = false;
            gate->revoked = true;
            gate->target = nullptr;
            gate->runtime = nullptr;
        }
        item.module->dispatch_gates.clear();
        for (auto& [ignored_reference, gate] :
             item.module->dependency_dispatch_gates) {
            (void)ignored_reference;
            const std::lock_guard gate_lock(gate->mutex);
            gate->accepting = false;
            gate->revoked = true;
            gate->target = nullptr;
            gate->runtime = nullptr;
        }
        item.module->dependency_dispatch_gates.clear();
    }
}

GeneratedOverlayError GeneratedOverlayRuntime::unload(
    const GeneratedOverlayToken lifetime) noexcept {
    const std::lock_guard lock(mutex_);
    if (busy_) {
        return GeneratedOverlayError::busy;
    }
    if (poisoned_) {
        return GeneratedOverlayError::runtime_poisoned;
    }
    ModuleRecord* module = nullptr;
    const GeneratedOverlayError error = validate_token(lifetime, module, true);
    if (error != GeneratedOverlayError::none) {
        return error;
    }
    (void)module;
    return GeneratedOverlayError::cache_invalidation_rejected;
}

GeneratedOverlayError GeneratedOverlayRuntime::unload(
    const GeneratedOverlayToken lifetime,
    GeneratedInstructionCacheInvalidator& cache) noexcept {
    std::unique_lock lock(mutex_);
    if (busy_ || (active_invocation != nullptr &&
                  active_invocation->runtime == this)) {
        return GeneratedOverlayError::busy;
    }
    if (poisoned_) {
        return GeneratedOverlayError::runtime_poisoned;
    }
    ModuleRecord* module = nullptr;
    GeneratedOverlayError error = validate_token(lifetime, module, true);
    if (error != GeneratedOverlayError::none) {
        return error;
    }
    if (table_ == nullptr) {
        return GeneratedOverlayError::table_not_installed;
    }

    busy_ = true;
    std::vector<PreparedUnloadModule> prepared_modules;
    std::vector<GeneratedInstructionCacheRange> prepared_cache_ranges;
    bool admission_closed = false;
    const auto abort_prepared = [&cache, &prepared_cache_ranges]() noexcept {
        for (auto iterator = prepared_cache_ranges.rbegin();
             iterator != prepared_cache_ranges.rend(); ++iterator) {
            cache.abort(*iterator);
        }
        prepared_cache_ranges.clear();
    };
    try {
        const std::int32_t signed_base =
            static_cast<std::int32_t>(module->guest_base);
        if (section_addresses_[module->image.section_index] != signed_base) {
            poisoned_ = true;
            busy_ = false;
            return GeneratedOverlayError::runtime_poisoned;
        }

        std::set<std::uint64_t> unavailable;
        unavailable.insert(module->image.opaque_module_id);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [module_id, owner] : modules_) {
                if (!owner->active || unavailable.contains(module_id)) {
                    continue;
                }
                bool function_reference = false;
                bool custom_reference = false;
                bool r32_reference = false;
                if (module_has_direct_reference_to(
                        *owner, unavailable, function_reference,
                        custom_reference, r32_reference)) {
                    unavailable.insert(module_id);
                    changed = true;
                }
            }
        }

        prepared_modules.reserve(unavailable.size());
        const auto prepare_one = [&](ModuleRecord& current, const bool target) {
            PreparedUnloadModule prepared{};
            prepared.module = &current;
            prepared.target = target;
            if (!target) {
                (void)module_has_direct_reference_to(
                    current, unavailable, prepared.invalidate_function_bindings,
                    prepared.invalidate_custom_bindings,
                    prepared.invalidate_r32_bindings);
            }
            const std::size_t extent = mapped_size(current.extents);
            // Allocate every transaction buffer before quiescence. Its live
            // contents must be captured only after admitted calls finish, or
            // committing this snapshot could erase their final data/BSS
            // writes in an affected dependent.
            prepared.staged_bytes.resize(extent);
            prepared.cache_range = {
                current.image.opaque_module_id,
                current.guest_base,
                current.extents.text_size,
            };
            prepared_modules.emplace_back(std::move(prepared));
        };

        prepare_one(*module, true);
        for (const auto& [module_id, owner] : modules_) {
            if (owner.get() != module && unavailable.contains(module_id)) {
                prepare_one(*owner, false);
            }
        }
        prepared_cache_ranges.reserve(prepared_modules.size());
        for (const PreparedUnloadModule& prepared : prepared_modules) {
            prepared_cache_ranges.push_back(prepared.cache_range);
            if (!cache.prepare(prepared.cache_range)) {
                abort_prepared();
                busy_ = false;
                return GeneratedOverlayError::cache_invalidation_rejected;
            }
        }

        {
            const std::lock_guard admission_lock(execution_admission_->mutex);
            execution_admission_->accepting = false;
            admission_closed = true;
        }
        // Existing root calls retain their admission lease until they return.
        // Keep their gates usable while waiting: a generated body may perform
        // a nested get_function dispatch as part of completing that call.
        lock.unlock();
        {
            std::unique_lock admission_lock(execution_admission_->mutex);
            execution_admission_->idle.wait(admission_lock, [this]() {
                return execution_admission_->active_invocations == 0U;
            });
        }
        lock.lock();
        // Admission is still closed, so no new root can race gate suspension
        // after the last admitted call (including all nested dispatch) exits.
        suspend_dispatch_gates(prepared_modules);

        for (PreparedUnloadModule& prepared : prepared_modules) {
            if (prepared.target) {
                std::fill(
                    prepared.staged_bytes.begin(), prepared.staged_bytes.end(),
                    std::byte{0});
                continue;
            }
            std::copy_n(
                guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                    prepared.module->memory_offset),
                prepared.staged_bytes.size(), prepared.staged_bytes.begin());
            if (prepared.invalidate_custom_bindings) {
                for (const CustomOverlayRelocationRecord& record :
                     prepared.module->image.custom_relocations) {
                    std::copy_n(
                        prepared.module->image.initialized_bytes.begin() +
                            static_cast<std::ptrdiff_t>(record.site_offset),
                        4U,
                        prepared.staged_bytes.begin() +
                            static_cast<std::ptrdiff_t>(record.site_offset));
                }
            }
            if (prepared.invalidate_r32_bindings) {
                for (const GeneratedR32RelocationDescriptor& descriptor :
                     prepared.module->validated_generated_relocation_descriptors) {
                    std::copy_n(
                        prepared.module->image.initialized_bytes.begin() +
                            static_cast<std::ptrdiff_t>(descriptor.site_offset),
                        4U,
                        prepared.staged_bytes.begin() +
                            static_cast<std::ptrdiff_t>(descriptor.site_offset));
                }
            }
        }

        bool unloaded = table_->update_section_lifecycle(
            GeneratedSectionLifecycle::unload,
            module->image.section_index,
            0);
        unloaded = unloaded &&
            section_addresses_[module->image.section_index] == 0;
        if (!unloaded) {
            bool restored =
                section_addresses_[module->image.section_index] == signed_base;
            if (!restored) {
                restored = table_->update_section_lifecycle(
                    GeneratedSectionLifecycle::load,
                    module->image.section_index,
                    signed_base);
            }
            if (!restored ||
                section_addresses_[module->image.section_index] != signed_base) {
                poisoned_ = true;
                revoke_dispatch_gates(prepared_modules);
                abort_prepared();
                busy_ = false;
                return GeneratedOverlayError::runtime_poisoned;
            }
            resume_dispatch_gates(prepared_modules);
            {
                const std::lock_guard admission_lock(execution_admission_->mutex);
                execution_admission_->accepting = true;
                admission_closed = false;
            }
            abort_prepared();
            busy_ = false;
            return GeneratedOverlayError::section_unload_failed;
        }

        // All fallible work and execution quiescence precede this point.
        for (PreparedUnloadModule& prepared : prepared_modules) {
            std::copy(
                prepared.staged_bytes.begin(), prepared.staged_bytes.end(),
                guest_memory_.begin() + static_cast<std::ptrdiff_t>(
                    prepared.module->memory_offset));
            cache.commit(prepared.cache_range);
            prepared.module->execution_valid = false;
            if (prepared.target) {
                prepared.module->active = false;
                prepared.module->bindings.clear();
                continue;
            }
            if (prepared.invalidate_function_bindings) {
                for (auto iterator = prepared.module->bindings.begin();
                     iterator != prepared.module->bindings.end();) {
                    if (unavailable.contains(iterator->second.target_module_id)) {
                        iterator = prepared.module->bindings.erase(iterator);
                    }
                    else {
                        ++iterator;
                    }
                }
            }
            if (prepared.invalidate_custom_bindings) {
                prepared.module->custom_bindings_valid = false;
            }
            if (prepared.invalidate_r32_bindings) {
                prepared.module->r32_bindings_valid = false;
            }
        }
        revoke_dispatch_gates(prepared_modules);
        {
            const std::lock_guard admission_lock(execution_admission_->mutex);
            execution_admission_->accepting = true;
            admission_closed = false;
        }
        prepared_cache_ranges.clear();
        busy_ = false;
        return GeneratedOverlayError::none;
    }
    catch (const std::bad_alloc&) {
        if (!prepared_modules.empty()) {
            resume_dispatch_gates(prepared_modules);
        }
        if (admission_closed) {
            const std::lock_guard admission_lock(execution_admission_->mutex);
            execution_admission_->accepting = true;
        }
        abort_prepared();
        busy_ = false;
        return GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        const std::int32_t signed_base =
            static_cast<std::int32_t>(module->guest_base);
        bool restored =
            section_addresses_[module->image.section_index] == signed_base;
        if (!restored) {
            try {
                restored = table_->update_section_lifecycle(
                    GeneratedSectionLifecycle::load,
                    module->image.section_index,
                    signed_base);
            }
            catch (...) {
                restored = false;
            }
        }
        if (!restored ||
            section_addresses_[module->image.section_index] != signed_base) {
            poisoned_ = true;
            revoke_dispatch_gates(prepared_modules);
            error = GeneratedOverlayError::runtime_poisoned;
        }
        else {
            resume_dispatch_gates(prepared_modules);
            if (admission_closed) {
                const std::lock_guard admission_lock(execution_admission_->mutex);
                execution_admission_->accepting = true;
                admission_closed = false;
            }
            error = GeneratedOverlayError::callback_exception;
        }
        abort_prepared();
        busy_ = false;
        return error;
    }
}

GeneratedOverlayError GeneratedOverlayRuntime::validate_token(
    const GeneratedOverlayToken& token,
    ModuleRecord*& output,
    const bool require_active) noexcept {
    output = nullptr;
    if (!token.valid()) {
        return GeneratedOverlayError::stale_token;
    }
    if (token.runtime_identity_ != runtime_identity_) {
        return GeneratedOverlayError::cross_runtime_token;
    }
    const auto iterator = modules_.find(token.opaque_module_id_);
    if (iterator == modules_.end()) {
        return GeneratedOverlayError::module_not_found;
    }
    ModuleRecord* const module = iterator->second.get();
    if (module->generation != token.generation_) {
        return GeneratedOverlayError::stale_token;
    }
    if (require_active && !module->active) {
        return GeneratedOverlayError::module_inactive;
    }
    if (!require_active && module->active) {
        return GeneratedOverlayError::module_active;
    }
    output = module;
    return GeneratedOverlayError::none;
}

GeneratedOverlayFunctionResult GeneratedOverlayRuntime::lookup_function(
    const GeneratedOverlayToken lifetime,
    const std::uint32_t function_offset) noexcept {
    const std::lock_guard lock(mutex_);
    GeneratedOverlayFunctionResult result{};
    if (busy_) {
        result.error = GeneratedOverlayError::busy;
        return result;
    }
    if (poisoned_) {
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    ModuleRecord* module = nullptr;
    result.error = validate_token(lifetime, module, true);
    if (result.error != GeneratedOverlayError::none) {
        return result;
    }
    if (section_addresses_[module->image.section_index] !=
        static_cast<std::int32_t>(module->guest_base)) {
        poisoned_ = true;
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    if (!module->execution_valid) {
        result.error = GeneratedOverlayError::dependency_invalidated;
        return result;
    }
    busy_ = true;
    try {
        const GeneratedOverlayFunction function =
            lookup_active_offset(*module, function_offset);
        result.function = make_dispatch_handle(module, function_offset, function);
        result.error = poisoned_
            ? GeneratedOverlayError::runtime_poisoned
            : (!result.function.valid()
                ? GeneratedOverlayError::function_not_found
                : GeneratedOverlayError::none);
    }
    catch (const std::bad_alloc&) {
        result.error = GeneratedOverlayError::internal_failure;
        result.function = {};
    }
    catch (...) {
        result.error = GeneratedOverlayError::callback_exception;
        result.function = {};
    }
    busy_ = false;
    return result;
}

GeneratedOverlayFunctionResult
GeneratedOverlayRuntime::lookup_function_by_guest_address(
    const std::uint32_t guest_address) noexcept {
    const std::lock_guard lock(mutex_);
    GeneratedOverlayFunctionResult result{};
    const bool invocation_reentry = active_invocation != nullptr &&
        active_invocation->runtime == this;
    if (busy_ && !invocation_reentry) {
        result.error = GeneratedOverlayError::busy;
        return result;
    }
    if (poisoned_) {
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    if (table_ == nullptr) {
        result.error = GeneratedOverlayError::table_not_installed;
        return result;
    }
    if ((guest_address & 3U) != 0U) {
        result.error = GeneratedOverlayError::function_not_found;
        return result;
    }

    std::uint32_t selected_section = 0U;
    ModuleRecord* selected_module = nullptr;
    bool selected = false;
    for (const auto& [ignored_id, module] : modules_) {
        (void)ignored_id;
        if (!module->active) {
            continue;
        }
        const std::uint64_t text_end =
            static_cast<std::uint64_t>(module->guest_base) +
            module->extents.text_size;
        if (guest_address < module->guest_base ||
            static_cast<std::uint64_t>(guest_address) >= text_end) {
            continue;
        }
        if (section_addresses_[module->image.section_index] !=
            static_cast<std::int32_t>(module->guest_base)) {
            poisoned_ = true;
            result.error = GeneratedOverlayError::runtime_poisoned;
            return result;
        }
        if (!module->execution_valid) {
            result.error = GeneratedOverlayError::dependency_invalidated;
            return result;
        }
        selected_section = module->image.section_index;
        selected_module = module.get();
        selected = true;
        break;
    }

    if (!selected) {
        for (std::size_t index = 0U; index < section_extents_.size(); ++index) {
            if (section_extents_[index].is_overlay) {
                continue;
            }
            if (index >= initial_section_addresses_.size() ||
                section_addresses_[index] != initial_section_addresses_[index]) {
                poisoned_ = true;
                result.error = GeneratedOverlayError::runtime_poisoned;
                return result;
            }
            const std::uint32_t base =
                static_cast<std::uint32_t>(section_addresses_[index]);
            const std::uint64_t text_end =
                static_cast<std::uint64_t>(base) +
                section_extents_[index].text_size;
            if (guest_address >= base &&
                static_cast<std::uint64_t>(guest_address) < text_end) {
                selected_section = static_cast<std::uint32_t>(index);
                selected = true;
                break;
            }
        }
    }
    if (!selected) {
        result.error = GeneratedOverlayError::function_not_found;
        return result;
    }

    const bool owns_busy = !busy_;
    if (owns_busy) {
        busy_ = true;
    }
    try {
        GeneratedSectionExtents current_extents{};
        if (table_->section_count() != section_extents_.size() ||
            !table_->section_extents(selected_section, current_extents) ||
            !same_extents(current_extents, section_extents_[selected_section])) {
            result.error = GeneratedOverlayError::invalid_section_metadata;
        }
        else {
            const GeneratedOverlayFunction function = table_->lookup_function(
                static_cast<std::int32_t>(guest_address));
            result.function = make_dispatch_handle(
                selected_module,
                selected_module == nullptr
                    ? guest_address
                    : guest_address - selected_module->guest_base,
                function);
            result.error = poisoned_
                ? GeneratedOverlayError::runtime_poisoned
                : (!result.function.valid()
                    ? GeneratedOverlayError::function_not_found
                    : GeneratedOverlayError::none);
        }
    }
    catch (const std::bad_alloc&) {
        result.error = GeneratedOverlayError::internal_failure;
        result.function = {};
    }
    catch (...) {
        result.error = GeneratedOverlayError::callback_exception;
        result.function = {};
    }
    if (owns_busy) {
        busy_ = false;
    }
    return result;
}

GeneratedOverlayError
GeneratedOverlayRuntime::invoke_leased_function_by_guest_address(
    const std::uint32_t guest_address,
    std::uint8_t* const rdram,
    ::recomp_context* const context) noexcept {
    const std::lock_guard lock(mutex_);
    if (active_invocation == nullptr || active_invocation->runtime != this) {
        return GeneratedOverlayError::stale_token;
    }
    if (rdram == nullptr || context == nullptr ||
        rdram != reinterpret_cast<std::uint8_t*>(guest_memory_.data())) {
        return GeneratedOverlayError::invalid_mapping;
    }
    if (poisoned_) {
        return GeneratedOverlayError::runtime_poisoned;
    }
    if (table_ == nullptr) {
        return GeneratedOverlayError::table_not_installed;
    }
    if ((guest_address & 3U) != 0U) {
        return GeneratedOverlayError::function_not_found;
    }

    std::uint32_t selected_section = 0U;
    ModuleRecord* selected_module = nullptr;
    bool selected = false;
    for (const auto& [ignored_id, module] : modules_) {
        (void)ignored_id;
        const std::uint64_t text_end =
            static_cast<std::uint64_t>(module->guest_base) +
            module->extents.text_size;
        if (!module->active || guest_address < module->guest_base ||
            static_cast<std::uint64_t>(guest_address) >= text_end) {
            continue;
        }
        if (!module->execution_valid) {
            return GeneratedOverlayError::dependency_invalidated;
        }
        if (section_addresses_[module->image.section_index] !=
            static_cast<std::int32_t>(module->guest_base)) {
            poisoned_ = true;
            return GeneratedOverlayError::runtime_poisoned;
        }
        selected_section = module->image.section_index;
        selected_module = module.get();
        selected = true;
        break;
    }
    if (!selected) {
        for (std::size_t index = 0U; index < section_extents_.size(); ++index) {
            if (section_extents_[index].is_overlay) {
                continue;
            }
            if (index >= initial_section_addresses_.size() ||
                section_addresses_[index] != initial_section_addresses_[index]) {
                poisoned_ = true;
                return GeneratedOverlayError::runtime_poisoned;
            }
            const std::uint32_t base =
                static_cast<std::uint32_t>(section_addresses_[index]);
            const std::uint64_t text_end =
                static_cast<std::uint64_t>(base) +
                section_extents_[index].text_size;
            if (guest_address >= base &&
                static_cast<std::uint64_t>(guest_address) < text_end) {
                selected_section = static_cast<std::uint32_t>(index);
                selected = true;
                break;
            }
        }
    }
    if (!selected) {
        return GeneratedOverlayError::function_not_found;
    }
    try {
        GeneratedSectionExtents current_extents{};
        if (table_->section_count() != section_extents_.size() ||
            !table_->section_extents(selected_section, current_extents) ||
            !same_extents(current_extents, section_extents_[selected_section])) {
            return GeneratedOverlayError::invalid_section_metadata;
        }
        const GeneratedOverlayFunction function = table_->lookup_function(
            static_cast<std::int32_t>(guest_address));
        const GeneratedOverlayFunctionHandle handle = make_dispatch_handle(
            selected_module,
            selected_module == nullptr
                ? guest_address
                : guest_address - selected_module->guest_base,
            function);
        if (poisoned_) {
            return GeneratedOverlayError::runtime_poisoned;
        }
        if (!handle.valid()) {
            return GeneratedOverlayError::function_not_found;
        }
        // The caller already owns the outer execution-admission lease. Calling
        // through handle.invoke() here would incorrectly request a new root
        // admission while unload is quiescing that outer call. The raw target
        // remains local and is invoked synchronously after the stable-identity
        // gate check above.
        function(rdram, context);
        return GeneratedOverlayError::none;
    }
    catch (const std::bad_alloc&) {
        return GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        return GeneratedOverlayError::callback_exception;
    }
}

GeneratedOverlayFunctionResult GeneratedOverlayRuntime::resolve_dependency(
    const GeneratedOverlayToken owner_lifetime,
    const std::uint64_t opaque_reference_id) noexcept {
    const std::lock_guard lock(mutex_);
    GeneratedOverlayFunctionResult result{};
    if (busy_) {
        result.error = GeneratedOverlayError::busy;
        return result;
    }
    if (poisoned_) {
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    ModuleRecord* owner = nullptr;
    result.error = validate_token(owner_lifetime, owner, true);
    if (result.error != GeneratedOverlayError::none) {
        return result;
    }
    if (!owner->execution_valid) {
        result.error = GeneratedOverlayError::dependency_invalidated;
        return result;
    }
    const auto binding = owner->bindings.find(opaque_reference_id);
    if (binding == owner->bindings.end()) {
        const bool declared = std::any_of(
            owner->image.dependencies.begin(),
            owner->image.dependencies.end(),
            [opaque_reference_id](const GeneratedOverlayDependency& dependency) {
                return dependency.opaque_reference_id == opaque_reference_id;
            });
        result.error = declared
            ? GeneratedOverlayError::dependency_invalidated
            : GeneratedOverlayError::invalid_dependency;
        return result;
    }
    const auto target = modules_.find(binding->second.target_module_id);
    if (target == modules_.end() || !target->second->active ||
        target->second->generation != binding->second.target_generation ||
        binding->second.function == nullptr) {
        result.error = GeneratedOverlayError::dependency_invalidated;
        return result;
    }
    if (section_addresses_[target->second->image.section_index] !=
        static_cast<std::int32_t>(target->second->guest_base)) {
        poisoned_ = true;
        result.error = GeneratedOverlayError::runtime_poisoned;
        return result;
    }
    const auto dependency = std::find_if(
        owner->image.dependencies.begin(), owner->image.dependencies.end(),
        [opaque_reference_id](const GeneratedOverlayDependency& candidate) {
            return candidate.opaque_reference_id == opaque_reference_id;
        });
    if (dependency == owner->image.dependencies.end()) {
        result.error = GeneratedOverlayError::invalid_dependency;
        return result;
    }
    try {
        result.function = make_dependency_dispatch_handle(
            *owner, opaque_reference_id, binding->second.function);
        if (poisoned_) {
            result.error = GeneratedOverlayError::runtime_poisoned;
        }
        else if (!result.function.valid()) {
            result.error = GeneratedOverlayError::function_not_found;
        }
    }
    catch (const std::bad_alloc&) {
        result.function = {};
        result.error = GeneratedOverlayError::internal_failure;
    }
    catch (...) {
        result.function = {};
        result.error = GeneratedOverlayError::internal_failure;
    }
    return result;
}

const char* generated_overlay_error_message(
    const GeneratedOverlayError error) noexcept {
    switch (error) {
    case GeneratedOverlayError::none:
        return "no error";
    case GeneratedOverlayError::identity_exhausted:
        return "overlay runtime identity or generation is exhausted";
    case GeneratedOverlayError::invalid_table:
        return "generated overlay table is invalid";
    case GeneratedOverlayError::table_already_installed:
        return "a generated overlay table is already installed";
    case GeneratedOverlayError::table_not_installed:
        return "no generated overlay table is installed";
    case GeneratedOverlayError::invalid_section_count:
        return "generated section count is invalid or unstable";
    case GeneratedOverlayError::section_initialization_failed:
        return "generated section initialization failed";
    case GeneratedOverlayError::invalid_section_metadata:
        return "generated section metadata is invalid";
    case GeneratedOverlayError::initial_overlay_published:
        return "generated initialization published an overlay section";
    case GeneratedOverlayError::invalid_image:
        return "overlay image does not match generated section metadata";
    case GeneratedOverlayError::invalid_mapping:
        return "overlay mapping is outside cached guest memory";
    case GeneratedOverlayError::overlapping_mapping:
        return "overlay mapping overlaps an active module";
    case GeneratedOverlayError::duplicate_module:
        return "overlay module or generated section is already registered";
    case GeneratedOverlayError::reload_required:
        return "a prior overlay lifetime must use same-base reload";
    case GeneratedOverlayError::module_not_found:
        return "overlay module is not registered";
    case GeneratedOverlayError::module_active:
        return "overlay module is already active";
    case GeneratedOverlayError::module_inactive:
        return "overlay module is inactive";
    case GeneratedOverlayError::invalid_dependency:
        return "overlay dependency identity is invalid or undeclared";
    case GeneratedOverlayError::invalid_custom_relocation:
        return "custom relocation metadata is invalid";
    case GeneratedOverlayError::invalid_custom_binding:
        return "custom relocation binding is invalid";
    case GeneratedOverlayError::custom_binding_unresolved:
        return "custom relocation binding is unresolved";
    case GeneratedOverlayError::custom_relocation_failed:
        return "custom relocation application failed";
    case GeneratedOverlayError::custom_relocation_overlap:
        return "custom and generated relocation sites overlap";
    case GeneratedOverlayError::relocation_site_mismatch:
        return "generated relocation sites changed after custom validation";
    case GeneratedOverlayError::relocation_descriptor_mismatch:
        return "generated relocation descriptors are incomplete or changed";
    case GeneratedOverlayError::dependency_unresolved:
        return "overlay dependency target is not active";
    case GeneratedOverlayError::dependency_invalidated:
        return "overlay dependency binding is invalidated";
    case GeneratedOverlayError::invalid_relocation_count:
        return "generated relocation count exceeds the bounded section";
    case GeneratedOverlayError::section_load_failed:
        return "generated section load lifecycle failed";
    case GeneratedOverlayError::section_unload_failed:
        return "generated section unload lifecycle failed";
    case GeneratedOverlayError::relocation_failed:
        return "checked generated relocation application failed";
    case GeneratedOverlayError::relocation_count_mismatch:
        return "generated relocation count changed or was not fully applied";
    case GeneratedOverlayError::relocation_wrote_outside_section:
        return "generated relocation modified memory outside its section";
    case GeneratedOverlayError::bss_not_zero:
        return "generated relocation modified cleared BSS";
    case GeneratedOverlayError::cache_invalidation_rejected:
        return "instruction-cache invalidation preparation was rejected";
    case GeneratedOverlayError::function_not_found:
        return "generated function lookup failed";
    case GeneratedOverlayError::stale_token:
        return "overlay lifetime token is stale or invalid";
    case GeneratedOverlayError::cross_runtime_token:
        return "overlay lifetime token belongs to another runtime";
    case GeneratedOverlayError::busy:
        return "overlay runtime rejects callback re-entry";
    case GeneratedOverlayError::runtime_poisoned:
        return "overlay runtime cannot restore generated lifecycle state";
    case GeneratedOverlayError::callback_exception:
        return "generated table or cache callback raised an exception";
    case GeneratedOverlayError::internal_failure:
        return "overlay runtime could not allocate transactional state";
    }
    return "unknown generated overlay runtime error";
}

void* swap_overlay_invocation_context(void* const context) noexcept {
    void* const previous = active_invocation;
    active_invocation = static_cast<InvocationLeaseContext*>(context);
    return previous;
}

}  // namespace jfg
