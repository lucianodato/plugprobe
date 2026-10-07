// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// capture.cpp: shared PNG shots and AVI video on top of plugprobeGrabFrame.
#include "capture.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>

namespace pp {
namespace {
constexpr double kVideoFps = 15.0;  // ponytail: fixed rate, enough to watch a sweep
constexpr float kJpegQuality = 0.7f;

juce::Image toImage(const PlugprobeRgbFrame& f) {
  juce::Image img(juce::Image::RGB, f.w, f.h, false);
  for (int y = 0; y < f.h; ++y)
    for (int x = 0; x < f.w; ++x) {
      const unsigned char* p = f.rgb.data() + ((size_t)y * f.w + x) * 3;
      img.setPixelAt(x, y, juce::Colour(p[0], p[1], p[2]));
    }
  return img;
}

// ponytail: byte-variance heuristic; a real UI carries antialiased edges, so a
// grab with <=4 distinct sampled bytes is not a UI.
bool isBlank(const PlugprobeRgbFrame& f) {
  std::set<unsigned char> seen;
  for (size_t i = 0; i < f.rgb.size(); i += 97) {
    seen.insert(f.rgb[i]);
    if (seen.size() > 4) return false;
  }
  return true;
}

// Little-endian RIFF writer for the AVI container, built in memory.
struct Out {
  std::vector<unsigned char> b;
  void raw(const void* p, size_t n) {
    auto* c = static_cast<const unsigned char*>(p);
    b.insert(b.end(), c, c + n);
  }
  void id(const char* s) { raw(s, 4); }
  void u16(unsigned v) {
    b.push_back(v & 0xff);
    b.push_back((v >> 8) & 0xff);
  }
  void u32(unsigned v) {
    u16(v & 0xffff);
    u16(v >> 16);
  }
  void patch32(size_t pos, unsigned v) {
    for (int i = 0; i < 4; ++i) b[pos + i] = (v >> (8 * i)) & 0xff;
  }
  size_t listBegin(const char* type) {
    id("LIST");
    size_t at = b.size();  // size field; payload (incl. type) follows
    u32(0);
    id(type);
    return at;
  }
  void listEnd(size_t at) { patch32(at, (unsigned)(b.size() - at - 4)); }
  void chunk(const char* fcc, const void* p, size_t n) {
    id(fcc);
    u32((unsigned)n);
    raw(p, n);
    if (n & 1) b.push_back(0);
  }
};
}  // namespace

int saveShotPng(void* hv, const char* path, int* w, int* h) {
  PlugprobeRgbFrame f;
  if (!plugprobeGrabFrame(hv, f)) return 0;
  if (w) *w = f.w;
  if (h) *h = f.h;
  if (isBlank(f)) return -1;
  juce::File file(path);
  file.getParentDirectory().createDirectory();
  juce::FileOutputStream os(file);
  if (!os.openedOk()) return 0;
  os.setPosition(0);
  os.truncate();
  juce::PNGImageFormat png;
  return png.writeImageToStream(toImage(f), os) ? 1 : 0;
}

std::unique_ptr<VideoRec> videoStart(void* hv) {
  auto r = std::make_unique<VideoRec>();
  r->hv = hv;
  return r;
}

void videoGrab(VideoRec& r) {
  double now = juce::Time::getMillisecondCounterHiRes();
  if (now - r.lastMs < 1000.0 / kVideoFps) return;
  r.lastMs = now;
  PlugprobeRgbFrame f;
  if (!plugprobeGrabFrame(r.hv, f)) return;
  if (r.jpegs.empty()) {
    r.w = f.w;
    r.h = f.h;
  } else if (f.w != r.w || f.h != r.h) {
    return;  // resized mid-take: drop the frame, keep the locked size
  }
  juce::MemoryOutputStream mos;
  juce::JPEGImageFormat jpg;
  jpg.setQuality(kJpegQuality);
  jpg.writeImageToStream(toImage(f), mos);
  const auto* p = static_cast<const unsigned char*>(mos.getData());
  r.jpegs.emplace_back(p, p + mos.getDataSize());
}

bool videoFinish(VideoRec& r, const juce::File& take, const juce::File& out,
                 juce::String& err) {
  if (r.jpegs.empty()) {
    err = "no frames captured";
    return false;
  }
  juce::AudioFormatManager fm;
  fm.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(take));
  if (!rd) {
    err = "cannot read take for video audio";
    return false;
  }
  const int ch = std::max(1, (int)rd->numChannels);
  const unsigned sr = (unsigned)std::lround(rd->sampleRate);
  const juce::int64 total = rd->lengthInSamples;
  const unsigned blockAlign = (unsigned)ch * 2;

  // ponytail: whole take in RAM as float, then 16-bit; stream it if long takes matter.
  juce::AudioBuffer<float> all(ch, (int)total);
  rd->read(all.getArrayOfWritePointers(), ch, 0, (int)total);
  std::vector<short> pcm((size_t)total * ch);
  for (int c = 0; c < ch; ++c) {
    const float* s = all.getReadPointer(c);
    for (juce::int64 i = 0; i < total; ++i)
      pcm[(size_t)i * ch + c] = (short)juce::jlimit(
          -32768, 32767, (int)std::lrint(s[i] * 32767.0f));
  }

