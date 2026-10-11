add_executable(jfg_timer_tests timer_tests.cpp)
add_executable(jfg_guest_thread_transport_tests guest_thread_transport_tests.cpp)
target_link_libraries(jfg_guest_thread_transport_tests PRIVATE jfg_boot)
target_compile_features(jfg_guest_thread_transport_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_guest_thread_transport_tests)
add_test(NAME jfg.guest_thread_transport COMMAND jfg_guest_thread_transport_tests)
add_executable(jfg_guest_eret_observation_tests guest_eret_observation_tests.cpp)
target_link_libraries(jfg_guest_eret_observation_tests PRIVATE jfg_boot)
target_compile_features(jfg_guest_eret_observation_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_guest_eret_observation_tests)
add_test(NAME jfg.guest_eret_observation COMMAND jfg_guest_eret_observation_tests)
add_executable(jfg_eret_transfer_probe_tests eret_transfer_probe_tests.cpp)
target_include_directories(jfg_eret_transfer_probe_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_eret_transfer_probe_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_eret_transfer_probe_tests)
add_test(NAME jfg.eret_transfer_probe COMMAND jfg_eret_transfer_probe_tests)
add_executable(jfg_instruction_effect_trace_tests instruction_effect_trace_tests.cpp)
target_include_directories(jfg_instruction_effect_trace_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_instruction_effect_trace_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_instruction_effect_trace_tests)
add_test(NAME jfg.instruction_effect_trace COMMAND jfg_instruction_effect_trace_tests
  "${CMAKE_CURRENT_BINARY_DIR}/instruction-effect-test.bin")
add_executable(jfg_reference_pif_boot_tests reference_pif_boot_tests.cpp)
target_include_directories(jfg_reference_pif_boot_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_pif_boot_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_pif_boot_tests)
add_test(NAME jfg.reference_pif_boot COMMAND jfg_reference_pif_boot_tests)
add_executable(jfg_execution_clock_tests execution_clock_tests.cpp)
target_link_libraries(jfg_execution_clock_tests PRIVATE jfg_boot)
target_compile_features(jfg_execution_clock_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_execution_clock_tests)
add_test(NAME jfg.execution_clock COMMAND jfg_execution_clock_tests)
add_executable(jfg_tlb_tests tlb_tests.cpp)
target_include_directories(jfg_tlb_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_tlb_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_tlb_tests)
add_test(NAME jfg.tlb COMMAND jfg_tlb_tests)
add_executable(jfg_mi_interrupt_mask_tests mi_interrupt_mask_tests.cpp)
target_include_directories(jfg_mi_interrupt_mask_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_mi_interrupt_mask_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_mi_interrupt_mask_tests)
add_test(NAME jfg.mi_interrupt_mask COMMAND jfg_mi_interrupt_mask_tests)
add_executable(jfg_sp_status_tests sp_status_tests.cpp)
target_include_directories(jfg_sp_status_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_sp_status_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_sp_status_tests)
add_test(NAME jfg.sp_status COMMAND jfg_sp_status_tests)
add_executable(jfg_point_probe_tests point_probe_tests.cpp)
target_include_directories(jfg_point_probe_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_point_probe_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_point_probe_tests)
add_test(NAME jfg.point_probe COMMAND jfg_point_probe_tests)

add_executable(jfg_device_event_probe_tests device_event_probe_tests.c)
target_include_directories(jfg_device_event_probe_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_device_event_probe_tests PRIVATE c_std_11)
jfg_set_project_warnings(jfg_device_event_probe_tests)
add_test(NAME jfg.device_event_probe COMMAND jfg_device_event_probe_tests)
add_executable(jfg_device_event_probe_cpp_tests device_event_probe_cpp_tests.cpp)
target_include_directories(jfg_device_event_probe_cpp_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_device_event_probe_cpp_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_device_event_probe_cpp_tests)
add_test(NAME jfg.device_event_probe_cpp COMMAND jfg_device_event_probe_cpp_tests)

add_executable(jfg_reference_event_commit_tests reference_event_commit_tests.cpp)
target_include_directories(jfg_reference_event_commit_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_event_commit_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_event_commit_tests)
add_test(NAME jfg.reference_event_commit COMMAND jfg_reference_event_commit_tests)

