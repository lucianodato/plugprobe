// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// capture.h: OS-independent capture pipeline. Each OS supplies only
// plugprobeGrabFrame (pixels); PNG shots and AVI video are written here, so
// the output is identical on every platform.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "plugprobe_os.h"

namespace pp {
// 1 ok / 0 fail / -1 blank (uniform grab: GPU or async-painted editor).
int saveShotPng(void* hv, const char* path, int* w, int* h);

struct VideoRec {
  void* hv = nullptr;
  juce::int64 lastFrame = -1;
  int w = 0, h = 0;                            // locked to the first frame
  std::vector<juce::int64> samplePositions;
  std::vector<std::vector<unsigned char>> jpegs;  // one JPEG per grabbed frame
};
std::unique_ptr<VideoRec> videoStart(void* hv);
// Capture by audio position so host/capture overhead cannot lengthen the AVI.
void videoGrab(VideoRec& r, juce::int64 samplePos, double sampleRate);
// Writes an AVI (MJPEG video + the take as PCM audio) to `out`.
bool videoFinish(VideoRec& r, const juce::File& take, const juce::File& out,
                 juce::String& err);
}  // namespace pp
