cmake_minimum_required(VERSION 3.22)
project(MetaTuneInspired VERSION 0.1.0)

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
