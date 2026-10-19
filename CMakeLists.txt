cmake_minimum_required(VERSION 3.16)
project(MetaTuneInspiredDSPTests VERSION 1.0 LANGUAGES CXX)

# Portable DSP regressions by default; optional JUCE VST3 builds include standard and Live variants.
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

add_dsp_regression(autotune_confidence_consonants tests/RealtimeAutotune_confidence_consonants.cpp)
add_dsp_regression(autotune_pdc_cpu tests/RealtimeAutotune_pdc_cpu.cpp)

# Offline WAV renderer and diagnostic CPU benchmark; not VST3 targets.
add_executable(autotune_offline_renderer tools/render_wav.cpp)
target_compile_features(autotune_offline_renderer PRIVATE cxx_std_17)
target_include_directories(autotune_offline_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
add_executable(autotune_cpu_benchmark tools/benchmark_cpu.cpp)
target_compile_features(autotune_cpu_benchmark PRIVATE cxx_std_17)
target_include_directories(autotune_cpu_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

add_dsp_regression(autotune_rebuild_stress tests/RealtimeAutotune_rebuild_stress.cpp)

# Optional full JUCE VST3 build. Set JUCE_PATH to a JUCE checkout:
# cmake -S . -B build-vst3 -DBUILD_VST3=ON -DJUCE_PATH=../JUCE
option(BUILD_VST3 "Build standalone VST3 target (requires JUCE)" OFF)
set(JUCE_PATH "" CACHE PATH "Path to a JUCE source checkout")
if(BUILD_VST3)
    if(JUCE_PATH AND EXISTS "${JUCE_PATH}/CMakeLists.txt")
        add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE")
    else()
        find_package(JUCE CONFIG QUIET)
        if(NOT JUCE_FOUND)
            message(FATAL_ERROR "Set -DJUCE_PATH=/absolute/path/to/JUCE or install JUCE CMake package")
        endif()
    endif()
    juce_add_plugin(MetaTuneInspired
        COMPANY_NAME "Skell Audio"
        BUNDLE_ID "com.skellaudio.metatuneinspired"
        PLUGIN_MANUFACTURER_CODE Skel
        PLUGIN_CODE MtIn
        FORMATS VST3 Standalone
        PRODUCT_NAME "Skell Tune"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE)
    juce_generate_juce_header(MetaTuneInspired)
    target_sources(MetaTuneInspired PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/RealtimeAutotune.h)
    target_compile_features(MetaTuneInspired PRIVATE cxx_std_17)
    target_compile_definitions(MetaTuneInspired PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(MetaTuneInspired PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
    # Dedicated low-latency VST3: separate plugin identity and fixed-fast DSP,
    # allowing a smaller honest host PDC without mode-switch underflow.
    option(BUILD_LIVE_VST3 "Build extra fast-only Skell Tune Live plugin" ON)
    if(BUILD_LIVE_VST3)
        juce_add_plugin(SkellTuneLive
            COMPANY_NAME "Skell Audio"
            BUNDLE_ID "com.skellaudio.skelltunelive"
            PLUGIN_MANUFACTURER_CODE Skel
            PLUGIN_CODE SkLv
            FORMATS VST3 Standalone
            PRODUCT_NAME "Skell Tune Live"
            IS_SYNTH FALSE
            NEEDS_MIDI_INPUT FALSE
            NEEDS_MIDI_OUTPUT FALSE
            IS_MIDI_EFFECT FALSE
            EDITOR_WANTS_KEYBOARD_FOCUS FALSE
            COPY_PLUGIN_AFTER_BUILD FALSE)
        juce_generate_juce_header(SkellTuneLive)
        target_sources(SkellTuneLive PRIVATE
            Source/PluginProcessor.cpp Source/PluginProcessor.h
            Source/PluginEditor.cpp Source/PluginEditor.h
            Source/RealtimeAutotune.h)
        target_compile_features(SkellTuneLive PRIVATE cxx_std_17)
        target_compile_definitions(SkellTuneLive PUBLIC
            SKELLTUNE_LIVE_FIXED_LOW_LATENCY=1
            JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
        target_link_libraries(SkellTuneLive PRIVATE juce::juce_audio_utils
            PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
    endif()
endif()
add_dsp_regression(autotune_adversarial tests/RealtimeAutotune_adversarial.cpp)
add_dsp_regression(autotune_detector_96k tests/RealtimeAutotune_detector_96k.cpp)
add_dsp_regression(autotune_realtime_safety tests/RealtimeAutotune_realtime_safety.cpp)

# Regression gate for near-unison signal transparency, both grain modes.
add_dsp_regression(autotune_unison_transparency tests/RealtimeAutotune_unison_transparency.cpp)

# Level-match gate at three rates, three voice pitches and six correction ratios.
add_dsp_regression(autotune_level_grid tests/RealtimeAutotune_level_grid.cpp)

# Low-latency and recovered sample-rate regressions. Prevent fictitious grains
# during breaths and stale FIR coefficients after changing DAW sample rates.
add_dsp_regression(autotune_low_latency_recovery tests/RealtimeAutotune_low_latency_recovery.cpp)
# Separate host latency budget for the build-time locked Live VST3 target.
add_dsp_regression(autotune_live_latency_budget tests/RealtimeAutotune_live_latency_budget.cpp)

# Blueprint-driven regressions: weak/missing fundamentals and real octave transitions.
add_dsp_regression(autotune_blueprint_pitch tests/RealtimeAutotune_blueprint_pitch.cpp)
# Explicit anti-alias confidence gate for the new 8:1 analysis signal.
add_dsp_regression(autotune_blueprint_alias tests/RealtimeAutotune_blueprint_alias.cpp)

# V7: phase-coherent near-unison handoff across both synthesis modes and rates.
add_dsp_regression(autotune_unity_handoff tests/RealtimeAutotune_unity_handoff.cpp)

# V8: a real-voice excerpt catches sustained octave errors missed by synthetic tones.
add_executable(autotune_real_vocal_octave tests/RealtimeAutotune_real_vocal_octave.cpp)
target_compile_features(autotune_real_vocal_octave PRIVATE cxx_std_17)
target_include_directories(autotune_real_vocal_octave PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
add_test(NAME autotune_real_vocal_octave_data COMMAND autotune_real_vocal_octave
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/real_voice_octave_200hz.wav")

# V9: explicit source phase-lock reset at sustained note changes.
add_dsp_regression(autotune_phase_reacquisition tests/RealtimeAutotune_phase_reacquisition.cpp)
# V9: opt-in hard tune tracking tuned using independent real-vocal A/B data.
add_dsp_regression(autotune_precision_mode tests/RealtimeAutotune_precision_mode.cpp)

# V10: protect phase continuity through host block boundaries and fractional
# source-centre fitting (normal mode); Live keeps the proven fixed-delay path.
add_dsp_regression(autotune_phase_continuity tests/RealtimeAutotune_phase_continuity.cpp)

# V11: extended full-rate lag refinement only near the 70-Hz pitch floor.
add_dsp_regression(autotune_low_register_precision tests/RealtimeAutotune_low_register_precision.cpp)

add_dsp_regression(autotune_aligned_bypass tests/RealtimeAutotune_aligned_bypass.cpp)

# V13: DAWs and drivers can supply invalid floating-point samples.
add_dsp_regression(autotune_nonfinite_recovery tests/RealtimeAutotune_nonfinite_recovery.cpp)
add_dsp_regression(autotune_block_mailbox tests/RealtimeAutotune_block_mailbox.cpp)

# V14: precision output-clock stability under expressive source vibrato.
add_dsp_regression(autotune_precision_output_clock tests/RealtimeAutotune_precision_output_clock.cpp)

# V15: moving delay taps can undo pitch correction on changing notes.
add_dsp_regression(autotune_pitch_preserving_pdc tests/RealtimeAutotune_pitch_preserving_pdc.cpp)
add_dsp_regression(autotune_stable_pdc_multirate tests/RealtimeAutotune_stable_pdc_multirate.cpp)
