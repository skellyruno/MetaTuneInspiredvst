cmake_minimum_required(VERSION 3.22)
project(MetaTuneInspired VERSION 1.11.0)

# Configure with -DJUCE_PATH=/path/to/JUCE (JUCE 7.0.8+ recommended).
if(NOT DEFINED JUCE_PATH)
  message(FATAL_ERROR "Set JUCE_PATH to your local JUCE checkout, e.g. -DJUCE_PATH=C:/JUCE")
endif()
add_subdirectory(${JUCE_PATH} JUCE)

juce_add_plugin(MetaTuneInspired
    COMPANY_NAME "IndependentAudio"
    BUNDLE_ID "audio.independentaudio.metatuneinspired"
    PLUGIN_MANUFACTURER_CODE InAu
    PLUGIN_CODE MtIn
    FORMATS VST3 Standalone
    PRODUCT_NAME "MetaTune Inspired"
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
)
target_compile_features(MetaTuneInspired PRIVATE cxx_std_17)
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
endif()

# This target is compiled and executed only by the Windows GitHub Actions job.
# It loads the produced VST3 through JUCE's VST3PluginFormat (not the source
# processor class), so basic Windows binary, scanning and state bugs surface.
option(SKELLTUNE_BUILD_WINDOWS_HOST_SMOKE "Build JUCE VST3 host smoke executable" OFF)
if(WIN32 AND SKELLTUNE_BUILD_WINDOWS_HOST_SMOKE)
  juce_add_console_app(SkellyVST3Smoke PRODUCT_NAME "Skelly VST3 Smoke")
  target_sources(SkellyVST3Smoke PRIVATE tools/windows_vst3_smoke.cpp)
  target_compile_features(SkellyVST3Smoke PRIVATE cxx_std_17)
  target_compile_definitions(SkellyVST3Smoke PRIVATE
      JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(SkellyVST3Smoke PRIVATE
      juce::juce_audio_processors juce::juce_audio_utils
      juce::juce_recommended_config_flags)
endif()