add_executable(jfg_reference_flash_bus_tests reference_flash_bus_tests.cpp)
target_link_libraries(jfg_reference_flash_bus_tests PRIVATE jfg_runtime)
target_compile_features(jfg_reference_flash_bus_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_flash_bus_tests)
add_test(NAME jfg.reference_flash_bus COMMAND jfg_reference_flash_bus_tests)

add_executable(jfg_reference_ai_dma_tests reference_ai_dma_tests.cpp)
target_include_directories(jfg_reference_ai_dma_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_ai_dma_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_ai_dma_tests)
add_test(NAME jfg.reference_ai_dma COMMAND jfg_reference_ai_dma_tests)

add_executable(jfg_reference_si_dma_tests reference_si_dma_tests.cpp)
target_link_libraries(jfg_reference_si_dma_tests PRIVATE jfg_runtime)
target_include_directories(jfg_reference_si_dma_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_si_dma_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_si_dma_tests)
add_test(NAME jfg.reference_si_dma COMMAND jfg_reference_si_dma_tests)

add_executable(jfg_reference_compare_tests reference_compare_tests.cpp)
target_include_directories(jfg_reference_compare_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_compare_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_compare_tests)
add_test(NAME jfg.reference_compare COMMAND jfg_reference_compare_tests)

add_executable(jfg_reference_pi_dma_tests reference_pi_dma_tests.cpp)
target_include_directories(jfg_reference_pi_dma_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_pi_dma_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_pi_dma_tests)
add_test(NAME jfg.reference_pi_dma COMMAND jfg_reference_pi_dma_tests)

add_executable(jfg_reference_sp_dma_tests reference_sp_dma_tests.cpp)
target_include_directories(jfg_reference_sp_dma_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_sp_dma_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_sp_dma_tests)
add_test(NAME jfg.reference_sp_dma COMMAND jfg_reference_sp_dma_tests)
add_executable(jfg_reference_device_timing_tests reference_device_timing_tests.cpp)
target_include_directories(jfg_reference_device_timing_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_reference_device_timing_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reference_device_timing_tests)
add_test(NAME jfg.reference_device_timing COMMAND jfg_reference_device_timing_tests)
add_executable(jfg_oracle_round_even_tests oracle_round_even_tests.c)
target_compile_features(jfg_oracle_round_even_tests PRIVATE c_std_99)
if(NOT MSVC)
    target_link_libraries(jfg_oracle_round_even_tests PRIVATE m)
    target_compile_options(jfg_oracle_round_even_tests PRIVATE -frounding-math)
endif()
add_test(NAME jfg.oracle_round_even COMMAND jfg_oracle_round_even_tests)
target_link_libraries(jfg_timer_tests PRIVATE jfg_boot)
target_compile_features(jfg_timer_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_timer_tests)
add_test(NAME jfg.timers COMMAND jfg_timer_tests)

add_executable(jfg_unit_tests
    runtime_tests.cpp
)

target_link_libraries(jfg_unit_tests PRIVATE jfg_runtime)
target_compile_features(jfg_unit_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_unit_tests)

add_test(NAME jfg.unit COMMAND jfg_unit_tests)

