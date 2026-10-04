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
  float tr = trim.load();  // param-less: stepped per block, fine for a fixture
  int n = io.getNumSamples(), nCh = io.getNumChannels();
  for (int i = 0; i < n; ++i) {
    filter.setCutoffFrequency(smooth.getNextValue());
    for (int c = 0; c < nCh; ++c) {
      float x = io.getSample(c, i);
      float y = filter.processSample(c, x) * tr;
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

// --- Editor: family look and feel (self-contained mini-LNF mirroring
// noise-repellent's palette + slider rendering; no cross-repo coupling) ---
namespace {
// Palette mirrors NoiseRepellentLookAndFeel.
const juce::Colour kPanelBg(0xff343a48), kPanelBorder(0xff4f586c);
const juce::Colour kDisplayBg(0xff232832), kGrid(0xff3d4657);
const juce::Colour kGridLabel(0xffa8b3c4), kLegend(0xffd8e0ec);
const juce::Colour kAqua(0xff5cc0d4), kAmber(0xffe5a000);
const juce::Colour kThumb(0xff525c70), kThumbEdge(0xff8492a8);
const juce::Colour kTrack(0xff1c2028), kFooter(0xff94a3b8);
float xForFreq(float hz, float w) {
  float t = std::log(hz / 20.f) / std::log(20000.f / 20.f);
  return 8 + t * (w - 16);
}
juce::String hzText(float hz) {
  return hz >= 1000.f ? juce::String(hz / 1000.f, 1) + " kHz"
                      : juce::String((int)hz) + " Hz";
}
class FixtureLookAndFeel : public juce::LookAndFeel_V4 {
 public:
  FixtureLookAndFeel() {
    setColour(juce::Slider::thumbColourId, kThumb);
    setColour(juce::Slider::trackColourId, kTrack);
  }
  // Same horizontal rendering as the family LNF: dark rounded track,
  // accent fill, capped thumb with edge + tick.
  void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                        float sliderPos, float, float,
                        juce::Slider::SliderStyle,
                        juce::Slider& slider) override {
    juce::Colour fill =
        slider.findColour(juce::Slider::rotarySliderFillColourId);
    if (fill.isTransparent()) fill = kAmber;
    float trackH = 4.0f, trackY = y + (height - trackH) * 0.5f;
    g.setColour(kTrack);
    g.fillRoundedRectangle((float)x, trackY, (float)width, trackH, 2.0f);
    g.setColour(fill);
    g.fillRoundedRectangle((float)x, trackY, sliderPos - (float)x, trackH, 2.0f);
    float thumbW = 12.0f, thumbH = 16.0f;
    float thumbX = sliderPos - thumbW * 0.5f;
    float thumbY = y + (height - thumbH) * 0.5f;
    g.setColour(kThumb);
    g.fillRoundedRectangle(thumbX, thumbY, thumbW, thumbH, 2.0f);
    g.setColour(kThumbEdge);
    g.drawRoundedRectangle(thumbX, thumbY, thumbW, thumbH, 2.0f, 1.0f);
    g.setColour(fill);
    g.fillRect(thumbX + 5.0f, thumbY + 4.0f, 2.0f, 8.0f);
  }
};
class Spectrum : public juce::Component, private juce::Timer {
 public:
  explicit Spectrum(LowpassProcessor& p) : proc(p) { startTimerHz(30); }
  void paint(juce::Graphics& g) override {
    auto r = getLocalBounds().toFloat();
    float w = r.getWidth(), h = r.getHeight();
    g.fillAll(kDisplayBg);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    for (int db = -90; db <= -20; db += 10) {  // dB grid
      float y = h - (db + 100) / 80 * h;
      g.setColour(kGrid);
      g.drawHorizontalLine((int)y, 0.0f, w);
      g.setColour(kGridLabel);
      g.drawText(juce::String(db) + " dB", 8, (int)y - 12, 60, 12,
                 juce::Justification::left);
    }
    for (float f : {50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f,
                    20000.f}) {  // log freq grid
      float x = xForFreq(f, w);
      g.setColour(kGrid);
      g.drawVerticalLine((int)x, 0.0f, h);
      g.setColour(kGridLabel);
      juce::String label = f >= 1000.f ? juce::String(f / 1000.f, 0) + "k"
                                       : juce::String((int)f);
      g.drawText(label, (int)x + 4, (int)h - 14, 40, 12,
                 juce::Justification::left);
    }
    juce::Path curve;  // output spectrum, aqua with alpha fill
    bool started = false;
    float sr = proc.getSampleRate() > 0 ? (float)proc.getSampleRate() : 48000.f;
    for (int k = 1; k < 1024; ++k) {
      float hz = k * sr / 2048.f;
      if (hz < 20 || hz > 20000) continue;
      float db = 20 * std::log10(bins[(size_t)k] + 1e-6f);
      float y = h - (juce::jlimit(-100.f, -20.f, db) + 100) / 80 * h;
      float x = xForFreq(hz, w);
      if (!started) {
        curve.startNewSubPath(x, y);
        started = true;
      } else {
        curve.lineTo(x, y);
      }
    }
    if (started) {
      juce::Path fill = curve;
      fill.lineTo(xForFreq(20000.f, w), h);
      fill.lineTo(xForFreq(20.f, w), h);
      fill.closeSubPath();
      g.setColour(kAqua.withAlpha(0.25f));
      g.fillPath(fill);
      g.setColour(kAqua);
      g.strokePath(curve, juce::PathStrokeType(1.5f));
      float cx = xForFreq(proc.cutoffParam()->get(), w);  // cutoff marker
      g.setColour(kAmber);
      g.drawVerticalLine((int)cx, 0.0f, h);
    }
  }

  void timerCallback() override {
    if (proc.spectrumIfFresh(raw)) {
      for (size_t k = 0; k < bins.size(); ++k)  // gentle temporal smoothing
        bins[k] += 0.5f * (raw[k] - bins[k]);
      repaint();
    }
  }

 private:
  LowpassProcessor& proc;
  std::array<float, 1024> raw{}, bins{};
};
class LowpassEditor : public juce::AudioProcessorEditor, private juce::Timer {
 public:
  explicit LowpassEditor(LowpassProcessor& p)
      : AudioProcessorEditor(p), proc(p), spec(p) {
    brand.setText("PROBE LOWPASS", juce::dontSendNotification);
    brand.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    brand.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(brand);
    addAndMakeVisible(spec);
    cutoff.setSliderStyle(juce::Slider::LinearHorizontal);
    cutoff.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    cutoff.setRange(20, 20000);
    cutoff.setName("Cutoff");  // stable AX node id (AXSlider:Cutoff)
    cutoff.setTitle("Cutoff");
    cutoff.setLookAndFeel(&lnf);
    cutoff.setColour(juce::Slider::rotarySliderFillColourId, kAmber);
    cutoff.onValueChange = [this] {
      proc.cutoffParam()->setValueNotifyingHost(
          proc.cutoffParam()->convertTo0to1((float)cutoff.getValue()));
    };
    addAndMakeVisible(cutoff);
    cap.setText("CUTOFF", juce::dontSendNotification);
    cap.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    cap.setColour(juce::Label::textColourId, kLegend);
    addAndMakeVisible(cap);
    val.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    val.setColour(juce::Label::textColourId, kLegend);
    val.setJustificationType(juce::Justification::right);
    addAndMakeVisible(val);
    tcap.setText("TRIM", juce::dontSendNotification);
    tcap.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    tcap.setColour(juce::Label::textColourId, kLegend);
    addAndMakeVisible(tcap);
    tval.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    tval.setColour(juce::Label::textColourId, kLegend);
    tval.setJustificationType(juce::Justification::right);
    addAndMakeVisible(tval);
    trimSl.setSliderStyle(juce::Slider::LinearHorizontal);
    trimSl.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    trimSl.setRange(0, 2);
    trimSl.setValue(1.0, juce::dontSendNotification);
    trimSl.setName("Trim");  // stable AX node id (AXSlider:Trim)
    trimSl.setTitle("Trim");
    trimSl.setLookAndFeel(&lnf);
    trimSl.setColour(juce::Slider::rotarySliderFillColourId, kAmber);
    trimSl.onValueChange = [this] {
      proc.trim.store((float)trimSl.getValue());
    };
    addAndMakeVisible(trimSl);
    tval.setText("1.00x", juce::dontSendNotification);
    val.setText(hzText(20000.f), juce::dontSendNotification);
    foot.setText("cutoff sweep test fixture", juce::dontSendNotification);
    foot.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    foot.setColour(juce::Label::textColourId, kFooter);
    addAndMakeVisible(foot);
    startTimerHz(30);  // reflect automated (param-plane) moves on screen
    setSize(480, 390);
  }
  ~LowpassEditor() override {
    cutoff.setLookAndFeel(nullptr);
    trimSl.setLookAndFeel(nullptr);
  }
  void paint(juce::Graphics& g) override {
    g.fillAll(kPanelBg);
    g.setColour(kPanelBorder);
    g.drawHorizontalLine(44, 0.0f, (float)getWidth());
    g.drawHorizontalLine(getHeight() - 25, 0.0f, (float)getWidth());
  }
  void timerCallback() override {
    float hz = proc.cutoffParam()->get();
    if (std::abs(hz - (float)cutoff.getValue()) > 1) {
      cutoff.setValue(hz, juce::dontSendNotification);
      val.setText(hzText(hz), juce::dontSendNotification);
    }
    float tr = proc.trim.load();
    if (std::abs(tr - (float)trimSl.getValue()) > 0.001) {
      trimSl.setValue(tr, juce::dontSendNotification);
      tval.setText(juce::String(tr, 2) + "x", juce::dontSendNotification);
    }
  }
  void resized() override {
    brand.setBounds(12, 8, 300, 28);
    spec.setBounds(10, 50, getWidth() - 20, 200);
    cap.setBounds(12, 258, 70, 20);
    val.setBounds(getWidth() - 112, 258, 100, 20);
    cutoff.setBounds(86, 282, getWidth() - 98, 24);
    tcap.setBounds(12, 310, 70, 20);
    tval.setBounds(getWidth() - 112, 310, 100, 20);
    trimSl.setBounds(86, 334, getWidth() - 98, 24);
    foot.setBounds(12, getHeight() - 24, getWidth() - 24, 20);
  }

 private:
  LowpassProcessor& proc;
  FixtureLookAndFeel lnf;
  Spectrum spec;
  juce::Label brand, cap, val, tcap, tval, foot;
  juce::Slider cutoff, trimSl;
};
}  // namespace

juce::AudioProcessorEditor* LowpassProcessor::createEditor() {
  return new LowpassEditor(*this);
}

// JUCE entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new LowpassProcessor();
}
