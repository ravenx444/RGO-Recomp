# RGO frame interpolation (docs/rgo/framerate.md). Two changes to RT64's
# workload queue, both inert while no target refresh rate is set (Frame rate:
# Original), so the original renderer path is unchanged:
#  1. Auxiliary workloads (off-screen targets such as JFG's 64x64 shadow
#     masks) are rendered but do not count as a game frame: they are not
#     matched, do not replace the previous frame, and do not touch the
#     interpolation tick and frame counters or the present candidates. RT64
#     otherwise matches each task with the task before it.
#  2. "Unlocked" (refreshRateTarget kUnlockedRefreshTarget) is not capped at
#     the display's refresh rate; RT64's own time budget drops frames it
#     cannot render in time.
set(jfg_interp_source "${jfg_rt64_root}/src/hle/rt64_workload_queue.cpp")
# Validate the canonical text so CRLF checkouts are accepted.
file(READ "${jfg_interp_source}" jfg_interp_contents)
string(REPLACE "\r\n" "\n" jfg_interp_contents "${jfg_interp_contents}")
string(SHA256 jfg_interp_hash "${jfg_interp_contents}")
if(NOT jfg_interp_hash STREQUAL "6e53030e8899c42347b51e67879fcb9679ad487ac067c192612c432d85bca4a4")
    message(FATAL_ERROR "Pinned RT64 workload queue differs from the audited frame-interpolation input")
endif()

