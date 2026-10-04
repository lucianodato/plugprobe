// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// session.cpp: file-backed M2 session helpers. See session.h.
#include "session.h"

#include <algorithm>
#include <cmath>

namespace pp {
juce::File resolveSessionFile(const juce::String& s) {
  juce::File f(s);
  if (f.isDirectory()) f = f.getChildFile("session.json");
  return f;
}

bool loadSession(const juce::String& s, juce::var& out, juce::String& err) {
  juce::File f = resolveSessionFile(s);
  juce::var v;
  if (!juce::JSON::parse(f.loadFileAsString(), v) || !v.isObject()) {
    err = "unknown session: " + s;
    return false;
  }
  if (!v.hasProperty("plugin") && !(bool)v["bypass"]) {
    err = "corrupt session file: " + f.getFullPathName();
    return false;
  }
  out = v;
  return true;
}

juce::String newSessionDir() {
  juce::File d = juce::File::getSpecialLocation(juce::File::tempDirectory)
                     .getChildFile("plugprobe-sessions")
                     .getChildFile(juce::Uuid().toString());
  d.createDirectory();
  return d.getFullPathName();
}

bool renderLoopOffline(const juce::PluginDescription* desc,
                       const std::vector<std::vector<float>>& inCh, double sr,
                       int block, const juce::var& baseParams,
                       std::vector<std::vector<float>>& outCh,
                       juce::String& err) {
  if (desc == nullptr || inCh.empty()) {
    outCh = inCh;
    return true;
  }
  juce::String e;
  auto inst = instantiate(*desc, sr, block, e);
  if (inst == nullptr) {
    err = e;
    return false;
  }
  if (baseParams.isObject()) applyParams(*inst, baseParams);
  inst->setRateAndBufferSizeDetails(sr, block);
  inst->prepareToPlay(sr, block);
  size_t n = inCh[0].size();
  int nCh = std::max(1, (int)inCh.size());
  outCh.assign((size_t)nCh, std::vector<float>(n, 0));
  juce::AudioBuffer<float> blk(std::max(2, nCh), block);
  juce::MidiBuffer midi;
  for (size_t pos = 0; pos < n; pos += (size_t)block) {
    int m = (int)std::min((size_t)block, n - pos);
    blk.clear();
    for (int c = 0; c < blk.getNumChannels(); ++c) {
      float* w = blk.getWritePointer(c);
      const auto& src = inCh[(size_t)c % inCh.size()];
      for (int i = 0; i < m; ++i) w[i] = src[pos + (size_t)i];
    }
    midi.clear();
    inst->processBlock(blk, midi);
    for (int c = 0; c < nCh; ++c) {
      const float* r = blk.getReadPointer(c % blk.getNumChannels());
      for (int i = 0; i < m; ++i) outCh[(size_t)c][pos + (size_t)i] = r[i];
    }
  }
  inst->releaseResources();
  return true;
}

juce::var mergeParams(const juce::var& oldP, const juce::var& add) {
  auto* m = new juce::DynamicObject();
  if (auto* o = oldP.getDynamicObject())
    for (auto& kv : o->getProperties()) m->setProperty(kv.name, kv.value);
  if (auto* o = add.getDynamicObject())
    for (auto& kv : o->getProperties()) m->setProperty(kv.name, kv.value);
  return juce::var(m);
}

void metersOf(const std::vector<std::vector<float>>& ch, double sr,
              size_t from, size_t n, juce::DynamicObject* o) {
  if (ch.empty() || ch[0].empty() || n < 8) {
    o->setProperty("peakDb", -120.0);
    o->setProperty("rmsDb", -120.0);
    o->setProperty("lufsM", -120.0);
    o->setProperty("crestDb", 0.0);
    o->setProperty("spectrum", juce::var(juce::Array<juce::var>()));
    return;
  }
  size_t end = std::min(from + n, ch[0].size());
  from = std::min(from, ch[0].size());
  double esum = 0;
  size_t cnt = 0;
  float peak = 0;
  for (auto& c : ch)
    for (size_t i = from; i < end; ++i) {
      peak = std::max(peak, std::abs(c[i]));
      esum += (double)c[i] * c[i];
      ++cnt;
    }
  double rms = cnt > 0 ? std::sqrt(esum / cnt) : 0;
  double peakDb = peak <= 0 ? -120.0 : 20.0 * std::log10(peak);
  double rmsDb = toDb(rms);
  o->setProperty("peakDb", peakDb);
  o->setProperty("rmsDb", rmsDb);
  o->setProperty("lufsM", rmsDb);  // RMS proxy, same convention as compare
  o->setProperty("lufsNote", "rms-proxy");
  o->setProperty("crestDb", peakDb - rmsDb);
  // 16 log-spaced bins, 20Hz..20kHz, from a 4096 Hann FFT of ch 0.
  juce::Array<juce::var> bins;
  size_t avail = end > from ? end - from : 0;
  if (avail >= 256) {
    juce::dsp::FFT fft(12);
    std::vector<float> w(8192, 0);
    size_t f0 = end - std::min(avail, (size_t)4096);
    for (size_t i = 0; i < 4096 && f0 + i < end; ++i) {
      float hann = 0.5f - 0.5f * std::cos(2 * M_PI * i / 4096);
      w[i] = ch[0][f0 + i] * hann;
    }
    fft.performFrequencyOnlyForwardTransform(w.data());
    for (int b = 0; b < 16; ++b) {
      double fLo = 20.0 * std::pow(1000.0, b / 16.0);
      double fHi = 20.0 * std::pow(1000.0, (b + 1) / 16.0);
      int kLo = std::max(1, (int)(fLo / sr * 4096));
      int kHi = std::min(2047, std::max(kLo, (int)(fHi / sr * 4096)));
      double m = 0;
      for (int k = kLo; k <= kHi; ++k) m = std::max(m, (double)w[(size_t)k]);
      bins.add(toDb(m / 2048.0));
    }
  }
  o->setProperty("spectrum", juce::var(bins));
}
}  // namespace pp
