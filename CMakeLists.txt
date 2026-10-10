cmake_minimum_required(VERSION 3.20)

# MSBuild's tracking paths repeat target names and remain subject to MAX_PATH.
# CMAKE_OBJECT_PATH_MAX only shortens object names, not these .tlog paths.
if(CMAKE_HOST_WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
    string(LENGTH "${CMAKE_BINARY_DIR}" jfg_binary_root_length)
    if(jfg_binary_root_length GREATER 80)
        message(FATAL_ERROR "Windows build output path is too long for MSBuild. Use python scripts/build_windows.py --test (or --build-root D:/JFG-builds). Existing files are preserved.")
    endif()
endif()


project(
    jfg_recomp
    VERSION 1.0.0
    DESCRIPTION "ROM-free host scaffolding for a Jet Force Gemini native recompilation"
    LANGUAGES C CXX
)

option(JFG_BUILD_TESTS "Build the ROM-free unit tests" ON)
option(JFG_BUILD_SYNTHETIC_GENERATED_TESTS "Build ROM-free generated-code link tests" ON)
option(JFG_WARNINGS_AS_ERRORS "Treat handwritten-code warnings as errors" ON)
option(JFG_ENABLE_GENERATED_CODE "Build locally generated CPU code from a private root" OFF)
option(JFG_BUILD_G2_PRODUCERS "Build private G2 evidence producers" OFF)
option(JFG_BUILD_PHASE5_CAPTURE
    "Build the private Phase 5 boundary-capture producer" OFF)
option(JFG_BUILD_PHASE6_NATIVE_BOOT "Build the private Phase 6 native M2 runner" OFF)
option(JFG_ENABLE_RT64 "Build the pinned RT64 renderer shell" OFF)
option(JFG_BUILD_PHASE7_RT64_RUNNER
    "Build the private-input Phase 7 RT64 captured-task runner" OFF)
option(JFG_BUILD_PHASE8_LIVE
    "Build the private Phase 8 live generated-code runtime" OFF)
# ExecutorShutdown unwinds through generated extern "C" guest frames. MSVC's
# default /EHsc lets the compiler assume extern "C" callees never throw, which
# makes that unwind undefined for every host of the executor. /EHs keeps
# synchronous exceptions and drops that assumption. Replacing the default flag
# avoids the D9025 override warning a per-target option would add.
if(MSVC)
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

option(JFG_ENABLE_ADDRESS_SANITIZER
    "Instrument the complete selected build with AddressSanitizer" OFF)
set(JFG_GENERATED_ROOT "" CACHE PATH "Explicit private root containing generated CPU code")
set(JFG_PHASE6_LIBULTRA_IDENTIFICATION "" CACHE FILEPATH "Explicit private libultra identification JSON")
set(JFG_PHASE6_ROM "" CACHE FILEPATH "Explicit private supported ROM for the Phase 6 M2 test")
set(JFG_PHASE6_EMULATOR_CHECKPOINT "" CACHE FILEPATH "Explicit private emulator checkpoint for the Phase 6 M2 test")
set(JFG_PHASE6_BIZHAWK_EXECUTABLE "" CACHE FILEPATH "Explicit pinned BizHawk executable used for the Phase 6 oracle")
set(JFG_PHASE6_EMULATOR_PROBE "" CACHE FILEPATH "Explicit private Lua probe used for the Phase 6 oracle")
set(JFG_RT64_ROOT "" CACHE PATH "Explicit source root for the pinned RT64 checkout")
set(JFG_PHASE8_PRIVATE_AUDIO_ADAPTER_ROOT "" CACHE PATH
    "Ignored root containing the reviewed generated Phase 8 audio adapter")
set(JFG_PHASE8_PRIVATE_AUDIO_RUNTIME_INCLUDE "" CACHE PATH
    "Ignored include root required by the generated Phase 8 audio adapter")

if(JFG_ENABLE_ADDRESS_SANITIZER)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/DEBUG /INCREMENTAL:NO)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=address -fno-sanitize-recover=all)
        add_link_options(-fsanitize=address)
    else()
        message(FATAL_ERROR "JFG_ENABLE_ADDRESS_SANITIZER has no supported toolchain flags")
    endif()
endif()
set(JFG_G2_PRIVATE_GRAPHICS_ORACLE_ROOT "" CACHE PATH
    "Ignored/external root implementing the independent G2 graphics oracle ABI")
set(JFG_G2_PRIVATE_AUDIO_ADAPTER_ROOT "" CACHE PATH
    "Ignored/external root implementing the G2 private audio adapter ABI")
set(JFG_G2_PRIVATE_AUDIO_RUNTIME_INCLUDE "" CACHE PATH
    "Ignored/external include root for the private generated RSP adapter")
set(JFG_G2_PRIVATE_AUDIO_ORACLE_ROOT "" CACHE PATH
    "Ignored/external root implementing the independent G2 audio oracle ABI")

if(JFG_BUILD_G2_PRODUCERS AND NOT JFG_ENABLE_GENERATED_CODE)
    message(FATAL_ERROR "JFG_BUILD_G2_PRODUCERS requires JFG_ENABLE_GENERATED_CODE")
endif()

if(JFG_BUILD_PHASE5_CAPTURE AND NOT JFG_ENABLE_GENERATED_CODE)
    message(FATAL_ERROR "JFG_BUILD_PHASE5_CAPTURE requires JFG_ENABLE_GENERATED_CODE")
