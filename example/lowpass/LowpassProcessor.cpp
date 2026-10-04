// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
#include "LowpassProcessor.h"

LowpassProcessor::LowpassProcessor()
    : AudioProcessor(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())) {
  addParameter(cutoff = new juce::AudioParameterFloat(
                   "cutoff", "Cutoff", juce::NormalisableRange<float>(20.f, 20000.f), 20000.f, "Hz"));
}

void LowpassProcessor::prepareToPlay(double sr, int block) {
  juce::dsp::ProcessSpec spec{sr, (juce::uint32)block, 2};
  filter.prepare(spec);
  filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
  filter.setResonance(0.7071f);
  smooth.reset(sr, 0.05);  // zipper-free sweeps under test automation
  smooth.setCurrentAndTargetValue(cutoff->get());
  fifo.fill(0);
  fifoPos = 0;
}

void LowpassProcessor::processBlock(juce::AudioBuffer<float>& io, juce::MidiBuffer&) {
  juce::ScopedNoDenormals noDenormals;
  smooth.setTargetValue(cutoff->get());
  int n = io.getNumSamples(), nCh = io.getNumChannels();
  for (int i = 0; i < n; ++i) {
    filter.setCutoffFrequency(smooth.getNextValue());
    for (int c = 0; c < nCh; ++c) {
      float x = io.getSample(c, i);
      float y = filter.processSample(c, x);
      io.setSample(c, i, y);
      if (c == 0 && fifoPos < fftSize) fifo[(size_t)fifoPos++] = y;
    }
    if (fifoPos >= fftSize) {
      std::fill(fifo.begin() + fftSize, fifo.end(), 0);
      fft.performFrequencyOnlyForwardTransform(fifo.data());
      for (int k = 0; k < 1024; ++k) spectrum[(size_t)k] = fifo[(size_t)k];
      fresh.store(true);
      fifoPos = 0;
    }
  }
}

bool LowpassProcessor::spectrumIfFresh(std::array<float, 1024>& out) {
  if (!fresh.exchange(false)) return false;
  out = spectrum;  // torn float = one glitchy pixel; fine for a fixture
  return true;
}

// --- Editor: cutoff slider + spectrum with cutoff marker ---
namespace {
float xForFreq(float hz, float w) {
  float t = std::log(hz / 20.f) / std::log(20000.f / 20.f);
  return 8 + t * (w - 16);
}
class Spectrum : public juce::Component, private juce::Timer {
 public:
  explicit Spectrum(LowpassProcessor& p) : proc(p) { startTimerHz(30); }
  void timerCallback() override {
    if (proc.spectrumIfFresh(bins)) repaint();
  }
  void paint(juce::Graphics& g) override {
    auto r = getLocalBounds().toFloat();
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::darkgrey);
    for (float hz : {100.f, 1000.f, 10000.f}) {
      float x = xForFreq(hz, r.getWidth());
      g.drawVerticalLine((int)x, 0, r.getHeight());
    }
    juce::Path path;
    bool started = false;
    for (int k = 1; k < 1024; ++k) {
      float hz = k * 48000.f / 4096.f;  // display scale; sr-exact not needed
      if (hz < 20 || hz > 20000) continue;
      float db = 20 * std::log10(bins[(size_t)k] + 1e-6f);
      float y = r.getHeight() - (juce::jlimit(-90.f, 0.f, db) + 90) / 90 * r.getHeight();
      float x = xForFreq(hz, r.getWidth());
      if (!started) {
        path.startNewSubPath(x, y);
        started = true;
      } else {
        path.lineTo(x, y);
      }
    }
    g.setColour(juce::Colours::lime);
    g.strokePath(path, juce::PathStrokeType(1.5f));
    float cx = xForFreq(proc.cutoffParam()->get(), r.getWidth());
    g.setColour(juce::Colours::white);
    g.drawVerticalLine((int)cx, 0, r.getHeight());
  }

 private:
  LowpassProcessor& proc;
  std::array<float, 1024> bins{};
};
class LowpassEditor : public juce::AudioProcessorEditor, private juce::Timer {
 public:
  explicit LowpassEditor(LowpassProcessor& p)
      : AudioProcessorEditor(p), proc(p), spec(p) {
    addAndMakeVisible(spec);
    cutoff.setRange(20, 20000);
    cutoff.setTextValueSuffix(" Hz");
    cutoff.onValueChange = [this] {
      proc.cutoffParam()->setValueNotifyingHost(
          proc.cutoffParam()->convertTo0to1((float)cutoff.getValue()));
    };
    addAndMakeVisible(cutoff);
    addAndMakeVisible(label);
    label.setText("Cutoff", juce::dontSendNotification);
    label.attachToComponent(&cutoff, true);
    startTimerHz(30);  // reflect automated (param-plane) moves on screen
    setSize(420, 320);
  }
  void timerCallback() override {
    float hz = proc.cutoffParam()->get();
    if (std::abs(hz - (float)cutoff.getValue()) > 1)
      cutoff.setValue(hz, juce::dontSendNotification);
  }
  void resized() override {
    spec.setBounds(0, 0, getWidth(), 220);
    cutoff.setBounds(80, 250, getWidth() - 100, 40);
  }

 private:
  LowpassProcessor& proc;
  Spectrum spec;
  juce::Slider cutoff{juce::Slider::LinearHorizontal, juce::Slider::TextBoxBelow};
  juce::Label label;
};
}  // namespace

juce::AudioProcessorEditor* LowpassProcessor::createEditor() {
  return new LowpassEditor(*this);
}

// JUCE entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new LowpassProcessor();
}
