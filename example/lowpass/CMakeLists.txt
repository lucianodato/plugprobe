# GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
# Test-fixture plugin (not a product): proves plugprobe end-to-end in CI.
juce_add_plugin(ProbeLowpass
    VERSION 1.0.0
    COMPANY_NAME "plugprobe"
    PLUGIN_MANUFACTURER_CODE PlPb
    PLUGIN_CODE Pbl1
    FORMATS VST3
    PRODUCT_NAME "Probe Lowpass"
    PLUGIN_IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE)
target_sources(ProbeLowpass PRIVATE
    LowpassProcessor.h LowpassProcessor.cpp)
target_link_libraries(ProbeLowpass PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_gui_basics)
target_compile_definitions(ProbeLowpass PRIVATE JUCE_VST3_CAN_REPLACE_VST2=0)