  // Frame i covers audio [floor(i*sr/fps), floor((i+1)*sr/fps)). Pad with the
  // last grabbed frame so the video runs as long as the take.
  const size_t grabbed = r.jpegs.size();
  const size_t needed = (size_t)std::ceil(total / (double)sr * kVideoFps);
  const size_t nFrames = std::max(grabbed, needed);
  auto audioStart = [&](size_t i) {
    return std::min<juce::int64>(total, (juce::int64)(i * sr / kVideoFps));
  };

  Out o;
  o.id("RIFF");
  size_t riff = o.b.size();
  o.u32(0);
  o.id("AVI ");

  size_t hdrl = o.listBegin("hdrl");
  {
    Out h;  // avih: 56 bytes
    h.u32((unsigned)std::lround(1e6 / kVideoFps));
    h.u32(0);
    h.u32(0);
    h.u32(0x10);  // AVIF_HASINDEX
    h.u32((unsigned)nFrames);
    h.u32(0);
    h.u32(2);
    h.u32(0);
    h.u32((unsigned)r.w);
    h.u32((unsigned)r.h);
    for (int i = 0; i < 4; ++i) h.u32(0);
    o.chunk("avih", h.b.data(), h.b.size());
  }
  {
    size_t s = o.listBegin("strl");
    Out v;  // strh (video): 56 bytes
    v.id("vids");
    v.id("MJPG");
    v.u32(0);
    v.u16(0);
    v.u16(0);
    v.u32(0);
    v.u32(1);
    v.u32((unsigned)kVideoFps);
    v.u32(0);
    v.u32((unsigned)nFrames);
    v.u32(0);
    v.u32(0xffffffff);
    v.u32(0);
    v.u16(0);
    v.u16(0);
    v.u16((unsigned)r.w);
    v.u16((unsigned)r.h);
    o.chunk("strh", v.b.data(), v.b.size());
    Out bi;  // strf: BITMAPINFOHEADER, 40 bytes
    bi.u32(40);
    bi.u32((unsigned)r.w);
    bi.u32((unsigned)r.h);
    bi.u16(1);
    bi.u16(24);
    bi.id("MJPG");
    bi.u32((unsigned)r.w * r.h * 3);
    for (int i = 0; i < 4; ++i) bi.u32(0);
    o.chunk("strf", bi.b.data(), bi.b.size());
    o.listEnd(s);
  }
  {
    size_t s = o.listBegin("strl");
    Out a;  // strh (audio): 56 bytes, scale = block align so length is in samples
    a.id("auds");
    a.u32(0);
    a.u32(0);
    a.u16(0);
    a.u16(0);
    a.u32(0);
    a.u32(blockAlign);
    a.u32(sr * blockAlign);
    a.u32(0);
    a.u32((unsigned)total);
    a.u32(0);
    a.u32(0xffffffff);
    a.u32(blockAlign);
    a.u32(0);
    a.u32(0);
    o.chunk("strh", a.b.data(), a.b.size());
    Out wf;  // strf: WAVEFORMATEX PCM, 18 bytes
    wf.u16(1);
    wf.u16((unsigned)ch);
    wf.u32(sr);
    wf.u32(sr * blockAlign);
    wf.u16(blockAlign);
    wf.u16(16);
    wf.u16(0);
    o.chunk("strf", wf.b.data(), wf.b.size());
    o.listEnd(s);
  }
  o.listEnd(hdrl);

  size_t movi = o.listBegin("movi");
  struct Idx {
    char id[4];
    unsigned flags, off, size;
  };
  std::vector<Idx> idx;
  for (size_t i = 0; i < nFrames; ++i) {
    const auto& jpg = r.jpegs[std::min(i, grabbed - 1)];
    size_t hdr = o.b.size();
    idx.push_back({{'0', '0', 'd', 'c'}, 0x10, (unsigned)(hdr - (movi + 4)),
                   (unsigned)jpg.size()});
    o.chunk("00dc", jpg.data(), jpg.size());

    juce::int64 a = audioStart(i), b = audioStart(i + 1);
    if (b > a) {
      size_t n = (size_t)(b - a) * blockAlign;
      hdr = o.b.size();
      idx.push_back({{'0', '1', 'w', 'b'}, 0, (unsigned)(hdr - (movi + 4)),
                     (unsigned)n});
      o.chunk("01wb", &pcm[(size_t)a * ch], n);
    }
  }
  o.listEnd(movi);

  Out ix;
  for (const auto& e : idx) {
    ix.raw(e.id, 4);
    ix.u32(e.flags);
    ix.u32(e.off);
    ix.u32(e.size);
  }
  o.chunk("idx1", ix.b.data(), ix.b.size());
  o.patch32(riff, (unsigned)(o.b.size() - (riff + 4)));

  out.getParentDirectory().createDirectory();
  if (!out.replaceWithData(o.b.data(), o.b.size())) {
    err = "cannot write video file";
    return false;
  }
  return true;
}
}  // namespace pp
