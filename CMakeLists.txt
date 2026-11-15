cmake_minimum_required(VERSION 3.22)
project(MetaTuneInspired VERSION 1.13.0)

# Configure with -DJUCE_PATH=/path/to/JUCE (JUCE 7.0.8+ recommended).
if(NOT DEFINED JUCE_PATH)
  message(FATAL_ERROR "Set JUCE_PATH to your local JUCE checkout, e.g. -DJUCE_PATH=C:/JUCE")
endif()
add_subdirectory(${JUCE_PATH} JUCE)

# Research build is a SEPARATE plugin. Never use its distinct ID or binary
# to overwrite an approved MetaTune installation or FL Studio project.
option(SKELLTUNE_LOW_VOICE_LAB "Build unapproved low-register research engine as separate VST3" OFF)
option(SKELLTUNE_LAB_FAST_2P5 "Build optional 2.5ms nominal LAB prototype; NEVER a production default" OFF)
option(SKELLTUNE_3WAY_AB_LAB "Build a single separate Current / 3.4 / 2.5 VST3 listening comparison" OFF)
if(SKELLTUNE_3WAY_AB_LAB AND (NOT SKELLTUNE_LOW_VOICE_LAB OR NOT SKELLTUNE_LAB_FAST_2P5))
  message(FATAL_ERROR "3-way listening LAB requires the low-voice and fast-LAB DSP source paths")
endif()
if(SKELLTUNE_LAB_FAST_2P5 AND NOT SKELLTUNE_LOW_VOICE_LAB)
  message(FATAL_ERROR "Fast 2.5ms path must use separate LowVoice LAB plugin identity")
endif()
set(SKELLTUNE_PLUGIN_BUNDLE_ID "audio.independentaudio.metatuneinspired")
set(SKELLTUNE_PLUGIN_CODE MtIn)
set(SKELLTUNE_PRODUCT_NAME "MetaTune Inspired")
set(SKELLTUNE_PLUGIN_FORMATS VST3 Standalone)
if(SKELLTUNE_LOW_VOICE_LAB)
  if(SKELLTUNE_3WAY_AB_LAB)
    set(SKELLTUNE_PLUGIN_BUNDLE_ID "audio.independentaudio.metatune.abcomparelab")
    set(SKELLTUNE_PLUGIN_CODE MtAb)
    set(SKELLTUNE_PRODUCT_NAME "MetaTune A-B LAB")
  elseif(SKELLTUNE_LAB_FAST_2P5)
    set(SKELLTUNE_PLUGIN_BUNDLE_ID "audio.independentaudio.metatune.lowvoicefastlab")
    set(SKELLTUNE_PLUGIN_CODE MtL2)
    set(SKELLTUNE_PRODUCT_NAME "MetaTune LowVoice 2.5 LAB")
  else()
    set(SKELLTUNE_PLUGIN_BUNDLE_ID "audio.independentaudio.metatune.lowvoicelab")
    set(SKELLTUNE_PLUGIN_CODE MtLv)
    set(SKELLTUNE_PRODUCT_NAME "MetaTune LowVoice LAB")
  endif()
  set(SKELLTUNE_PLUGIN_FORMATS VST3)
endif()
juce_add_plugin(MetaTuneInspired
    COMPANY_NAME "IndependentAudio"
    BUNDLE_ID "${SKELLTUNE_PLUGIN_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE InAu
    PLUGIN_CODE ${SKELLTUNE_PLUGIN_CODE}
    FORMATS ${SKELLTUNE_PLUGIN_FORMATS}
    PRODUCT_NAME "${SKELLTUNE_PRODUCT_NAME}"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)
juce_generate_juce_header(MetaTuneInspired)
target_sources(MetaTuneInspired PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/RealtimeAutotune.h
    Source/SkellyFusionEngine.h
    Source/FusionPeriodGuard.h
    Source/PitchRiskModel.h
    Source/RealtimeLatencyLimits.h
    Source/SnapLiveShifter.h
    # The supplied LyraTune DSP is now the actual tuned-audio implementation.
    Source/LyraDSP/TunerChain.cpp
    Source/LyraDSP/Pitch/YinDetector.cpp
    Source/LyraDSP/Pitch/TargetSelector.cpp
    Source/LyraDSP/Pitch/CorrectionEngine.cpp
    Source/LyraDSP/Pitch/PitchShifter.cpp
    Source/LyraDSP/Doubler.cpp
    Source/LyraDSP/FormantAccent.cpp
)
target_compile_features(MetaTuneInspired PRIVATE cxx_std_17)
if(SKELLTUNE_LOW_VOICE_LAB)
  target_compile_definitions(MetaTuneInspired PRIVATE SKELLTUNE_LOW_VOICE_LAB=1)
  if(SKELLTUNE_LAB_FAST_2P5)
    target_compile_definitions(MetaTuneInspired PRIVATE SKELLTUNE_LAB_FAST_2P5=1)
  endif()
  if(SKELLTUNE_3WAY_AB_LAB)
    target_compile_definitions(MetaTuneInspired PRIVATE SKELLTUNE_3WAY_AB_LAB=1)
  endif()
