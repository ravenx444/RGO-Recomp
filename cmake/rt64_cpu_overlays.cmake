# Apply the VI framebuffer's CPU edits to every interpolated presentation.
# JFG draws weapon sight lines and digits after GPU completion. RT64's base
# target receives these changes, but its separate interpolated targets do not.
set(jfg_cpu_source "${jfg_rt64_root}/src/hle/rt64_present_queue.cpp")
file(READ "${jfg_cpu_source}" jfg_cpu_contents)
string(REPLACE "\r\n" "\n" jfg_cpu_contents "${jfg_cpu_contents}")
string(SHA256 jfg_cpu_hash "${jfg_cpu_contents}")
if(NOT jfg_cpu_hash STREQUAL "023dc3f4bc74bf6863e37ad1781fb32eaa543a7713bd8b9a3757b2262a1a1e52")
    message(FATAL_ERROR "Pinned RT64 present queue differs from the audited CPU overlay input")
endif()

function(jfg_cpu_patch old new)
    string(FIND "${jfg_cpu_contents}" "${old}" first)
    string(FIND "${jfg_cpu_contents}" "${old}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "Pinned RT64 CPU overlay selection site is missing or ambiguous")
    endif()
    string(REPLACE "${old}" "${new}" patched "${jfg_cpu_contents}")
    set(jfg_cpu_contents "${patched}" PARENT_SCOPE)
endfunction()

# Track the actual target selection. RT64 may reduce framesToPresent while
# waiting for a skipped workload; that does not turn its extra target into the
# normal target, even when only one presentation remains.
jfg_cpu_patch(
[=[            uint32_t frameCountersNextPresented = 0;]=]
[=[            uint32_t frameCountersNextPresented = 0;
            bool jfgExtraTarget = false;]=])
jfg_cpu_patch(
[=[                    colorTarget = ext.sharedResources->interpolatedColorTargets[targetIndex].get();]=]
[=[                    colorTarget = ext.sharedResources->interpolatedColorTargets[targetIndex].get();
                    jfgExtraTarget = true;]=])

set(jfg_cpu_old [=[            uint32_t swapChainIndex = 0;]=])
set(jfg_cpu_new [=[            // RGO: CPU drawing belongs to the VI image, including every extra
            // presentation of it. The normal target already received these
            // edits above; MSAA and later frames use separate resolved targets.
            if (jfgExtraTarget && colorTarget != nullptr && !present.fbOperations.empty()) {
                const std::scoped_lock lock(screenFbChangePoolMutex);
                for (const auto &op : present.fbOperations) {
                    if (op.type != FramebufferOperation::Type::WriteChanges ||
                        op.writeChanges.address != present.screenVI.fbAddress())
                        continue;
                    const Framebuffer *fb = fbManager.find(op.writeChanges.address);
                    const FramebufferChange *change = screenFbChangePool.get(op.writeChanges.id);
                    if (fb != nullptr && change != nullptr &&
                        fb->lastWriteType == Framebuffer::Type::Color &&
                        fb->width == present.screenVI.fbSize().x && fb->siz == present.screenVI.fbSiz()) {
                        RenderWorkerExecution workerExecution(ext.presentGraphicsWorker);
                        colorTarget->copyFromChanges(ext.presentGraphicsWorker, *change,
                            fb->width, fb->height, 0, ext.shaderLibrary);
                    }
                }
            }

            uint32_t swapChainIndex = 0;]=])
string(FIND "${jfg_cpu_contents}" "${jfg_cpu_old}" jfg_cpu_first)
string(FIND "${jfg_cpu_contents}" "${jfg_cpu_old}" jfg_cpu_last REVERSE)
if(jfg_cpu_first EQUAL -1 OR NOT jfg_cpu_first EQUAL jfg_cpu_last)
    message(FATAL_ERROR "Pinned RT64 CPU overlay patch site is missing or ambiguous")
endif()
string(REPLACE "${jfg_cpu_old}" "${jfg_cpu_new}" jfg_cpu_contents "${jfg_cpu_contents}")
set(jfg_cpu_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64/hle/rt64_present_queue.cpp")
jfg_write_if_changed("${jfg_cpu_generated}" "${jfg_cpu_contents}")
get_target_property(jfg_cpu_sources rt64 SOURCES)
set(jfg_cpu_patched_sources)
set(jfg_cpu_replaced FALSE)
foreach(jfg_cpu_unit IN LISTS jfg_cpu_sources)
    if(jfg_cpu_unit MATCHES "(^|[/\\\\])rt64_present_queue\\.cpp$")
        list(APPEND jfg_cpu_patched_sources "${jfg_cpu_generated}")
        set(jfg_cpu_replaced TRUE)
    else()
        list(APPEND jfg_cpu_patched_sources "${jfg_cpu_unit}")
    endif()
endforeach()
if(NOT jfg_cpu_replaced)
    message(FATAL_ERROR "Pinned RT64 target is missing its present queue")
endif()
set_property(TARGET rt64 PROPERTY SOURCES "${jfg_cpu_patched_sources}")
target_include_directories(rt64 PRIVATE "${jfg_rt64_root}/src/hle")
