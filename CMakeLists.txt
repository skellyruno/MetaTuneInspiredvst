cmake_minimum_required(VERSION 3.16)
project(MetaTuneInspiredDSPTests VERSION 1.0 LANGUAGES CXX)

# This is ONLY the portable DSP regression harness; it is not the JUCE VST3 build.
include(CTest)

function(add_dsp_regression name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_dsp_regression(autotune_validation tests/RealtimeAutotune_validation.cpp)
add_dsp_regression(autotune_latency_regression tests/RealtimeAutotune_latency_regression.cpp)
add_dsp_regression(autotune_block_regression tests/RealtimeAutotune_block_regression.cpp)
# Multi-rate quality grid is a real CTest gate; returns nonzero on outliers.
add_executable(autotune_quality_grid tests/RealtimeAutotune_quality_grid.cpp)
target_compile_features(autotune_quality_grid PRIVATE cxx_std_17)
target_include_directories(autotune_quality_grid PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
add_test(NAME autotune_quality_grid COMMAND autotune_quality_grid)

add_dsp_regression(autotune_automation_safety tests/RealtimeAutotune_automation_safety.cpp)

add_dsp_regression(autotune_glide_regression tests/RealtimeAutotune_glide_regression.cpp)

# Separate reference-controller sanity check from the production DSP.
add_executable(reference_note_controller_test research/low_latency_deep_dive/test_controller.cpp)
target_compile_features(reference_note_controller_test PRIVATE cxx_std_17)
add_test(NAME reference_note_controller_test COMMAND reference_note_controller_test)

add_dsp_regression(autotune_confidence_consonants tests/RealtimeAutotune_confidence_consonants.cpp)
add_dsp_regression(autotune_pdc_cpu tests/RealtimeAutotune_pdc_cpu.cpp)

# Offline WAV renderer and diagnostic CPU benchmark; not VST3 targets.
add_executable(autotune_offline_renderer tools/render_wav.cpp)
target_compile_features(autotune_offline_renderer PRIVATE cxx_std_17)
target_include_directories(autotune_offline_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
add_executable(autotune_cpu_benchmark tools/benchmark_cpu.cpp)
target_compile_features(autotune_cpu_benchmark PRIVATE cxx_std_17)
target_include_directories(autotune_cpu_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