# This deliberately does not link jfg_runtime or any generated-code target:
# the probe exports the alternate trap ABI symbols that minimal_runtime.cpp
# normally owns.  It is a ROM-free mechanics test only, not G2 evidence.
add_executable(jfg_g2_trap_probe_runtime_tests
    "${PROJECT_SOURCE_DIR}/src/evidence/g2_trap_probe_runtime.cpp"
    g2_trap_probe_runtime_tests.cpp
)
target_include_directories(jfg_g2_trap_probe_runtime_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(jfg_g2_trap_probe_runtime_tests PRIVATE cxx_std_20)
target_compile_definitions(jfg_g2_trap_probe_runtime_tests PRIVATE JFG_G2_TRAP_PROBE_TESTING=1)
find_package(Threads REQUIRED)
target_link_libraries(jfg_g2_trap_probe_runtime_tests PRIVATE Threads::Threads)
jfg_set_project_warnings(jfg_g2_trap_probe_runtime_tests)
add_test(
    NAME jfg.g2_trap_probe_runtime
    COMMAND jfg_g2_trap_probe_runtime_tests
)

add_executable(jfg_audio_task_bridge_tests
    audio_task_bridge_tests.cpp
)
target_link_libraries(jfg_audio_task_bridge_tests PRIVATE jfg_runtime)
target_compile_features(jfg_audio_task_bridge_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_audio_task_bridge_tests)
add_test(NAME jfg.audio_task_bridge COMMAND jfg_audio_task_bridge_tests)

add_executable(jfg_input_replay_tests
    input_replay_tests.cpp
)
target_link_libraries(jfg_input_replay_tests PRIVATE jfg_runtime)
target_compile_features(jfg_input_replay_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_input_replay_tests)
add_test(NAME jfg.input_replay COMMAND jfg_input_replay_tests)

add_executable(jfg_input_stick_tests
    input_stick_tests.cpp
)
target_link_libraries(jfg_input_stick_tests PRIVATE jfg_runtime)
target_compile_features(jfg_input_stick_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_input_stick_tests)
add_test(NAME jfg.input_stick COMMAND jfg_input_stick_tests)

add_executable(jfg_cic_nus_6105_tests
    cic_nus_6105_tests.cpp
)
target_link_libraries(jfg_cic_nus_6105_tests PRIVATE jfg_runtime)
target_compile_features(jfg_cic_nus_6105_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_cic_nus_6105_tests)
add_test(NAME jfg.cic_nus_6105 COMMAND jfg_cic_nus_6105_tests)

add_executable(jfg_controller_pak_tests
    controller_pak_tests.cpp
)
target_link_libraries(jfg_controller_pak_tests PRIVATE jfg_runtime)
target_compile_features(jfg_controller_pak_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_controller_pak_tests)
add_test(NAME jfg.controller_pak COMMAND jfg_controller_pak_tests)

add_executable(jfg_custom_overlay_relocator_tests
    custom_overlay_relocator_tests.cpp
)
target_link_libraries(jfg_custom_overlay_relocator_tests PRIVATE jfg_runtime)
target_compile_features(jfg_custom_overlay_relocator_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_custom_overlay_relocator_tests)
add_test(
    NAME jfg.custom_overlay_relocator
    COMMAND jfg_custom_overlay_relocator_tests
)

add_executable(jfg_save_device_runtime_tests
    save_device_runtime_tests.cpp
)
target_link_libraries(jfg_save_device_runtime_tests PRIVATE jfg_runtime)
target_compile_features(jfg_save_device_runtime_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_save_device_runtime_tests)
add_test(NAME jfg.save_device_runtime COMMAND jfg_save_device_runtime_tests)

add_executable(jfg_generated_overlay_runtime_tests
    generated_overlay_runtime_tests.cpp
)
target_link_libraries(jfg_generated_overlay_runtime_tests PRIVATE jfg_runtime)
target_compile_features(jfg_generated_overlay_runtime_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_generated_overlay_runtime_tests)
add_test(
    NAME jfg.generated_overlay_runtime
    COMMAND jfg_generated_overlay_runtime_tests
)

add_executable(jfg_graphics_task_bridge_tests
    graphics_task_bridge_tests.cpp
)
target_link_libraries(jfg_graphics_task_bridge_tests PRIVATE jfg_runtime)
target_compile_features(jfg_graphics_task_bridge_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_graphics_task_bridge_tests)
add_test(NAME jfg.graphics_task_bridge COMMAND jfg_graphics_task_bridge_tests)

add_executable(jfg_rt64_overlay_address_tests
    rt64_overlay_address_tests.cpp
)
target_link_libraries(jfg_rt64_overlay_address_tests PRIVATE jfg_runtime)
target_compile_features(jfg_rt64_overlay_address_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_rt64_overlay_address_tests)
add_test(NAME jfg.rt64_overlay_address COMMAND jfg_rt64_overlay_address_tests)

add_executable(jfg_renderer_callbacks_tests
    renderer_callbacks_tests.cpp
)
target_link_libraries(jfg_renderer_callbacks_tests PRIVATE jfg_runtime)
target_compile_features(jfg_renderer_callbacks_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_renderer_callbacks_tests)
add_test(NAME jfg.renderer_callbacks COMMAND jfg_renderer_callbacks_tests)

if(TARGET jfg_rt64_renderer)
    add_executable(jfg_rt64_shell_tests
        rt64_shell_tests.cpp
    )
    target_link_libraries(jfg_rt64_shell_tests PRIVATE jfg_rt64_renderer)
    target_include_directories(jfg_rt64_shell_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/renderer"
    )
    target_compile_features(jfg_rt64_shell_tests PRIVATE cxx_std_20)
    jfg_set_project_warnings(jfg_rt64_shell_tests)
    if(WIN32)
        add_custom_command(TARGET jfg_rt64_shell_tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxcompiler.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxil.dll"
                "$<TARGET_FILE_DIR:jfg_rt64_shell_tests>"
            VERBATIM)
    endif()
    add_test(NAME jfg.rt64_shell COMMAND jfg_rt64_shell_tests)

    # Exercise the installed command handlers against RT64's CPU state.
    # These tests do not create a graphics device or require a game ROM.
    add_executable(jfg_rt64_f3ddkr_texture_tests rt64_f3ddkr_texture_tests.cpp)
    target_link_libraries(jfg_rt64_f3ddkr_texture_tests PRIVATE jfg_rt64_renderer)
    target_include_directories(jfg_rt64_f3ddkr_texture_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/renderer"
        $<TARGET_PROPERTY:jfg_rt64_renderer,INCLUDE_DIRECTORIES>)
    target_compile_definitions(jfg_rt64_f3ddkr_texture_tests PRIVATE
        $<TARGET_PROPERTY:jfg_rt64_renderer,COMPILE_DEFINITIONS>)
    target_compile_features(jfg_rt64_f3ddkr_texture_tests PRIVATE cxx_std_20)
    jfg_set_project_warnings(jfg_rt64_f3ddkr_texture_tests)
    if(MSVC)
        target_compile_options(jfg_rt64_f3ddkr_texture_tests PRIVATE
            /wd4201 /wd4244 /wd4324)
    endif()
    if(WIN32)
        add_custom_command(TARGET jfg_rt64_f3ddkr_texture_tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxcompiler.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxil.dll"
                "$<TARGET_FILE_DIR:jfg_rt64_f3ddkr_texture_tests>"
            VERBATIM)
    endif()
    add_test(NAME jfg.rt64_f3ddkr_texture COMMAND jfg_rt64_f3ddkr_texture_tests)

    add_executable(jfg_rt64_depth_bounds_tests rt64_depth_bounds_tests.cpp)
    target_link_libraries(jfg_rt64_depth_bounds_tests PRIVATE jfg_rt64_renderer)
    target_include_directories(jfg_rt64_depth_bounds_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/renderer"
        $<TARGET_PROPERTY:jfg_rt64_renderer,INCLUDE_DIRECTORIES>)
    target_compile_definitions(jfg_rt64_depth_bounds_tests PRIVATE
        $<TARGET_PROPERTY:jfg_rt64_renderer,COMPILE_DEFINITIONS>)
    target_compile_features(jfg_rt64_depth_bounds_tests PRIVATE cxx_std_20)
    jfg_set_project_warnings(jfg_rt64_depth_bounds_tests)
    if(MSVC)
        target_compile_options(jfg_rt64_depth_bounds_tests PRIVATE
            /wd4201 /wd4244 /wd4324)
    endif()
    if(WIN32)
        add_custom_command(TARGET jfg_rt64_depth_bounds_tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${jfg_rt64_root}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxcompiler.dll"
                "${jfg_rt64_root}/src/contrib/dxc/bin/x64/dxil.dll"
                "$<TARGET_FILE_DIR:jfg_rt64_depth_bounds_tests>"
            VERBATIM)
    endif()
    add_test(NAME jfg.rt64_depth_bounds COMMAND jfg_rt64_depth_bounds_tests)
endif()

add_executable(jfg_bounded_custom_graphics_tests
    bounded_custom_graphics_tests.cpp
)
target_link_libraries(jfg_bounded_custom_graphics_tests PRIVATE jfg_runtime)
target_compile_features(jfg_bounded_custom_graphics_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_bounded_custom_graphics_tests)
add_test(
    NAME jfg.bounded_custom_graphics
    COMMAND jfg_bounded_custom_graphics_tests
)

add_executable(jfg_testkernel_tests
    testkernel_tests.cpp
)
target_link_libraries(jfg_testkernel_tests PRIVATE jfg_testkernel)
target_compile_features(jfg_testkernel_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_testkernel_tests)
add_test(NAME jfg.testkernel COMMAND jfg_testkernel_tests)

add_test(NAME jfg.testkernel_self_check COMMAND jfg-test self-check)
add_test(NAME jfg.testkernel_repeat COMMAND jfg-test repeat 5)

add_executable(jfg_boot_tests
    boot_tests.cpp
)
target_link_libraries(jfg_boot_tests PRIVATE jfg_boot)
target_compile_features(jfg_boot_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_boot_tests)
add_test(NAME jfg.boot COMMAND jfg_boot_tests)

add_executable(jfg_reset_handoff_tests reset_handoff_tests.cpp)
target_link_libraries(jfg_reset_handoff_tests PRIVATE jfg_boot)
target_compile_features(jfg_reset_handoff_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_reset_handoff_tests)
add_test(NAME jfg.reset_handoff COMMAND jfg_reset_handoff_tests)

add_executable(jfg_hle_tests
    hle_tests.cpp
)
target_link_libraries(jfg_hle_tests PRIVATE jfg_boot)
target_compile_features(jfg_hle_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_hle_tests)
add_test(NAME jfg.hle COMMAND jfg_hle_tests)

add_executable(jfg_runlink_module_table_tests
    runlink_module_table_tests.cpp
)
target_link_libraries(jfg_runlink_module_table_tests PRIVATE jfg_boot)
target_compile_features(jfg_runlink_module_table_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_runlink_module_table_tests)
add_test(
    NAME jfg.runlink_module_table
    COMMAND jfg_runlink_module_table_tests
)

add_executable(jfg_executor_tests
    executor_tests.cpp
)
target_link_libraries(jfg_executor_tests PRIVATE jfg_boot)
target_compile_features(jfg_executor_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_executor_tests)
add_test(NAME jfg.executor COMMAND jfg_executor_tests)

add_executable(jfg_thread_scheduler_tests
    thread_scheduler_tests.cpp
)
target_link_libraries(jfg_thread_scheduler_tests PRIVATE jfg_boot)
target_compile_features(jfg_thread_scheduler_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_thread_scheduler_tests)
add_test(NAME jfg.thread_scheduler COMMAND jfg_thread_scheduler_tests)

add_test(NAME jfg.boot_self_check COMMAND jfg-boot self-check)
add_test(NAME jfg.boot_repeat COMMAND jfg-boot repeat 5)
add_test(NAME jfg.boot_invalid_rom COMMAND jfg-boot invalid-rom)

add_test(
    NAME jfg.no_rom
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:jfg>"
        "-DEXPECTED=No ROM was provided"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_expected_failure.cmake"
)

add_test(
    NAME jfg.missing_rom
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:jfg>"
        "-DARGUMENTS=--rom;definitely-missing.z64"
        "-DEXPECTED=ROM validation failed"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_expected_failure.cmake"
)

if(JFG_BUILD_SYNTHETIC_GENERATED_TESTS)
    jfg_add_generated_code(
        NAME jfg_synthetic_generated
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated-code-synthetic"
        ALLOW_TRACKED_ROM_FREE_FIXTURE
    )
    jfg_add_generated_code_tests(
        NAME jfg_synthetic_generated
        LINK_SMOKE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated_link_smoke.cpp"
        BASELINE_AUDIT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated_baseline_audit.cpp"
        PATCH_AUDIT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated_patch_audit.cpp"
    )

    add_executable(jfg_generated_runtime_capacity_tests
        generated_runtime_capacity_tests.cpp
    )
    target_include_directories(jfg_generated_runtime_capacity_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/include")
    target_include_directories(jfg_generated_runtime_capacity_tests SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated-code-synthetic/include"
    )
    target_link_libraries(jfg_generated_runtime_capacity_tests PRIVATE
        jfg_synthetic_generated_minimal_runtime_objects
    )
    target_compile_features(jfg_generated_runtime_capacity_tests PRIVATE cxx_std_20)
    jfg_set_project_warnings(jfg_generated_runtime_capacity_tests)
    add_test(
        NAME jfg_synthetic_generated.runtime_capacity
        COMMAND jfg_generated_runtime_capacity_tests
    )

    add_test(
        NAME jfg_synthetic_generated.reject_tracked_real_root
        COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=${CMAKE_COMMAND}"
            "-DARGUMENTS=-S;${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated-code-synthetic/configure-tests/tracked-root;-B;${CMAKE_CURRENT_BINARY_DIR}/n-tr;-DJFG_PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DEXPECTED=must be ignored by git"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_expected_failure.cmake"
    )

    add_test(
        NAME jfg_synthetic_generated.reject_include_escape
        COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=${CMAKE_COMMAND}"
            "-DARGUMENTS=-S;${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated-code-synthetic/configure-tests/include-escape;-B;${CMAKE_CURRENT_BINARY_DIR}/n-ie;-DJFG_PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DEXPECTED=n64recomp_include must use a canonical contained path"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_expected_failure.cmake"
    )

    add_test(
        NAME jfg_synthetic_generated.reject_float_version
        COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=${CMAKE_COMMAND}"
            "-DARGUMENTS=-S;${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated-code-synthetic/configure-tests/version-float;-B;${CMAKE_CURRENT_BINARY_DIR}/n-fv;-DJFG_PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DEXPECTED=source-manifest: unsupported-version"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_expected_failure.cmake"
    )

    foreach(rejection_case
        multi-function
        vacuous-registry
        extra-registry-reference
        generated-runtime-data-owner
        extra-generated-data
        compiler-data-near-name
    )
        if(rejection_case STREQUAL "multi-function")
            set(rejection_case_id "mf")
        elseif(rejection_case STREQUAL "vacuous-registry")
            set(rejection_case_id "vr")
        elseif(rejection_case STREQUAL "extra-registry-reference")
            set(rejection_case_id "er")
        elseif(rejection_case STREQUAL "generated-runtime-data-owner")
            set(rejection_case_id "rd")
        elseif(rejection_case STREQUAL "extra-generated-data")
            set(rejection_case_id "gd")
        elseif(rejection_case STREQUAL "compiler-data-near-name")
            set(rejection_case_id "cd")
        else()
            message(FATAL_ERROR "Missing compact ID for generated-code rejection case")
        endif()
        add_test(
            NAME jfg_synthetic_generated.reject_${rejection_case}
            COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated-code-synthetic/configure-tests/object-shape"
                "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}/n-${rejection_case_id}"
                "-DCASE=${rejection_case}"
                "-DPROJECT_SOURCE=${PROJECT_SOURCE_DIR}"
                "-DGENERATOR=${CMAKE_GENERATOR}"
                "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                "-DC_COMPILER=${CMAKE_C_COMPILER}"
                "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DRC_COMPILER=${CMAKE_RC_COMPILER}"
                "-DCMAKE_MT_TOOL=${CMAKE_MT}"
                "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
                "-DCONFIG=$<CONFIG>"
                "-DEXPECTED=$<IF:$<STREQUAL:${rejection_case},multi-function>,external-function-count=2 expected=1,$<IF:$<STREQUAL:${rejection_case},vacuous-registry>,incomplete-coverage,$<IF:$<STREQUAL:${rejection_case},extra-registry-reference>,unexpected-reference-count=1,$<IF:$<STREQUAL:${rejection_case},generated-runtime-data-owner>,forbidden-runtime-data-owner-count=1,data-inventory-mismatch actual-count=2>>>>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_configure_build_failure.cmake"
        )
    endforeach()

    add_test(
        NAME jfg_synthetic_generated.reject_hidden_patch_anchor
        COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=${CMAKE_COMMAND}"
            "-DARGUMENTS=-S;${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated-code-synthetic/configure-tests/hidden-anchor;-B;${CMAKE_CURRENT_BINARY_DIR}/n-ha;-DJFG_PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DEXPECTED=source-manifest: invalid-member-set"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_expected_failure.cmake"
    )
endif()

add_executable(jfg_controller_mapping_tests controller_mapping_tests.cpp)
target_link_libraries(jfg_controller_mapping_tests PRIVATE jfg_runtime)
target_compile_features(jfg_controller_mapping_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_controller_mapping_tests)
add_test(NAME jfg.controller_mapping COMMAND jfg_controller_mapping_tests "${PROJECT_SOURCE_DIR}/tests/fixtures/controller-default.ini")

add_executable(jfg_ipl_handoff_tests ipl_handoff_tests.cpp)
target_include_directories(jfg_ipl_handoff_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_ipl_handoff_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_ipl_handoff_tests)
add_test(NAME jfg.ipl_handoff COMMAND jfg_ipl_handoff_tests)

add_executable(jfg_gameplay_trace_tests gameplay_trace_tests.cpp)
target_include_directories(jfg_gameplay_trace_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_gameplay_trace_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_gameplay_trace_tests)
add_test(NAME jfg.gameplay_trace COMMAND jfg_gameplay_trace_tests "${CMAKE_CURRENT_BINARY_DIR}/gameplay-trace-test.tsv")

add_executable(jfg_vi_presentation_tests vi_presentation_tests.cpp)
target_include_directories(jfg_vi_presentation_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_vi_presentation_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_vi_presentation_tests)
add_test(NAME jfg.vi_presentation COMMAND jfg_vi_presentation_tests)

add_executable(jfg_widescreen_tests widescreen_tests.cpp)
target_include_directories(jfg_widescreen_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_widescreen_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_widescreen_tests)
add_test(NAME jfg.widescreen COMMAND jfg_widescreen_tests)

add_executable(jfg_video_settings_tests video_settings_tests.cpp)
target_include_directories(jfg_video_settings_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_video_settings_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_video_settings_tests)
add_test(NAME jfg.video_settings COMMAND jfg_video_settings_tests)

add_executable(jfg_rgo_framerate_tests rgo_framerate_tests.cpp)
target_include_directories(jfg_rgo_framerate_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_rgo_framerate_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_rgo_framerate_tests)
add_test(NAME jfg.rgo_framerate COMMAND jfg_rgo_framerate_tests)

add_executable(jfg_rgo_gore_tests rgo_gore_tests.cpp)
target_include_directories(jfg_rgo_gore_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_rgo_gore_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_rgo_gore_tests)
add_test(NAME jfg.rgo_gore COMMAND jfg_rgo_gore_tests)

add_executable(jfg_frame_interpolation_tests frame_interpolation_tests.cpp)
target_include_directories(jfg_frame_interpolation_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_frame_interpolation_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_frame_interpolation_tests)
add_test(NAME jfg.frame_interpolation COMMAND jfg_frame_interpolation_tests)

add_executable(jfg_widescreen_hud_tests widescreen_hud_tests.cpp)
target_include_directories(jfg_widescreen_hud_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_widescreen_hud_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_widescreen_hud_tests)
add_test(NAME jfg.widescreen_hud COMMAND jfg_widescreen_hud_tests)

add_executable(jfg_navigation_mod_tests navigation_mod_tests.cpp)
target_include_directories(jfg_navigation_mod_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_navigation_mod_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_navigation_mod_tests)
add_test(NAME jfg.navigation_mod COMMAND jfg_navigation_mod_tests)

add_executable(jfg_master_volume_tests master_volume_tests.cpp)
target_include_directories(jfg_master_volume_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_master_volume_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_master_volume_tests)
add_test(NAME jfg.master_volume COMMAND jfg_master_volume_tests)

add_executable(jfg_sync_print_sink_tests sync_print_sink_tests.cpp)
target_link_libraries(jfg_sync_print_sink_tests PRIVATE jfg_boot)
target_compile_features(jfg_sync_print_sink_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_sync_print_sink_tests)
add_test(NAME jfg.sync_print_sink COMMAND jfg_sync_print_sink_tests)

add_executable(jfg_sound_queue_recovery_tests sound_queue_recovery_tests.cpp)
target_link_libraries(jfg_sound_queue_recovery_tests PRIVATE jfg_boot)
target_compile_features(jfg_sound_queue_recovery_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_sound_queue_recovery_tests)
add_test(NAME jfg.sound_queue_recovery COMMAND jfg_sound_queue_recovery_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME jfg.rt64_shader_dependencies
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/test_rt64_shader_dependencies.py"
        --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
        --make-program "${CMAKE_MAKE_PROGRAM}"
        --work-root "${CMAKE_CURRENT_BINARY_DIR}/shader-fixtures")

add_executable(jfg_playback_buffer_tests playback_buffer_tests.cpp)
target_include_directories(jfg_playback_buffer_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_playback_buffer_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_playback_buffer_tests)
add_test(NAME jfg.playback_buffer COMMAND jfg_playback_buffer_tests)

add_executable(jfg_controller_ports_tests controller_ports_tests.cpp)
target_link_libraries(jfg_controller_ports_tests PRIVATE jfg_runtime)
target_include_directories(jfg_controller_ports_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_controller_ports_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_controller_ports_tests)
add_test(NAME jfg.controller_ports COMMAND jfg_controller_ports_tests)

add_executable(jfg_gamepad_tests gamepad_tests.cpp)
target_include_directories(jfg_gamepad_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_gamepad_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_gamepad_tests)
add_test(NAME jfg.gamepad COMMAND jfg_gamepad_tests)

add_executable(jfg_dual_input_tests dual_input_tests.cpp)
target_include_directories(jfg_dual_input_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_dual_input_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_dual_input_tests)
add_test(NAME jfg.dual_input COMMAND jfg_dual_input_tests)

add_executable(jfg_controller_assignment_tests controller_assignment_tests.cpp)
target_include_directories(jfg_controller_assignment_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_controller_assignment_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_controller_assignment_tests)
add_test(NAME jfg.controller_assignment COMMAND jfg_controller_assignment_tests)

add_executable(jfg_pc_input_tests pc_input_tests.cpp)
target_link_libraries(jfg_pc_input_tests PRIVATE jfg_runtime)
target_compile_features(jfg_pc_input_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_pc_input_tests)
add_test(NAME jfg.pc_input COMMAND jfg_pc_input_tests)

add_executable(jfg_original_timing_runtime_tests original_timing_runtime_tests.cpp)
target_include_directories(jfg_original_timing_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(jfg_original_timing_runtime_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_original_timing_runtime_tests)
add_test(NAME jfg.original_timing_runtime COMMAND jfg_original_timing_runtime_tests)

foreach(async_test async_diagnostic_file async_snapshots)
  add_executable(jfg_${async_test}_tests ${async_test}_tests.cpp)
  target_include_directories(jfg_${async_test}_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
  target_compile_features(jfg_${async_test}_tests PRIVATE cxx_std_20)
  target_compile_definitions(jfg_${async_test}_tests PRIVATE NOMINMAX)
  jfg_set_project_warnings(jfg_${async_test}_tests)
  add_test(NAME jfg.${async_test} COMMAND jfg_${async_test}_tests "${CMAKE_CURRENT_BINARY_DIR}/${async_test}.txt")
endforeach()

add_executable(jfg_original_timing_fast_tests original_timing_fast_tests.cpp)
target_include_directories(jfg_original_timing_fast_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/original-timing-context")
target_compile_features(jfg_original_timing_fast_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_original_timing_fast_tests)
add_test(NAME jfg.original_timing_fast COMMAND jfg_original_timing_fast_tests)

add_executable(jfg_twin_stick_tests twin_stick_tests.cpp)
target_link_libraries(jfg_twin_stick_tests PRIVATE jfg_runtime)
target_compile_features(jfg_twin_stick_tests PRIVATE cxx_std_20)
jfg_set_project_warnings(jfg_twin_stick_tests)
add_test(NAME jfg.twin_stick COMMAND jfg_twin_stick_tests)
