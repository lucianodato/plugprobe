// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// session.h: M2 live-session state — agent-paced, file-backed.
// No daemon, no RT thread: `start` snapshots the loop + params to a session
// dir, `act` appends timestamped events, `stop` replays them as one `render`
// timeline (re-exec of this binary, so clicks run the real macOS GUI engine).
// Timing is logical (atMs), not wall-clock; realtime looping + monitor
// mirror are the documented ceiling (ponytail: daemon + lock-free taps if
// agents ever need true realtime).
#pragma once

#include "core.h"

namespace pp {
juce::File resolveSessionFile(const juce::String& s);
bool loadSession(const juce::String& s, juce::var& out, juce::String& err);
juce::String newSessionDir();
// Params-only headless render of a loop buffer (bypassDesc=null copies).
bool renderLoopOffline(const juce::PluginDescription* desc,
                       const std::vector<std::vector<float>>& inCh, double sr,
                       int block, const juce::var& baseParams,
                       std::vector<std::vector<float>>& outCh,
                       juce::String& err);
juce::var mergeParams(const juce::var& oldP, const juce::var& add);
// Meters over ch[from,from+n): peak/rms/lufs-proxy/crest + 16 log spectrum bins.
void metersOf(const std::vector<std::vector<float>>& ch, double sr,
              size_t from, size_t n, juce::DynamicObject* o);
}  // namespace pp
