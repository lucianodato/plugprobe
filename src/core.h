// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// core.h: shared backend — emit, JSON args, plugin hosting, params, WAV,
// metrics. Stateless helpers in namespace pp (function-local statics only).
#pragma once

#include <string>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

namespace pp {
inline constexpr double kDefSr = 48000.0;
inline constexpr int kDefBlock = 512;
juce::var errObj(const char* code, const juce::String& msg);
void quietStdout();
void restoreStdout();
void emitOk(const juce::var& data);
void emitErr(const juce::var& e);
juce::String jstr(const juce::var& v, const char* k, const char* d = "");
double jnum(const juce::var& v, const char* k, double d);
juce::AudioPluginFormatManager& formats();
void scanInto(juce::KnownPluginList& list,
              const std::vector<juce::String>& dirs,
              const juce::String& formatFilter, bool explicitPaths);
juce::String scanCachePath(const juce::var& a);  // args.cache or env
bool loadScanCache(const juce::String& path,
              const std::vector<juce::String>& dirs,
              juce::KnownPluginList& list);
void saveScanCache(const juce::String& path,
              const std::vector<juce::String>& dirs,
              const juce::KnownPluginList& list);
std::vector<juce::String> defaultPaths();
std::vector<juce::String> argPaths(const juce::var& a);
bool findPlugin(const juce::String& q, const std::vector<juce::String>& dirs,
                juce::PluginDescription& desc);
std::unique_ptr<juce::AudioPluginInstance> instantiate(
    const juce::PluginDescription& d, double sr, int block, juce::String& e);
int findParamIndex(juce::AudioPluginInstance& inst, const juce::String& key);
juce::String validParamNames(juce::AudioPluginInstance& inst);
juce::String unknownTargetIn(juce::AudioPluginInstance& inst,
                             const juce::var& map);
juce::var applyParams(juce::AudioPluginInstance& inst, const juce::var& map);
void setBundleProps(juce::DynamicObject* o, const juce::PluginDescription& d);
bool readWav(const juce::String& path, std::vector<std::vector<float>>& ch,
             double& sr);
bool writeWav(const juce::String& path,
              const std::vector<std::vector<float>>& ch, double sr);
juce::String hashHex(const std::vector<std::vector<float>>& ch);
double rmsOf(const std::vector<float>& v);
double peakDbOf(const std::vector<std::vector<float>>& ch);
double toDb(double x);
double clampd(double x, double lo, double hi);
void paceToRealtime(double t0ms, size_t doneFrames, double sr);
void pumpMessages();
}  // namespace pp