endif()
if(JFG_BUILD_PHASE6_NATIVE_BOOT AND NOT JFG_ENABLE_GENERATED_CODE)
    message(FATAL_ERROR "JFG_BUILD_PHASE6_NATIVE_BOOT requires JFG_ENABLE_GENERATED_CODE")
endif()
if(JFG_BUILD_PHASE8_LIVE AND
   (NOT JFG_BUILD_PHASE6_NATIVE_BOOT OR NOT JFG_ENABLE_RT64))
    message(FATAL_ERROR
        "JFG_BUILD_PHASE8_LIVE requires JFG_BUILD_PHASE6_NATIVE_BOOT and JFG_ENABLE_RT64")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(GeneratedCode)
include(CTest)

if(WIN32)
    enable_language(RC)
    set(jfg_application_icon "${CMAKE_CURRENT_BINARY_DIR}/jfg-application-icon.rc")
    file(WRITE "${jfg_application_icon}"
        "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/launcher/ui/assets/app.ico\"\n")
    add_executable(jfg-frontend WIN32 src/app/frontend_win32.cpp "${jfg_application_icon}")
    target_compile_features(jfg-frontend PRIVATE cxx_std_20)
    target_link_libraries(jfg-frontend PRIVATE user32 gdi32 shell32 ole32 comdlg32)
    target_compile_definitions(jfg-frontend PRIVATE UNICODE _UNICODE)
    target_include_directories(jfg-frontend PRIVATE include)
    jfg_set_project_warnings(jfg-frontend)
    add_executable(jfg-support-capture src/app/support_capture.cpp)
    target_include_directories(jfg-support-capture PRIVATE include)
    target_compile_features(jfg-support-capture PRIVATE cxx_std_20)
    target_link_libraries(jfg-support-capture PRIVATE dbghelp psapi)
    jfg_set_project_warnings(jfg-support-capture)
    if(BUILD_TESTING AND JFG_BUILD_TESTS)
        add_executable(jfg-support-fixture tests/support_snapshot_fixture.cpp)
        target_include_directories(jfg-support-fixture PRIVATE include)
        target_compile_features(jfg-support-fixture PRIVATE cxx_std_20)
        target_link_libraries(jfg-support-fixture PRIVATE psapi)
        jfg_set_project_warnings(jfg-support-fixture)
        if(MSVC)
            target_compile_options(jfg-support-fixture PRIVATE /Zi)
            target_link_options(jfg-support-fixture PRIVATE /DEBUG /INCREMENTAL:NO)
        endif()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME jfg.support_capture COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_support_capture.py"
            --fixture "$<TARGET_FILE:jfg-support-fixture>" --capture "$<TARGET_FILE:jfg-support-capture>")
        set_tests_properties(jfg.support_capture PROPERTIES TIMEOUT 45)
    endif()
endif()
option(JFG_SUPPORT_SYMBOLS "Keep matching local symbols for support stack traces" OFF)
if(MSVC AND JFG_SUPPORT_SYMBOLS)
    add_compile_options(/Zi)
    add_link_options(/DEBUG /INCREMENTAL:NO)
endif()

add_library(jfg_runtime STATIC
    src/platform/host.cpp
    src/runtime/audio_task_bridge.cpp
    src/runtime/bounded_custom_graphics.cpp
    src/runtime/cic_nus_6105.cpp
    src/runtime/controller_pak.cpp
    src/runtime/custom_overlay_relocator.cpp
    src/runtime/generated_overlay_runtime.cpp
    src/runtime/graphics_task_bridge.cpp
    src/runtime/input_stick.cpp
    src/runtime/input_replay.cpp
    src/runtime/log.cpp
    src/runtime/options.cpp
    src/runtime/renderer_callbacks.cpp
    src/runtime/rom_validator.cpp
    src/runtime/rt64_overlay_address.cpp
    src/runtime/save_device_runtime.cpp
)

