// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// gui.h: live-editor helpers — GUI-backed instances, window open/hold,
// screenshots, AX hit-testing. Used by snapshot/act/render.
#pragma once

#include "core.h"
#include "plugprobe_os.h"

namespace pp {
bool guiCapable(const juce::PluginDescription& desc);
std::unique_ptr<juce::AudioPluginInstance> guiCreate(
    const juce::PluginDescription& desc, double sr, int block, juce::String& e);
juce::AudioProcessorEditor* openEditor(juce::AudioPluginInstance& gui,
                                       bool onscreen, juce::String& why);
void holdUi(int holdMs);
#if JUCE_MAC
bool noteCapture(int rc, const juce::String& path, juce::String& saved,
                 juce::String& why);
#endif
juce::String saveEditorShot(const juce::PluginDescription& desc, double sr,
                            int block, const juce::String& path,
                            juce::String& shotWhy, juce::String& visWhy,
                            bool visible, int holdMs);
juce::var hitNodeAt(void* hv, double x, double y);
#if JUCE_MAC
// Poll the AX tree until a node id appears (or timeout); see gui.cpp.
bool axWaitForId(void* hv, const juce::String& nodeId, int timeoutMs);
#endif
}  // namespace pp