function(jfg_interp_patch old new)
    string(FIND "${jfg_interp_contents}" "${old}" first)
    string(FIND "${jfg_interp_contents}" "${old}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "Pinned RT64 frame-interpolation patch site is missing or ambiguous: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" patched "${jfg_interp_contents}")
    set(jfg_interp_contents "${patched}" PARENT_SCOPE)
endfunction()

jfg_interp_patch(
[=[            if ((ext.sharedResources->swapChainRate > 0) && (workloadConfig.targetRate > ext.sharedResources->swapChainRate)) {]=]
[=[            if ((ext.sharedResources->swapChainRate > 0) && (workloadConfig.targetRate > ext.sharedResources->swapChainRate) &&
                (ext.sharedResources->userConfig.refreshRateTarget < jfg::kUnlockedRefreshTarget)) {]=])

jfg_interp_patch(
[=[        bool frameReduction = false;
        while (threadsRunning) {]=]
[=[        bool frameReduction = false;
        GameFrame jfgAuxiliaryFrame;
        GameFrame jfgEmptyFrame;
        jfg::MainFrameHistory jfgHistory;
        while (threadsRunning) {]=])

jfg_interp_patch(
[=[                if (!workload.paused || (!gameFrames[curFrameIndex].workloads.empty() && (gameFrames[curFrameIndex].workloads[0] != (uint32_t)processCursor))) {]=]
[=[                // RGO: a workload without a screen-sized presentable color image is
                // auxiliary work between two game frames.
                bool jfgAuxiliary = false;
                if (workloadConfig.targetRate > 0) {
                    jfgAuxiliary = true;
                    for (uint32_t f = 0; f < workload.fbPairCount; f++) {
                        const FramebufferPair &fbPair = workload.fbPairs[f];
                        if (!fbPair.fastPaths.clearDepthOnly && !fbPair.drawColorRect.isEmpty() && fbPair.earlyPresentCandidate() &&
                            jfg::is_main_frame_target(fbPair.colorImage.width, uint32_t(fbPair.drawColorRect.bottom(true)),
                                workload.viFbSize[0], workload.viFbSize[1])) {
                            jfgAuxiliary = false;
                            break;
                        }
                    }
                }

                if (!jfgAuxiliary && (!workload.paused || (!gameFrames[curFrameIndex].workloads.empty() && (gameFrames[curFrameIndex].workloads[0] != (uint32_t)processCursor)))) {]=])

jfg_interp_patch(
[=[                GameFrame &curFrame = gameFrames[curFrameIndex];
                const GameFrame &prevFrame = gameFrames[prevFrameIndex];]=]
[=[                GameFrame &curFrame = jfgAuxiliary ? jfgAuxiliaryFrame : gameFrames[curFrameIndex];
                const uint32_t jfgExpectedPrevious = gameFrames[prevFrameIndex].workloads.empty()
                    ? uint32_t(-1) : gameFrames[prevFrameIndex].workloads.front();
                const bool jfgPreviousFrameValid = jfgHistory.observe(jfgAuxiliary,
                    uint32_t(processCursor), jfgExpectedPrevious);
                const GameFrame &prevFrame = jfgPreviousFrameValid
                    ? gameFrames[prevFrameIndex] : jfgEmptyFrame;]=])

jfg_interp_patch(
[=[                    colorVector.clear();
                    colorSet.clear();
                    for (int32_t f = workload.fbPairCount - 1; f >= 0; f--) {]=]
[=[                    if (!jfgAuxiliary) {
                        colorVector.clear();
                        colorSet.clear();
                    }
                    for (int32_t f = jfgAuxiliary ? -1 : int32_t(workload.fbPairCount) - 1; f >= 0; f--) {]=])

# A depth clear may use the screen address without having a color target.
# It must not become the image resolved for an interpolated MSAA frame.
jfg_interp_patch(
[=[                        bool interpolationCandidate = fbPair.earlyPresentCandidate();]=]
[=[                        bool interpolationCandidate = !fbPair.fastPaths.clearDepthOnly && fbPair.earlyPresentCandidate();]=])

jfg_interp_patch(
[=[                    if (usingMSAA && (overrideTarget != nullptr) && ((uint32_t)overrideTargetFbPairIndex == f)) {]=]
[=[                    if (usingMSAA && (overrideTarget != nullptr) && (colorTarget != nullptr) &&
                        ((uint32_t)overrideTargetFbPairIndex == f)) {]=])

jfg_interp_patch(
[=[                const bool requiresFrameMatching = (workloadConfig.targetRate > 0) || workloadConfig.raytracingEnabled;]=]
[=[                const bool requiresFrameMatching = !jfgAuxiliary && ((workloadConfig.targetRate > 0) || workloadConfig.raytracingEnabled);]=])

jfg_interp_patch(
[=[                const bool useDifferentCounters = (lastPresentId != workload.presentId);]=]
[=[                const bool useDifferentCounters = !jfgAuxiliary && (lastPresentId != workload.presentId);]=])

jfg_interp_patch(
[=[                curFrameCounters.skipped = false;
                curFrameCounters.presented = 0;
                curFrameCounters.available = 0;
                curFrameCounters.count = displayFrames;]=]
[=[                if (!jfgAuxiliary) {
                    curFrameCounters.skipped = false;
                    curFrameCounters.presented = 0;
                    curFrameCounters.available = 0;
                    curFrameCounters.count = displayFrames;
                }]=])

set(jfg_interp_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64/hle/rt64_workload_queue.cpp")
jfg_write_if_changed("${jfg_interp_generated}"
    "#include \"jfg/renderer/rt64_frame_grouping.hpp\"\n${jfg_interp_contents}")
get_target_property(jfg_interp_sources rt64 SOURCES)
set(jfg_interp_patched_sources)
set(jfg_interp_replaced FALSE)
foreach(jfg_interp_unit IN LISTS jfg_interp_sources)
    if(jfg_interp_unit MATCHES "(^|[/\\\\])rt64_workload_queue\\.cpp$")
        list(APPEND jfg_interp_patched_sources "${jfg_interp_generated}")
        set(jfg_interp_replaced TRUE)
    else()
        list(APPEND jfg_interp_patched_sources "${jfg_interp_unit}")
    endif()
endforeach()
if(NOT jfg_interp_replaced)
    message(FATAL_ERROR "Pinned RT64 target is missing its workload queue")
endif()
set_property(TARGET rt64 PROPERTY SOURCES "${jfg_interp_patched_sources}")
# Quoted includes of the original file resolve from its directory.
target_include_directories(rt64 PRIVATE "${jfg_rt64_root}/src/hle" "${PROJECT_SOURCE_DIR}/include")