target_include_directories(jfg_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

target_compile_features(jfg_runtime PUBLIC cxx_std_20)
jfg_set_project_warnings(jfg_runtime)

if(JFG_ENABLE_RT64)
    if(JFG_RT64_ROOT STREQUAL "")
        message(FATAL_ERROR "JFG_ENABLE_RT64 requires an explicit JFG_RT64_ROOT")
    endif()
    file(REAL_PATH "${JFG_RT64_ROOT}" jfg_rt64_root)
    if(NOT EXISTS "${jfg_rt64_root}/CMakeLists.txt" OR
       NOT EXISTS "${jfg_rt64_root}/LICENSE")
        message(FATAL_ERROR "JFG_RT64_ROOT is not an RT64 source checkout")
    endif()

    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${jfg_rt64_root}" rev-parse HEAD
        RESULT_VARIABLE jfg_rt64_git_result
        OUTPUT_VARIABLE jfg_rt64_revision
        ERROR_VARIABLE jfg_rt64_git_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(jfg_rt64_required_revision
        "5473732a822a4423b5696e7cb18fecc425a59875")
    if(NOT jfg_rt64_git_result EQUAL 0)
        message(FATAL_ERROR
            "Unable to inspect JFG_RT64_ROOT revision: ${jfg_rt64_git_error}")
    endif()
    if(NOT jfg_rt64_revision STREQUAL jfg_rt64_required_revision)
        message(FATAL_ERROR
            "JFG_RT64_ROOT must be pinned to ${jfg_rt64_required_revision}; "
            "found ${jfg_rt64_revision}")
    endif()

    set(RT64_STATIC ON CACHE BOOL "Build RT64 as a static library" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL
        "Do not register dependency-owned Zstd tests" FORCE)
    # Extend shader dependencies in RT64's directory without modifying its checkout.
    set(jfg_saved_rt64_project_include "${CMAKE_PROJECT_rt64_INCLUDE}")
    list(APPEND CMAKE_PROJECT_rt64_INCLUDE
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rt64_shader_dependencies.cmake")
    add_subdirectory("${jfg_rt64_root}"
        "${CMAKE_CURRENT_BINARY_DIR}/_deps/rt64-build" EXCLUDE_FROM_ALL)
    set(CMAKE_PROJECT_rt64_INCLUDE "${jfg_saved_rt64_project_include}")
    unset(jfg_saved_rt64_project_include)

    # The pinned transform uploader treats the mere existence of a previous
    # game frame as proof that every current workload was matched to it. On a
    # scene transition (and on this single-task acceptance harness), an
    # unmatched workload has empty interpolation arrays; uploading those as
    # world matrices produces GPU-only garbage geometry even though RT64's
    # CPU-transformed vertices are valid. Generate a hash-guarded compatibility
    # unit in the build tree and use current matrices until a workload is
    # actually mapped to a previous one.
    set(jfg_rt64_transform_processor
        "${jfg_rt64_root}/src/render/rt64_transform_processor.cpp")
    file(SHA256 "${jfg_rt64_transform_processor}"
        jfg_rt64_transform_processor_sha256)
    # Accept the same pinned file with Git's LF or CRLF checkout convention.
    # These are exact hashes; altered source still fails closed.
    if(NOT jfg_rt64_transform_processor_sha256 STREQUAL
       "68d023aa9e5376737aa02c3b310063b773edccd6092fc2e3af66e9e8d74da973" AND
       NOT jfg_rt64_transform_processor_sha256 STREQUAL
       "ffd56ba45e76a82c18397c4d777b228a2b0963ac28c6ba9c618b417437bebcf0")
        message(FATAL_ERROR
            "Pinned RT64 transform processor does not match the audited "
            "unmatched-workload compatibility patch input")
    endif()
    file(READ "${jfg_rt64_transform_processor}"
        jfg_rt64_transform_processor_contents)
    set(jfg_rt64_transform_previous_old
        "const bool prevFrameValid = (p.prevFrame != nullptr);")
    set(jfg_rt64_transform_previous_new
        "const bool prevFrameValid = (p.prevFrame != nullptr) && p.curFrame->frameMap.workloads[w].mapped;")
    string(FIND "${jfg_rt64_transform_processor_contents}"
        "${jfg_rt64_transform_previous_old}"
        jfg_rt64_transform_previous_offset)
    if(jfg_rt64_transform_previous_offset EQUAL -1)
        message(FATAL_ERROR
            "Unable to apply the audited RT64 unmatched-workload patch")
    endif()
    string(REPLACE "${jfg_rt64_transform_previous_old}"
        "${jfg_rt64_transform_previous_new}"
        jfg_rt64_transform_processor_patched
        "${jfg_rt64_transform_processor_contents}")
    set(jfg_rt64_transform_processor_generated
        "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64/render/rt64_transform_processor.cpp")
    file(MAKE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64/render")
    file(WRITE "${jfg_rt64_transform_processor_generated}"
        "${jfg_rt64_transform_processor_patched}")

    get_target_property(jfg_rt64_sources rt64 SOURCES)
    set(jfg_rt64_patched_sources)
    set(jfg_rt64_transform_source_replaced FALSE)
    foreach(jfg_rt64_source IN LISTS jfg_rt64_sources)
        if(jfg_rt64_source MATCHES
           "(^|[/\\\\])rt64_transform_processor\\.cpp$")
            list(APPEND jfg_rt64_patched_sources
                "${jfg_rt64_transform_processor_generated}")
            set(jfg_rt64_transform_source_replaced TRUE)
        else()
            list(APPEND jfg_rt64_patched_sources "${jfg_rt64_source}")
        endif()
    endforeach()
    if(NOT jfg_rt64_transform_source_replaced)
        message(FATAL_ERROR
            "Pinned RT64 target does not contain rt64_transform_processor.cpp")
    endif()
    set_property(TARGET rt64 PROPERTY SOURCES "${jfg_rt64_patched_sources}")
    target_include_directories(rt64 PRIVATE
        "${jfg_rt64_root}/src/render")

    include(cmake/rt64_vi_presentation.cmake)
    include(cmake/rt64_depth_bounds.cmake)
    include(cmake/rt64_frame_interpolation.cmake)

    # The pinned Plume D3D12 backend assumes every CopyTextureRegion
    # destination is a texture when configuring programmable sample
    # positions. A texture-to-readback-buffer copy legitimately has no
    # destination texture, so the unguarded call dereferences null. Keep the
    # pinned checkout pristine: generate one audited compatibility translation
    # unit in the build tree and replace only that source in Plume's target.
    if(WIN32)
        set(jfg_rt64_plume_d3d12
            "${jfg_rt64_root}/src/contrib/plume/plume_d3d12.cpp")
        file(SHA256 "${jfg_rt64_plume_d3d12}"
            jfg_rt64_plume_d3d12_sha256)
        if(NOT jfg_rt64_plume_d3d12_sha256 STREQUAL
           "b8c45f82da26295b40771e53c200d0e7d60989587324ecaddce1fefeb4c7b4ba" AND
           NOT jfg_rt64_plume_d3d12_sha256 STREQUAL
           "795f1e7d75c450106b7154f4089ea4549fff111e13d6a078b81114c3e0dea317")
            message(FATAL_ERROR
                "Pinned RT64 Plume D3D12 source does not match the audited "
                "readback compatibility patch input")
        endif()
        file(READ "${jfg_rt64_plume_d3d12}"
            jfg_rt64_plume_d3d12_contents)
        set(jfg_rt64_plume_readback_old
            "        setSamplePositions(dstLocation.texture);\n        d3d->CopyTextureRegion")
        set(jfg_rt64_plume_readback_new
            "        if (dstLocation.texture != nullptr) {\n            setSamplePositions(dstLocation.texture);\n        }\n        d3d->CopyTextureRegion")
        string(FIND "${jfg_rt64_plume_d3d12_contents}"
            "${jfg_rt64_plume_readback_old}"
            jfg_rt64_plume_readback_offset)
        if(jfg_rt64_plume_readback_offset EQUAL -1)
            message(FATAL_ERROR
                "Unable to apply the audited RT64 Plume readback patch")
        endif()
        string(REPLACE "${jfg_rt64_plume_readback_old}"
            "${jfg_rt64_plume_readback_new}"
            jfg_rt64_plume_d3d12_patched
            "${jfg_rt64_plume_d3d12_contents}")
        set(jfg_rt64_plume_d3d12_generated
            "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64/plume_d3d12.cpp")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/generated/rt64")
        file(WRITE "${jfg_rt64_plume_d3d12_generated}"
            "${jfg_rt64_plume_d3d12_patched}")

        get_target_property(jfg_rt64_plume_sources plume SOURCES)
        set(jfg_rt64_plume_patched_sources)
        set(jfg_rt64_plume_source_replaced FALSE)
        foreach(jfg_rt64_plume_source IN LISTS jfg_rt64_plume_sources)
            if(jfg_rt64_plume_source MATCHES
               "(^|[/\\\\])plume_d3d12\\.cpp$")
                list(APPEND jfg_rt64_plume_patched_sources
                    "${jfg_rt64_plume_d3d12_generated}")
                set(jfg_rt64_plume_source_replaced TRUE)
            else()
                list(APPEND jfg_rt64_plume_patched_sources
                    "${jfg_rt64_plume_source}")
            endif()
        endforeach()
        if(NOT jfg_rt64_plume_source_replaced)
            message(FATAL_ERROR
                "Pinned RT64 Plume target does not contain plume_d3d12.cpp")
        endif()
        set_property(TARGET plume PROPERTY SOURCES
            "${jfg_rt64_plume_patched_sources}")
    endif()

    add_library(jfg_rt64_renderer STATIC
        src/renderer/rt64_f3ddkr.cpp
        src/renderer/rt64_callbacks.cpp
        src/renderer/rt64_shell.cpp
    )
    target_include_directories(jfg_rt64_renderer
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        PRIVATE
            "${jfg_rt64_root}/src"
            "${jfg_rt64_root}/src/contrib"
            "${jfg_rt64_root}/src/contrib/plume"
            "${jfg_rt64_root}/src/contrib/hlslpp/include"
            "${jfg_rt64_root}/src/contrib/imgui"
            "${jfg_rt64_root}/src/contrib/mupen64plus-core/src/api"
            "${jfg_rt64_root}/src/contrib/nativefiledialog-extended/src/include"
            "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/include"
            "${CMAKE_CURRENT_BINARY_DIR}/src"
    )
    if(WIN32)
        target_include_directories(jfg_rt64_renderer PRIVATE
            "${jfg_rt64_root}/src/contrib/dxc/inc")
        target_link_directories(jfg_rt64_renderer PUBLIC
            "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64")
    endif()
    target_link_libraries(jfg_rt64_renderer PUBLIC jfg_runtime PRIVATE rt64)
    target_compile_features(jfg_rt64_renderer PUBLIC cxx_std_20)
    target_compile_definitions(jfg_rt64_renderer PRIVATE
        RT_ENABLED=0
        SCRIPT_ENABLED=0
        HLSL_CPU
        FFX_GCC
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES
        NOMINMAX
        __PRFCHWINTRIN_H
    )
    jfg_set_project_warnings(jfg_rt64_renderer)
    if(MSVC)
        # The pinned RT64 public headers intentionally use anonymous unions
        # and aligned interop layouts. Keep /WX for project code while
        # suppressing those upstream-header diagnostics on this boundary.
        target_compile_options(jfg_rt64_renderer PRIVATE
            /wd4201 /wd4244 /wd4324)
    endif()

    if(JFG_BUILD_PHASE7_RT64_RUNNER)
        if(NOT WIN32)
            message(FATAL_ERROR
                "JFG_BUILD_PHASE7_RT64_RUNNER currently requires Windows")
        endif()
        add_executable(jfg-phase7-rt64-task
            src/evidence/phase7_rt64_task_runner.cpp)
        target_link_libraries(jfg-phase7-rt64-task PRIVATE
            jfg_rt64_renderer user32)
        target_compile_features(jfg-phase7-rt64-task PRIVATE cxx_std_20)
        jfg_set_project_warnings(jfg-phase7-rt64-task)
        add_executable(jfg-phase7-simulation-hash
            src/evidence/phase7_simulation_hash_runner.cpp)
        target_link_libraries(jfg-phase7-simulation-hash PRIVATE jfg_runtime)
        target_compile_features(jfg-phase7-simulation-hash PRIVATE cxx_std_20)
        jfg_set_project_warnings(jfg-phase7-simulation-hash)
        add_custom_command(TARGET jfg-phase7-rt64-task POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxcompiler.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxil.dll"
                "$<TARGET_FILE_DIR:jfg-phase7-rt64-task>"
            VERBATIM)
    endif()
endif()

add_library(jfg_testkernel STATIC
    src/testkernel/capture.cpp
    src/testkernel/corpus.cpp
    src/testkernel/fakes.cpp
    src/testkernel/journal.cpp
    src/testkernel/scheduler.cpp
    src/testkernel/world.cpp
)

target_include_directories(jfg_testkernel
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

target_compile_features(jfg_testkernel PUBLIC cxx_std_20)
jfg_set_project_warnings(jfg_testkernel)

add_executable(jfg-test src/app/jfg_test_main.cpp)
target_link_libraries(jfg-test PRIVATE jfg_testkernel)
target_compile_features(jfg-test PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg-test)

add_library(jfg_boot STATIC
    src/boot/runtime.cpp
    src/boot/reset_handoff.cpp
    src/boot/hle.cpp
    src/boot/runlink_module_table.cpp
    src/boot/executor.cpp
    src/boot/thread_scheduler.cpp
)
target_include_directories(jfg_boot
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
find_package(Threads REQUIRED)
target_link_libraries(jfg_boot PUBLIC jfg_testkernel Threads::Threads)
target_compile_features(jfg_boot PUBLIC cxx_std_20)
jfg_set_project_warnings(jfg_boot)

add_executable(jfg-boot src/app/jfg_boot_main.cpp)
target_link_libraries(jfg-boot PRIVATE jfg_boot)
target_compile_features(jfg-boot PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg-boot)

add_executable(jfg src/app/main.cpp)
target_link_libraries(jfg PRIVATE jfg_runtime)
target_compile_features(jfg PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg)

if(JFG_ENABLE_GENERATED_CODE)
    if(JFG_GENERATED_ROOT STREQUAL "")
        message(FATAL_ERROR
            "JFG_ENABLE_GENERATED_CODE requires an explicit JFG_GENERATED_ROOT. "
            "An in-worktree root must be ignored by git; an external root must remain outside the worktree."
        )
    endif()

    jfg_add_generated_code(
        NAME jfg_generated
        ROOT "${JFG_GENERATED_ROOT}"
        REQUIRE_PRIVATE_ROOT
    )
    target_link_libraries(jfg PRIVATE jfg_generated_link)

    if(JFG_BUILD_PHASE5_CAPTURE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(jfg_phase5_capture_table
            "${CMAKE_CURRENT_BINARY_DIR}/phase5-private/capture-table.inc")
        add_custom_command(
            OUTPUT "${jfg_phase5_capture_table}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_phase5_capture_table.py"
                --generated-root "${JFG_GENERATED_ROOT}"
                --output "${jfg_phase5_capture_table}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_phase5_capture_table.py"
                "${JFG_GENERATED_ROOT}/symbol_inventory.json"
            COMMENT "Generating the private Phase 5 capture table"
            VERBATIM
        )
        add_executable(jfg_phase5_capture_producer
            src/evidence/phase5_capture_producer.cpp
            "${jfg_phase5_capture_table}"
        )
        set_source_files_properties("${jfg_phase5_capture_table}"
            PROPERTIES HEADER_FILE_ONLY TRUE)
        target_include_directories(jfg_phase5_capture_producer SYSTEM PRIVATE
            "${JFG_GENERATED_ROOT}/include"
        )
        target_compile_definitions(jfg_phase5_capture_producer PRIVATE
            "JFG_PHASE5_CAPTURE_TABLE_INC=\"${jfg_phase5_capture_table}\"")
        target_link_libraries(jfg_phase5_capture_producer PRIVATE
            jfg_testkernel
            jfg_generated_link
        )
        target_compile_features(jfg_phase5_capture_producer PRIVATE cxx_std_20)
        jfg_set_project_warnings(jfg_phase5_capture_producer)
    endif()

    if(JFG_BUILD_PHASE6_NATIVE_BOOT)
        jfg_resolve_private_file(jfg_phase6_libultra_identification
            "${JFG_PHASE6_LIBULTRA_IDENTIFICATION}"
            "JFG_PHASE6_LIBULTRA_IDENTIFICATION")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(jfg_phase6_dispatch_table "${CMAKE_CURRENT_BINARY_DIR}/phase6-private/native-dispatch-table.inc")
        add_custom_command(OUTPUT "${jfg_phase6_dispatch_table}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_phase6_native_dispatch_table.py"
                --identification "${jfg_phase6_libultra_identification}" --output "${jfg_phase6_dispatch_table}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_phase6_native_dispatch_table.py" "${jfg_phase6_libultra_identification}"
            VERBATIM)
        add_executable(jfg-native-boot src/app/jfg_native_boot_main.cpp src/boot/native_boot.cpp "${jfg_phase6_dispatch_table}")
        set_source_files_properties("${jfg_phase6_dispatch_table}" PROPERTIES HEADER_FILE_ONLY TRUE)
        target_include_directories(jfg-native-boot SYSTEM PRIVATE "${JFG_GENERATED_ROOT}/include" "${JFG_GENERATED_ROOT}")
        target_compile_definitions(jfg-native-boot PRIVATE "JFG_PHASE6_DISPATCH_TABLE_INC=\"${jfg_phase6_dispatch_table}\"")
        if(JFG_ENABLE_ADDRESS_SANITIZER)
            set(jfg_phase6_sanitizer_id "address")
            set(jfg_phase6_build_suffix "-ASAN")
        else()
            set(jfg_phase6_sanitizer_id "none")
            set(jfg_phase6_build_suffix "")
        endif()
        target_compile_definitions(jfg-native-boot PRIVATE
            "JFG_PHASE6_BUILD_IDENTITY=\"${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-$<CONFIG>${jfg_phase6_build_suffix}\""
            "JFG_PHASE6_SANITIZER_ID=\"${jfg_phase6_sanitizer_id}\"")
        target_link_libraries(jfg-native-boot PRIVATE jfg_boot jfg_runtime jfg_generated_link)
        if(JFG_BUILD_PHASE8_LIVE)
            jfg_resolve_private_root(jfg_phase8_private_audio_adapter_root
                "${JFG_PHASE8_PRIVATE_AUDIO_ADAPTER_ROOT}"
                "JFG_PHASE8_PRIVATE_AUDIO_ADAPTER_ROOT")
            jfg_resolve_private_root(jfg_phase8_private_audio_runtime_include
                "${JFG_PHASE8_PRIVATE_AUDIO_RUNTIME_INCLUDE}"
                "JFG_PHASE8_PRIVATE_AUDIO_RUNTIME_INCLUDE")
            set(jfg_phase8_private_audio_adapter_source
                "${jfg_phase8_private_audio_adapter_root}/production-audio-adapter.cpp")
            set(jfg_phase8_private_audio_generated_source
                "${jfg_phase8_private_audio_adapter_root}/brokered-jfg-audio-probe.cpp")
            foreach(jfg_phase8_private_audio_file IN ITEMS
                "${jfg_phase8_private_audio_adapter_source}"
                "${jfg_phase8_private_audio_generated_source}")
                if(IS_SYMLINK "${jfg_phase8_private_audio_file}" OR
                   NOT EXISTS "${jfg_phase8_private_audio_file}" OR
                   IS_DIRECTORY "${jfg_phase8_private_audio_file}")
                    message(FATAL_ERROR
                        "The Phase 8 private audio adapter closure is incomplete")
                endif()
            endforeach()
            get_filename_component(jfg_phase8_private_audio_runtime_root
                "${jfg_phase8_private_audio_runtime_include}/../.." ABSOLUTE)
            target_sources(jfg-native-boot PRIVATE
                "${jfg_phase8_private_audio_adapter_source}"
                "${jfg_phase8_private_audio_generated_source}")
            set_property(SOURCE "${jfg_phase8_private_audio_generated_source}"
                APPEND PROPERTY COMPILE_OPTIONS
                "$<$<CXX_COMPILER_ID:MSVC>:/wd4100>"
                "$<$<CXX_COMPILER_ID:MSVC>:/wd4702>"
                "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-unused-parameter>")
            set_property(SOURCE
                "${jfg_phase8_private_audio_adapter_source}"
                "${jfg_phase8_private_audio_generated_source}"
                APPEND PROPERTY COMPILE_OPTIONS
                "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-msse4.2>")
            target_include_directories(jfg-native-boot PRIVATE
                "${jfg_phase8_private_audio_adapter_root}")
            target_include_directories(jfg-native-boot SYSTEM PRIVATE
                "${jfg_phase8_private_audio_runtime_include}"
                "${jfg_phase8_private_audio_runtime_root}/ultramodern/include"
                "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/include")
            target_compile_definitions(jfg-native-boot PRIVATE
                JFG_PHASE8_LIVE_RUNTIME=1
                SDL_MAIN_HANDLED=1)
            target_link_directories(jfg-native-boot PRIVATE
                "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64")
            target_link_libraries(jfg-native-boot PRIVATE
                jfg_rt64_renderer SDL2 user32)
            add_custom_command(TARGET jfg-native-boot POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll"
                    "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxcompiler.dll"
                    "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxil.dll"
                    "$<TARGET_FILE_DIR:jfg-native-boot>"
                VERBATIM)
            if(MSVC)
                target_link_options(jfg-native-boot PRIVATE
                    "/MAP:${CMAKE_CURRENT_BINARY_DIR}/phase8-private-native-boot.map")
            endif()
        endif()
        if(WIN32)
            target_sources(jfg-native-boot PRIVATE "${jfg_application_icon}")
            target_link_libraries(jfg-native-boot PRIVATE advapi32 psapi)
            add_dependencies(jfg-native-boot jfg-support-capture)
            # Recompiled MIPS calls are ordinary host calls; boot-time overlay
            # initialization is substantially deeper than the MSVC 1 MiB
            # default, including on the deterministic guest-thread workers.
            target_link_options(jfg-native-boot PRIVATE /STACK:16777216)
            if(JFG_ENABLE_ADDRESS_SANITIZER AND MSVC)
                get_filename_component(jfg_msvc_tool_directory
                    "${CMAKE_CXX_COMPILER}" DIRECTORY)
                set(jfg_msvc_asan_runtime
                    "${jfg_msvc_tool_directory}/clang_rt.asan_dynamic-x86_64.dll")
                if(NOT EXISTS "${jfg_msvc_asan_runtime}")
                    message(FATAL_ERROR "MSVC AddressSanitizer runtime is unavailable")
                endif()
                add_custom_command(TARGET jfg-native-boot POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${jfg_msvc_asan_runtime}" "$<TARGET_FILE_DIR:jfg-native-boot>"
                    VERBATIM)
            endif()
        endif()
        target_compile_features(jfg-native-boot PRIVATE cxx_std_20)
        jfg_set_project_warnings(jfg-native-boot)

        if(BUILD_TESTING AND JFG_BUILD_TESTS)
            if((JFG_PHASE6_ROM STREQUAL "" AND
                (NOT JFG_PHASE6_EMULATOR_CHECKPOINT STREQUAL "" OR
                 NOT JFG_PHASE6_BIZHAWK_EXECUTABLE STREQUAL "" OR
                 NOT JFG_PHASE6_EMULATOR_PROBE STREQUAL "")) OR
               (NOT JFG_PHASE6_ROM STREQUAL "" AND
                (JFG_PHASE6_EMULATOR_CHECKPOINT STREQUAL "" OR
                 JFG_PHASE6_BIZHAWK_EXECUTABLE STREQUAL "" OR
                 JFG_PHASE6_EMULATOR_PROBE STREQUAL "")))
                message(FATAL_ERROR
                    "Phase 6 ROM, checkpoint, BizHawk executable, and probe must be supplied together")
            endif()
            if(NOT JFG_PHASE6_ROM STREQUAL "")
                jfg_resolve_private_file(jfg_phase6_rom
                    "${JFG_PHASE6_ROM}" "JFG_PHASE6_ROM")
                jfg_resolve_private_file(jfg_phase6_emulator_checkpoint
                    "${JFG_PHASE6_EMULATOR_CHECKPOINT}"
                    "JFG_PHASE6_EMULATOR_CHECKPOINT")
                jfg_resolve_private_file(jfg_phase6_bizhawk_executable
                    "${JFG_PHASE6_BIZHAWK_EXECUTABLE}"
                    "JFG_PHASE6_BIZHAWK_EXECUTABLE")
                jfg_resolve_private_file(jfg_phase6_emulator_probe
                    "${JFG_PHASE6_EMULATOR_PROBE}"
                    "JFG_PHASE6_EMULATOR_PROBE")
                add_test(
                    NAME jfg.phase6_native_m2
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_phase6_native_boot.py"
                        --executable "$<TARGET_FILE:jfg-native-boot>"
                        --rom "${jfg_phase6_rom}"
                        --oracle "${jfg_phase6_emulator_checkpoint}"
                        --generated-root "${JFG_GENERATED_ROOT}"
                        --identification "${jfg_phase6_libultra_identification}"
                        --bizhawk-executable "${jfg_phase6_bizhawk_executable}"
                        --oracle-probe "${jfg_phase6_emulator_probe}"
                        --expected-build-identity "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-$<CONFIG>"
                        --private-out "${CMAKE_CURRENT_BINARY_DIR}/phase6-private/native-boot-evidence.json"
                        --public-out "${CMAKE_CURRENT_BINARY_DIR}/phase6-private/native-boot-summary.json"
                        --stub-ledger-out "${CMAKE_CURRENT_BINARY_DIR}/phase6-private/native-stub-ledger.json"
                        --repeat 3)
                set_tests_properties(jfg.phase6_native_m2 PROPERTIES TIMEOUT 240)
            endif()
        endif()
    endif()

    if(JFG_BUILD_G2_PRODUCERS)
        function(jfg_add_g2_producer target)
            add_executable(${target} ${ARGN})
            target_include_directories(${target} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
            )
            target_compile_features(${target} PRIVATE cxx_std_20)
            jfg_set_project_warnings(${target})
        endfunction()

        add_executable(jfg_g2_overlay_producer
            src/evidence/g2_overlay_producer.cpp
            src/evidence/g2_trap_probe_runtime.cpp
        )
        target_include_directories(jfg_g2_overlay_producer SYSTEM PRIVATE
            "${JFG_GENERATED_ROOT}/include"
            "${JFG_GENERATED_ROOT}"
        )
        target_link_libraries(jfg_g2_overlay_producer PRIVATE
            jfg_runtime
            jfg_generated_g2_trap_link
        )
        target_compile_features(jfg_g2_overlay_producer PRIVATE cxx_std_20)
        jfg_set_project_warnings(jfg_g2_overlay_producer)

        foreach(required_private_variable IN ITEMS
            JFG_G2_PRIVATE_GRAPHICS_ORACLE_ROOT
            JFG_G2_PRIVATE_AUDIO_ADAPTER_ROOT
            JFG_G2_PRIVATE_AUDIO_RUNTIME_INCLUDE
            JFG_G2_PRIVATE_AUDIO_ORACLE_ROOT
        )
            if("${${required_private_variable}}" STREQUAL "")
                message(FATAL_ERROR
                    "JFG_BUILD_G2_PRODUCERS requires ${required_private_variable}")
            endif()
        endforeach()

        jfg_resolve_private_root(jfg_g2_private_graphics_oracle_root
            "${JFG_G2_PRIVATE_GRAPHICS_ORACLE_ROOT}"
            "JFG_G2_PRIVATE_GRAPHICS_ORACLE_ROOT")
        jfg_resolve_private_root(jfg_g2_private_audio_adapter_root
            "${JFG_G2_PRIVATE_AUDIO_ADAPTER_ROOT}"
            "JFG_G2_PRIVATE_AUDIO_ADAPTER_ROOT")
        jfg_resolve_private_root(jfg_g2_private_audio_runtime_include
            "${JFG_G2_PRIVATE_AUDIO_RUNTIME_INCLUDE}"
            "JFG_G2_PRIVATE_AUDIO_RUNTIME_INCLUDE")
        jfg_resolve_private_root(jfg_g2_private_audio_oracle_root
            "${JFG_G2_PRIVATE_AUDIO_ORACLE_ROOT}"
            "JFG_G2_PRIVATE_AUDIO_ORACLE_ROOT")

        if(jfg_g2_private_audio_adapter_root STREQUAL
                jfg_g2_private_audio_oracle_root)
            message(FATAL_ERROR
                "G2 native and oracle adapters must use distinct private roots")
        endif()

        set(jfg_g2_private_graphics_oracle_source
            "${jfg_g2_private_graphics_oracle_root}/production-graphics-oracle-adapter.cpp")
        set(jfg_g2_private_audio_adapter_source
            "${jfg_g2_private_audio_adapter_root}/production-audio-adapter.cpp")
        set(jfg_g2_private_audio_generated_source
            "${jfg_g2_private_audio_adapter_root}/brokered-jfg-audio-probe.cpp")
        set(jfg_g2_private_audio_oracle_source
            "${jfg_g2_private_audio_oracle_root}/production-audio-oracle-adapter.cpp")
        foreach(required_private_file IN ITEMS
            "${jfg_g2_private_graphics_oracle_source}"
            "${jfg_g2_private_audio_adapter_source}"
            "${jfg_g2_private_audio_generated_source}"
            "${jfg_g2_private_audio_oracle_source}"
        )
            if(IS_SYMLINK "${required_private_file}" OR
               NOT EXISTS "${required_private_file}" OR
               IS_DIRECTORY "${required_private_file}")
                message(FATAL_ERROR
                    "A fixed G2 private adapter closure file is absent")
            endif()
        endforeach()

        jfg_add_g2_producer(jfg_g2_graphics_native_producer
            src/evidence/g2_graphics_native_producer.cpp
            src/evidence/g2_graphics_bounded_adapter.cpp
        )
        target_link_libraries(jfg_g2_graphics_native_producer PRIVATE
            jfg_runtime)

        jfg_add_g2_producer(jfg_g2_graphics_oracle_producer
            src/evidence/g2_graphics_oracle_producer.cpp
            "${jfg_g2_private_graphics_oracle_source}"
        )

        get_filename_component(jfg_g2_private_audio_runtime_root
            "${jfg_g2_private_audio_runtime_include}/../.." ABSOLUTE)
        jfg_add_g2_producer(jfg_g2_audio_native_producer
            src/evidence/g2_audio_native_producer.cpp
            "${jfg_g2_private_audio_adapter_source}"
            "${jfg_g2_private_audio_generated_source}"
        )
        set_property(SOURCE "${jfg_g2_private_audio_generated_source}"
            APPEND PROPERTY COMPILE_OPTIONS
            "$<$<CXX_COMPILER_ID:MSVC>:/wd4100>"
            "$<$<CXX_COMPILER_ID:MSVC>:/wd4702>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-unused-parameter>"
        )
        # librecomp's RSP vector unit uses SSE4 intrinsics behind always_inline
        # wrappers; GCC and Clang require the target feature explicitly, while
        # x64 MSVC exposes these intrinsics unconditionally.
        set_property(SOURCE
            "${jfg_g2_private_audio_adapter_source}"
            "${jfg_g2_private_audio_generated_source}"
            APPEND PROPERTY COMPILE_OPTIONS
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-msse4.2>"
        )
        target_include_directories(jfg_g2_audio_native_producer PRIVATE
            "${jfg_g2_private_audio_adapter_root}"
        )
        target_include_directories(jfg_g2_audio_native_producer SYSTEM PRIVATE
            "${jfg_g2_private_audio_runtime_include}"
            "${jfg_g2_private_audio_runtime_root}/ultramodern/include"
            "${JFG_GENERATED_ROOT}/include"
            "${JFG_GENERATED_ROOT}"
        )
        target_link_libraries(jfg_g2_audio_native_producer PRIVATE jfg_runtime)

        jfg_add_g2_producer(jfg_g2_audio_oracle_producer
            src/evidence/g2_audio_oracle_producer.cpp
            "${jfg_g2_private_audio_oracle_source}"
        )
        # The independent audio oracle ABI is implemented against an HLE core
        # whose build is described by the ignored oracle root itself.
        add_subdirectory("${jfg_g2_private_audio_oracle_root}"
            jfg-g2-private-audio-oracle-core EXCLUDE_FROM_ALL)
        target_link_libraries(jfg_g2_audio_oracle_producer PRIVATE
            jfg_private_mupen_hle)

        jfg_add_g2_producer(jfg_g2_save_native_producer
            src/evidence/g2_save_native_producer.cpp
        )
        target_link_libraries(jfg_g2_save_native_producer PRIVATE jfg_runtime)

        jfg_add_g2_producer(jfg_g2_save_oracle_producer
            src/evidence/g2_save_oracle_producer.cpp
        )

        jfg_add_g2_producer(jfg_g2_cpu_producer
            src/evidence/g2_cpu_producer.cpp
        )
        jfg_add_g2_producer(jfg_g2_rsp_producer
            src/evidence/g2_rsp_producer.cpp
        )
        jfg_add_g2_producer(jfg_g2_trap_producer
            src/evidence/g2_trap_producer.cpp
            src/evidence/g2_trap_probe_runtime.cpp
        )
        jfg_add_g2_producer(jfg_g2_trap_oracle_producer
            src/evidence/g2_trap_oracle_producer.cpp
        )
    endif()

    if(BUILD_TESTING AND JFG_BUILD_TESTS)
        jfg_add_generated_code_tests(
            NAME jfg_generated
            LINK_SMOKE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated_link_smoke.cpp"
            BASELINE_AUDIT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated_baseline_audit.cpp"
            PATCH_AUDIT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated_patch_audit.cpp"
        )
    endif()
endif()

if(BUILD_TESTING AND JFG_BUILD_TESTS)
    add_subdirectory(tests)
endif()

install(TARGETS jfg RUNTIME DESTINATION bin)
