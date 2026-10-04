// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// Test-fixture plugin: one-pole-ish smooth lowpass + spectrum display.
// Exists so CI can prove plugprobe end-to-end (params move the cutoff,
// the FFT + video show it). Linear 20..20000Hz range keeps agent-side
// 0..1 mapping trivial: norm = (hz - 20) / 19980.
#pragma once

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

class LowpassProcessor : public juce::AudioProcessor {
 public:
  static constexpr int fftOrder = 11, fftSize = 1 << fftOrder;

  LowpassProcessor();
  ~LowpassProcessor() override = default;

  void prepareToPlay(double sr, int block) override;
  void releaseResources() override {}
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return "Probe Lowpass"; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  double getTailLengthSeconds() const override { return 0.1; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return "Default"; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock&) override {}
  void setStateInformation(const void*, int) override {}

  // Latest 1024-bin linear magnitudes (ch 0); drained by the editor.
  bool spectrumIfFresh(std::array<float, 1024>& out);
  juce::AudioParameterFloat* cutoffParam() const { return cutoff; }

 private:
  juce::AudioParameterFloat* cutoff = nullptr;
  juce::dsp::StateVariableTPTFilter<float> filter;
  juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smooth;
  juce::dsp::FFT fft{fftOrder};
  std::array<float, fftSize * 2> fifo{};
  int fifoPos = 0;
  std::array<float, 1024> spectrum{};
  std::atomic<bool> fresh{false};
};
