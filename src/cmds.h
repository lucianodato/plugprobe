// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmds.h: one run function per CLI command (defined in cmd_*.cpp).
#pragma once

#include "core.h"

namespace pp {
int runScan(const juce::var& args);
int runInspect(const juce::var& args);
int runRender(const juce::var& args);
int runCompare(const juce::var& args);
int runSnapshot(const juce::var& args);
int runAct(const juce::var& args);
int runSessionStart(const juce::var& args);
int runSessionAct(const juce::var& args);
int runSessionStop(const juce::var& args);
int runMeters(const juce::var& args);
}  // namespace pp