endif()
target_compile_definitions(MetaTuneInspired PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(MetaTuneInspired PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags
)


# Optional DSP regression suite; no effect on the existing Windows workflow.
option(SKELLTUNE_BUILD_TESTS "Build portable DSP validation targets" OFF)
if(SKELLTUNE_BUILD_TESTS)
  enable_testing()
  file(GLOB SKELLTUNE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/RealtimeAutotune_*.cpp")
  foreach(test_source IN LISTS SKELLTUNE_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable("${test_name}" "${test_source}")
    target_compile_features("${test_name}" PRIVATE cxx_std_17)
    target_include_directories("${test_name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    add_test(NAME "${test_name}" COMMAND "${test_name}")
  endforeach()

  # Original, framework-free LyraTune regression suite from the user ZIP.
  # This verifies source parity and exercises negative speed, note transitions,
  # vibrato, range changes, strict real-time allocation tests and stereo output.
  add_executable(LyraOriginalDSPTests
    tests/LyraDSP/dsp_tests.cpp
    Source/LyraDSP/Pitch/TargetSelector.cpp
    Source/LyraDSP/Pitch/YinDetector.cpp
    Source/LyraDSP/Pitch/PitchShifter.cpp
    Source/LyraDSP/Pitch/CorrectionEngine.cpp
    Source/LyraDSP/TunerChain.cpp
    Source/LyraDSP/Doubler.cpp
    Source/LyraDSP/FormantAccent.cpp)
  target_include_directories(LyraOriginalDSPTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/LyraDSP")
  target_compile_features(LyraOriginalDSPTests PRIVATE cxx_std_20)
  if(SKELLTUNE_LOW_VOICE_LAB)
    target_compile_definitions(LyraOriginalDSPTests PRIVATE SKELLTUNE_LOW_VOICE_LAB=1)
    if(SKELLTUNE_LAB_FAST_2P5)
      target_compile_definitions(LyraOriginalDSPTests PRIVATE SKELLTUNE_LAB_FAST_2P5=1)
    endif()
    if(SKELLTUNE_3WAY_AB_LAB)
      target_compile_definitions(LyraOriginalDSPTests PRIVATE SKELLTUNE_3WAY_AB_LAB=1)
    endif()
  endif()
  add_test(NAME LyraOriginalDSPTests COMMAND LyraOriginalDSPTests)

  # Actual MetaTune settings adapter, not merely Lyra's internal Params.
  # Run against the same shifter/controller objects as the VST3 audio path.
  add_executable(MetaTuneLyraAdapterSmoke
    tests/LyraDSP/metatune_adapter_smoke.cpp
    Source/LyraDSP/Pitch/TargetSelector.cpp
    Source/LyraDSP/Pitch/YinDetector.cpp
    Source/LyraDSP/Pitch/PitchShifter.cpp
    Source/LyraDSP/Pitch/CorrectionEngine.cpp
    Source/LyraDSP/TunerChain.cpp
    Source/LyraDSP/Doubler.cpp
    Source/LyraDSP/FormantAccent.cpp)
  target_include_directories(MetaTuneLyraAdapterSmoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source")
  target_compile_features(MetaTuneLyraAdapterSmoke PRIVATE cxx_std_17)
  if(SKELLTUNE_LOW_VOICE_LAB)
    target_compile_definitions(MetaTuneLyraAdapterSmoke PRIVATE SKELLTUNE_LOW_VOICE_LAB=1)
    if(SKELLTUNE_LAB_FAST_2P5)
      target_compile_definitions(MetaTuneLyraAdapterSmoke PRIVATE SKELLTUNE_LAB_FAST_2P5=1)
    endif()
    if(SKELLTUNE_3WAY_AB_LAB)
      target_compile_definitions(MetaTuneLyraAdapterSmoke PRIVATE SKELLTUNE_3WAY_AB_LAB=1)
    endif()
  endif()
  add_test(NAME MetaTuneLyraAdapterSmoke COMMAND MetaTuneLyraAdapterSmoke)
endif()

# This target is compiled and executed only by the Windows GitHub Actions job.
# It loads the produced VST3 through JUCE's VST3PluginFormat (not the source
# processor class), so basic Windows binary, scanning and state bugs surface.
option(SKELLTUNE_BUILD_WINDOWS_HOST_SMOKE "Build JUCE VST3 host smoke executable" OFF)
if(WIN32 AND SKELLTUNE_BUILD_WINDOWS_HOST_SMOKE)
  juce_add_console_app(SkellyVST3Smoke PRODUCT_NAME "Skelly VST3 Smoke")
  juce_generate_juce_header(SkellyVST3Smoke)
  target_sources(SkellyVST3Smoke PRIVATE tools/windows_vst3_smoke.cpp)
  # The VST3 host wrapper is a native child window, so its JUCE component
  # surface can legitimately rasterize black in headless Windows CI. Link
  # the exact same plugin shared-code target for independent software UI
  # rendering, while retaining actual VST3 scan/process/PDC tests.
  target_compile_features(SkellyVST3Smoke PRIVATE cxx_std_17)
  if(SKELLTUNE_LAB_FAST_2P5)
    target_compile_definitions(SkellyVST3Smoke PRIVATE SKELLTUNE_LAB_FAST_2P5=1)
  endif()
  if(SKELLTUNE_3WAY_AB_LAB)
    target_compile_definitions(SkellyVST3Smoke PRIVATE SKELLTUNE_3WAY_AB_LAB=1)
  endif()
  target_compile_definitions(SkellyVST3Smoke PRIVATE
      JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(SkellyVST3Smoke PRIVATE
      MetaTuneInspired juce::juce_audio_processors juce::juce_audio_utils
      juce::juce_recommended_config_flags)
endif()
