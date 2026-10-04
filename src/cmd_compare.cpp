// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_compare.cpp: `compare` command. See cmds.h.
#include "cmds.h"
#include "gui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {
// ponytail: RMS-diff stands in for LUFS-diff (calibration cancels in deltas);
// upgrade to K-weighted ITU-R BS.1770 if absolute loudness is ever asserted.
void compareBufs(const std::vector<float>& a, const std::vector<float>& b,
                 double sr, juce::DynamicObject* o, double t0 = 0,
                 double t1 = -1) {
  size_t n0 = (size_t)clampd(t0 * sr, 0, (double)a.size());
  size_t n1 = t1 < 0 ? a.size()
                     : (size_t)clampd(t1 * sr, 0, (double)a.size());
  size_t m = std::min(a.size(), b.size());
  m = n1 > n0 ? std::min(m, n1) : m;
  if (m <= n0 + 8) {
    o->setProperty("nullDb", -120.0);
    o->setProperty("lufsDiff", 0.0);
    o->setProperty("spectralDist", 0.0);
    o->setProperty("sdrDb", 120.0);
    o->setProperty("artifactDb", -120.0);
    o->setProperty("costDelta", 0.0);
    return;
  }
  double ea = 0, eb = 0, er = 0, dot = 0;
  for (size_t i = n0; i < m; ++i) {
    ea += (double)a[i] * a[i];
    eb += (double)b[i] * b[i];
    double r = a[i] - b[i];
    er += r * r;
    dot += (double)a[i] * b[i];
  }
  double rmsA = std::sqrt(ea / (m - n0)), rmsB = std::sqrt(eb / (m - n0));
  double nullDb =
      er <= 0 ? -120.0 : std::max(-120.0, 10 * std::log10(er / std::max(ea, 1e-18)));
  double sdr = er <= 0 ? 120.0 : std::min(120.0, 10 * std::log10(ea / er));
  double g = ea > 0 ? dot / ea : 1.0, eart = 0, bandAcc = 0;
  // ponytail: O(n) 3kHz-centred band proxy over stride; full STFT if saturates
  for (size_t i = n0; i < m; i += 4) {
    double t = (double)i / sr;
    double res = (double)a[i] - g * b[i];
    eart += res * res;
    bandAcc += res * std::cos(2 * M_PI * 3000.0 * t);
  }
  double specDist = 0;
  {
    juce::dsp::FFT fft(12);  // 4096
    std::vector<float> wa(8192, 0), wb(8192, 0);
    int wins = 0;
    for (size_t st = n0; st + 4096 < m; st += 2048) {
      for (int i = 0; i < 4096; ++i) {
        float w = 0.5f - 0.5f * std::cos(2 * M_PI * i / 4096);
        wa[(size_t)i] = a[st + (size_t)i] * w;
        wb[(size_t)i] = b[st + (size_t)i] * w;
      }
      std::fill(wa.begin() + 4096, wa.end(), 0);
      std::fill(wb.begin() + 4096, wb.end(), 0);
      fft.performFrequencyOnlyForwardTransform(wa.data());
      fft.performFrequencyOnlyForwardTransform(wb.data());
      double d = 0;
      for (int k = 1; k < 2048; ++k) {
        double la = std::log10(wa[(size_t)k] + 1e-9),
               lb = std::log10(wb[(size_t)k] + 1e-9);
        d += (la - lb) * (la - lb);
      }
      specDist += std::sqrt(d / 2047);
      ++wins;
    }
    specDist = wins > 0 ? specDist / wins : 0;
  }
  o->setProperty("nullDb", nullDb);
  o->setProperty("lufsDiff", toDb(rmsB) - toDb(rmsA));
  o->setProperty("spectralDist", specDist);
  o->setProperty("sdrDb", sdr);
  o->setProperty("artifactDb",
                 std::max(-120.0, 10 * std::log10(eart / std::max(ea, 1e-18))));
  o->setProperty("costDelta",
                 clampd(toDb(std::abs(bandAcc) + 1e-12) - toDb(rmsA + 1e-12),
                        -60.0, 60.0));
}

int runCompare(const juce::var& args)
{
    juce::String aP = jstr(args, "a"), bP = jstr(args, "b");
    if (aP.isEmpty() || bP.isEmpty()) {
      emitErr(errObj("ARGS", "compare needs {a,b}"));
      return 1;
    }
    std::vector<std::vector<float>> ca, cb;
    double sa = 0, sb = 0;
    if (!readWav(aP, ca, sa) || !readWav(bP, cb, sb) || ca.empty() ||
        cb.empty()) {
      emitErr(errObj("IO", "cannot read a/b wavs"));
      return 1;
    }
    double sr = std::min(sa, sb) > 0 ? std::min(sa, sb) : kDefSr;
    auto* o = new juce::DynamicObject();
    compareBufs(ca[0], cb[0], sr, o);
    if (args.hasProperty("slices")) {
      juce::Array<juce::var> arr;
      auto bounds = args["slices"];
      const juce::Array<juce::var>* ev =
          bounds.isArray() ? bounds.getArray()
                           : (bounds.hasProperty("events") &&
                                      bounds["events"].isArray()
                                  ? bounds["events"].getArray()
                                  : nullptr);
      std::vector<double> cuts{0};
      if (ev)
        for (auto& e : *ev) cuts.push_back((double)e["atMs"] / 1000.0);
      cuts.push_back((double)std::max(ca[0].size(), cb[0].size()) / sr);
      for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        auto* s = new juce::DynamicObject();
        compareBufs(ca[0], cb[0], sr, s, cuts[i], cuts[i + 1]);
        s->setProperty("fromS", cuts[i]);
        s->setProperty("toS", cuts[i + 1]);
        arr.add(juce::var(s));
      }
      o->setProperty("slices", juce::var(arr));
    }
    emitOk(juce::var(o));
    return 0;
}
}  // namespace pp
