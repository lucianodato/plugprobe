// MIT License — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe: single JUCE/C++ CLI per SPEC.md §3. `plugprobe <cmd> --json <args.json>`
// -> {ok,data|error} on stdout, exit 0/1. M0+M3 core: scan/inspect/render/
// compare real; snapshot/act/session/meters spec-shaped stubs for M1/M2.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#if !JUCE_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <string>
#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>  // pumpMessages (JUCE pump API is
#endif  // GUI-app-only; console hosts pump the runloop directly)

#if JUCE_MAC
// Defined in plugprobe_shot.mm (global linkage). Returns 1 ok / 0 fail / -1 blank.
int plugprobeSaveNSViewShot(void* nsView, const char* path, int* w, int* h);
// Window-list fallback for custom-painted (Metal/async) editors whose NSView
// grab comes out blank: captures the composited window when on-screen.
int plugprobeSaveWindowShot(void* nsView, const char* path, int* w, int* h);
bool plugprobeShowFront(void* nsView);
#endif
#if JUCE_MAC
// Defined in plugprobe_os.mm (global linkage). AX tree of our hosted window;
// press returns 1 pressed / 0 unknown id / -1 press failed.
struct PlugprobeAxNode {  std::string id, role, name;
  bool enabled;
  std::string value;
  double x, y, w, h;
};
std::vector<PlugprobeAxNode> plugprobeAxDump(void* nsView);
int plugprobeAxPressById(const char* nodeId, void* nsView, double* cx, double* cy);
// Raw screen-coordinate click (AX space == mouse space, top-left) for AX-empty
// custom-painted editors: grounded by a visible screenshot, flagged fragile.
bool plugprobeAxClickAt(double x, double y);
void plugprobeFocusWindow(void* nsView);
bool plugprobeAxDragAt(double x, double y, double dx, double dy);
bool plugprobeAxTypeText(const char* text);
void plugprobePumpApp(double seconds);
// Accessibility trust: ad-hoc signatures change hash every rebuild, which
// invalidates the grant — HID drag/type must check this at runtime.
extern "C" bool AXIsProcessTrusted(void);
#endif

namespace {
constexpr double kDefSr = 48000.0;
constexpr int kDefBlock = 512;
juce::var errObj(const char* code, const juce::String& msg) {
  auto* o = new juce::DynamicObject();
  o->setProperty("code", code);
  o->setProperty("message", msg);
  return juce::var(o);
}
// Foreign plugin backends (LV2/lilv especially) printf to stdout, which would
// corrupt the single-JSON-line contract: quarantine fd 1 during hosting work,
// restore it only for the final emit.
#if !JUCE_WINDOWS
int g_savedOut = -1;
void quietStdout() {
  if (g_savedOut < 0) {
    g_savedOut = dup(STDOUT_FILENO);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      close(dn);
    }
  }
}
void restoreStdout() {
  if (g_savedOut >= 0) {
    fflush(stdout);
    dup2(g_savedOut, STDOUT_FILENO);
  }
}
#else
void quietStdout() {}
void restoreStdout() { fflush(stdout); }
#endif

void emitOk(const juce::var& data) {
  auto* o = new juce::DynamicObject();
  o->setProperty("ok", true);
  o->setProperty("data", data);
  restoreStdout();
  std::printf("%s\n", juce::JSON::toString(juce::var(o), true).toRawUTF8());
  std::fflush(stdout);  // survive plugin exit-time crashes
  // ponytail: hard-exit; graceful teardown races plugin-owned timer threads
  // (its TimerThread posts to its queue while our exit unloads it -> SIGSEGV).
  // All artifacts are flushed above, so nothing is lost. Cannot fix
  // plugin-side (MCP scope only); containment is the honest fix.
  _exit(0);
}
void emitErr(const juce::var& e) {
  auto* o = new juce::DynamicObject();
  o->setProperty("ok", false);
  o->setProperty("error", e);
  restoreStdout();
  std::printf("%s\n", juce::JSON::toString(juce::var(o), true).toRawUTF8());
  std::fflush(stdout);  // survive plugin exit-time crashes
  _exit(1);
}
juce::String jstr(const juce::var& v, const char* k, const char* d = "") {
  return v.hasProperty(k) ? v[k].toString() : juce::String(d);
}
double jnum(const juce::var& v, const char* k, double d) {
  return v.hasProperty(k) ? (double)v[k] : d;
}

juce::AudioPluginFormatManager& formats() {
  static juce::AudioPluginFormatManager fm;
  static bool init = false;
  if (!init) {
    addHeadlessDefaultFormatsToManager(fm);
    init = true;
  }
  return fm;
}

void scanInto(juce::KnownPluginList& list,
              const std::vector<juce::String>& dirs,
              const juce::String& formatFilter, bool explicitPaths) {
  auto& fm = formats();
  for (auto* f : fm.getFormats()) {
    juce::String fname = f->getName();  // "VST3", "AudioUnit", "LV2", ...
    if (formatFilter.isNotEmpty() && !fname.containsIgnoreCase(formatFilter))
      continue;
    // AU discovery is registry-based (ignores dirs) and loads every installed
    // AU binary: only pay that when the caller didn't narrow to file paths.
    if (explicitPaths && fname.containsIgnoreCase("AudioUnit")) continue;
    juce::FileSearchPath sp;
    for (auto& d : dirs) sp.add(juce::File(d));
    if (sp.getNumPaths() == 0) continue;
    juce::PluginDirectoryScanner sc(list, *f, sp, true, juce::File());
    juce::String n;
    while (sc.scanNextFile(true, n)) {
    }
  }
}
std::vector<juce::String> defaultPaths() {
#if JUCE_MAC
  return {"/Library/Audio/Plug-Ins/VST3", "~/Library/Audio/Plug-Ins/VST3",
          "/Library/Audio/Plug-Ins/Components"};
#elif JUCE_WINDOWS
  return {"C:\\Program Files\\Common Files\\VST3"};
#else
  return {"~/.vst3", "/usr/lib/vst3", "~/.lv2", "/usr/lib/lv2"};
#endif
}
std::vector<juce::String> argPaths(const juce::var& a) {
  std::vector<juce::String> out;
  if (auto* arr = a["paths"].getArray()) {
    for (auto& v : *arr) out.push_back(v.toString());
  } else if (a.hasProperty("paths")) {
    out.push_back(a["paths"].toString());
  } else {
    out = defaultPaths();
  }
  return out;
}

// Resolve --plugin <id|path|name> to a PluginDescription via one scan pass.
bool findPlugin(const juce::String& q, const std::vector<juce::String>& dirs,
                juce::PluginDescription& desc) {
  if (q.isEmpty()) return false;
  juce::String qq = q.trim();
  // Strip bundle-inner paths (…​.vst3/Contents/…) to the bundle root.
  juce::String bq = qq;
  for (auto ext : {".vst3", ".component", ".vst"}) {
    int i = bq.indexOfIgnoreCase(ext);
    if (i >= 0) {
      bq = bq.substring(0, i + juce::String(ext).length());
      break;
    }
  }
  juce::File qf(bq);
  std::vector<juce::String> ds = dirs;
  if (qf.exists()) {  // dir bundles + files (existsAsFile misses dirs)
    ds.push_back(qf.isDirectory() ? qf.getFullPathName()
                                  : qf.getParentDirectory().getFullPathName());
  }
  juce::KnownPluginList list;
  scanInto(list, ds, "", true);
  juce::String qb = qf.getFileName();
  juce::String qbNoExt = qf.getFileNameWithoutExtension();
  for (auto& t : list.getTypes()) {
    if (t.fileOrIdentifier == qq || t.name == qq || t.descriptiveName == qq ||
        juce::String(t.uniqueId) == qq || t.fileOrIdentifier.contains(qq) ||
        qq.contains(t.fileOrIdentifier)) {
      desc = t;
      return true;
    }
    juce::String tb = juce::File(t.fileOrIdentifier).getFileName();
    if (qb.isNotEmpty() && (tb == qb || tb == qbNoExt || t.name == qbNoExt ||
                            t.descriptiveName == qbNoExt)) {
      desc = t;
      return true;
    }
  }
  return false;
}
std::unique_ptr<juce::AudioPluginInstance> instantiate(
    const juce::PluginDescription& d, double sr, int block, juce::String& e) {
  return formats().createPluginInstance(d, sr, block, e);
}

// --- Opt-in live UI (visible windows) + editor screenshots ---
// Diagnostic-only: never fail the command; off by default (headless path is
// byte-identical without them). macOS-only capture (plugprobe_shot.mm).
bool guiCapable(const juce::PluginDescription& desc) {
  return desc.pluginFormatName.containsIgnoreCase("VST3") ||
         desc.pluginFormatName.containsIgnoreCase("AudioUnit");
}
// GUI-backed instance (the headless manager has no editors by design).
std::unique_ptr<juce::AudioPluginInstance> guiCreate(
    const juce::PluginDescription& desc, double sr, int block,
    juce::String& e) {
#if !JUCE_MAC
  (void)desc;
  (void)sr;
  (void)block;
  (void)e;
  return nullptr;
#else
  // GUI VST3/AU formats derive from their headless bases, so they register
  // into the same manager; sync create stays on the message thread (here: main).
  // ponytail: intentionally leaked; a static destructor would unload plugin
  // bundles after JUCE shutdown and crash at exit.
  static juce::AudioPluginFormatManager* gm =
      new juce::AudioPluginFormatManager();
  static bool ginit = false;
  if (!ginit) {
    gm->addFormat(std::make_unique<juce::VST3PluginFormat>());
    gm->addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
    ginit = true;
  }
  return std::unique_ptr<juce::AudioPluginInstance>(
      gm->createPluginInstance(desc, sr, block, e));
#endif
}
// Open the instance's editor: centered onscreen (visible mode) or far
// offscreen (shot-only: attach + paint run, nothing flashes). Returns editor
// or null (why set: no-editor / no-peer / visible-not-supported). Visible
// opens self-verify: the window must be ordered in, or the open fails loud
// instead of echoing a false `"visible": true`.
void holdUi(int holdMs);
juce::AudioProcessorEditor* openEditor(juce::AudioPluginInstance& gui,
                                       bool onscreen, juce::String& why) {
  juce::AudioProcessorEditor* ed = nullptr;
  JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wdeprecated-declarations")
  ed = gui.createEditorIfNeeded();  // straight to creation: probing first
  JUCE_END_IGNORE_WARNINGS_GCC_LIKE  // double-creates views, crashing some plugins
  if (ed == nullptr) {
    why = "no-editor";
    return nullptr;
  }
  if (ed->getWindowHandle() == nullptr && !ed->isOnDesktop()) {
    // Offscreen components have no peer: attach hidden, never ordered front.
    ed->setVisible(false);
    ed->addToDesktop(0);
  }
  if (onscreen) {
    // VST3/AU attach their native views on visibility change; without it the
    // peer stays childless (black shot).
    if (auto* pd =
            juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
      auto r = pd->userBounds;
      ed->setTopLeftPosition(r.getX() + (r.getWidth() - ed->getWidth()) / 2,
                             r.getY() + (r.getHeight() - ed->getHeight()) / 2);
    } else {
      ed->setTopLeftPosition(100, 100);
    }
  } else {
    ed->setTopLeftPosition(-20000, -20000);
  }
  ed->setVisible(true);
  if (ed->getWindowHandle() == nullptr) {
    why = "no-peer";
    return nullptr;
  }
#if JUCE_MAC
  // Let asynchronously-built third-party views populate before the
  // caller walks/captures: attach, first paint and AX registration settle here.
  holdUi(400);
  if (onscreen) {
    // Background CLI tools are never ordered front unless they ask: promote
    // to a regular app, steal focus, then confirm pixels can reach screen.
    if (!plugprobeShowFront(ed->getWindowHandle())) {
      why = "visible-not-supported";
      if (ed->isOnDesktop()) ed->removeFromDesktop();
      return nullptr;
    }
  }
#endif
  return ed;
}
// Keep a visible window live for holdMs (macOS pumps + drains AppKit event
// delivery; elsewhere sleeps).
void holdUi(int holdMs) {
  if (holdMs <= 0) return;
#if JUCE_MAC
  double end = juce::Time::getMillisecondCounterHiRes() + holdMs;
  while (juce::Time::getMillisecondCounterHiRes() < end) plugprobePumpApp(0.05);
#else
  juce::Thread::sleep(holdMs);
#endif
}
#if JUCE_MAC
// Map capture result to saved-path-or-why. 1 ok / 0 fail / -1 blank view.
bool noteCapture(int rc, const juce::String& path, juce::String& saved,
                 juce::String& why) {
  if (rc > 0) {
    saved = juce::File(path).getFullPathName();
    return true;
  }
  if (rc < 0) {
    why = "editor-not-capturable";  // paints black: async/Metal view, no pixels
    juce::File(path).deleteFile();  // never leave a stale black PNG behind
  } else {
    why = "capture";
  }
  return false;
}
#endif
// Full shot flow on a fresh GUI instance. Capture and display failures are
// tracked separately: a blank (Metal/async) capture must never cancel the
// on-screen window (it still grounds coordinate clicks for AX-empty editors).
juce::String saveEditorShot(const juce::PluginDescription& desc, double sr,
                            int block, const juce::String& path,
                            juce::String& shotWhy, juce::String& visWhy,
                            bool visible, int holdMs) {
  shotWhy = {};
  visWhy = {};
  if (path.isEmpty() && !visible) return {};
#if !JUCE_MAC
  (void)desc;
  (void)sr;
  (void)block;
  return {};
#else
  if (!guiCapable(desc)) {
    if (path.isNotEmpty()) shotWhy = "unsupported-format";  // LV2/etc
    if (visible) visWhy = "unsupported-format";
    return {};
  }
  juce::String e;
  auto gui = guiCreate(desc, sr, block, e);
  if (gui == nullptr) {
    juce::String why =
        juce::String("gui-instantiate: ") + e.substring(0, 120);
    if (path.isNotEmpty()) shotWhy = why;
    if (visible) visWhy = why;
    return {};
  }
  juce::String openWhy;
  auto* ed = openEditor(*gui, visible, openWhy);
  if (ed == nullptr) {
    if (path.isNotEmpty()) shotWhy = openWhy;
    if (visible) visWhy = openWhy;
    return {};
  }
  juce::String saved;
  if (path.isNotEmpty()) {
    noteCapture(plugprobeSaveNSViewShot(ed->getWindowHandle(), path.toRawUTF8(),
                                    nullptr, nullptr),
                path, saved, shotWhy);
  }
  if (visible) holdUi(holdMs);
  if (ed->isOnDesktop()) ed->removeFromDesktop();
  return saved;
#endif
}

// --- WAV helpers (JUCE registered formats) ---
bool readWav(const juce::String& path, std::vector<std::vector<float>>& ch,
             double& sr) {
  juce::AudioFormatManager afm;
  afm.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> r(afm.createReaderFor(juce::File(path)));
  if (r == nullptr) return false;
  sr = r->sampleRate;
  int nCh = std::max(1, (int)r->numChannels);
  ch.assign((size_t)nCh, {});
  juce::AudioBuffer<float> buf(nCh, (int)r->lengthInSamples);
  r->read(&buf, 0, (int)r->lengthInSamples, 0, true, true);
  for (size_t c = 0; c < ch.size(); ++c)
    ch[c].assign(buf.getReadPointer((int)c),
                 buf.getReadPointer((int)c) + r->lengthInSamples);
  return true;
}
bool writeWav(const juce::String& path,
              const std::vector<std::vector<float>>& ch, double sr) {
  juce::File f(path);
  f.getParentDirectory().createDirectory();
  f.deleteFile();
  std::unique_ptr<juce::OutputStream> out(f.createOutputStream());
  if (out == nullptr) return false;
  juce::WavAudioFormat wav;
  auto opts = juce::AudioFormatWriterOptions()
                  .withSampleRate(sr)
                  .withChannelLayout(
                      juce::AudioChannelSet::discreteChannels((int)ch.size()))
                  .withBitsPerSample(16);
  std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(out, opts));
  if (w == nullptr) return false;
  juce::AudioBuffer<float> buf((int)ch.size(),
                               ch.empty() ? 0 : (int)ch[0].size());
  for (size_t c = 0; c < ch.size(); ++c)
    buf.copyFrom((int)c, 0, ch[c].data(), (int)ch[c].size());
  return w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}
// FNV-1a/64 over raw float bytes: deterministic content hash, no crypto dep.
juce::String hashHex(const std::vector<std::vector<float>>& ch) {
  uint64_t h = 1469598103934665603ULL;
  for (auto& c : ch) {
    const auto* p = reinterpret_cast<const uint8_t*>(c.data());
    for (size_t i = 0; i < c.size() * sizeof(float); ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
  }
  return juce::String::toHexString((int64_t)h);
}
double rmsOf(const std::vector<float>& v) {
  double s = 0;
  for (float x : v) s += (double)x * x;
  return v.empty() ? 0 : std::sqrt(s / v.size());
}
double peakDbOf(const std::vector<std::vector<float>>& ch) {
  float p = 0;
  for (auto& c : ch)
    for (float x : c) p = std::max(p, std::abs(x));
  return p <= 0 ? -120.0 : 20.0 * std::log10(p);
}
double toDb(double x) { return x <= 0 ? -120.0 : 20.0 * std::log10(x); }
double clampd(double x, double lo, double hi) {
  return std::min(hi, std::max(lo, x));
}

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

// Pace an offline render to wall-clock (1x) while a visible window is open, so
// the UI can be followed in real time. Sleeps in short pumped slices to keep
// the window live. Never slows headless renders; never catches up when DSP is
// slower than realtime.
void paceToRealtime(double t0ms, size_t doneFrames, double sr) {
  double target = t0ms + doneFrames / sr * 1000.0;
#if JUCE_MAC
  for (;;) {
    double left = target - juce::Time::getMillisecondCounterHiRes();
    if (left <= 0) break;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::min(left / 1000.0, 0.05),
                       false);
  }
#else
  double left = target - juce::Time::getMillisecondCounterHiRes();
  if (left > 0) juce::Thread::sleep((int)left);
#endif
}

// Flush plugin-side deferred (AsyncUpdater) param updates during offline renders.
// Headless render loops never re-enter the event loop, so mid-render timeline
// switches would otherwise sit in the plugin's message queue forever.
inline void pumpMessages() {
#if JUCE_MAC
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);
#endif
}

// Resolve {index|name} to a param index, or -1. Single choke point for all
// param-plane entry (render params/timeline/ui_script, act).
int findParamIndex(juce::AudioPluginInstance& inst, const juce::String& key) {
  if (key.containsOnly("0123456789")) {
    int idx = key.getIntValue();
    if (idx >= 0 && idx < inst.getParameters().size()) return idx;
    return -1;
  }
  for (int i = 0; i < inst.getParameters().size(); ++i)
    if (inst.getParameters()[i]->getName(128) == key) return i;
  return -1;
}
juce::String validParamNames(juce::AudioPluginInstance& inst) {
  juce::StringArray names;
  for (int i = 0; i < inst.getParameters().size(); ++i)
    names.add(juce::String(i) + ":" +
              inst.getParameters()[i]->getName(128));
  return names.joinIntoString(", ");
}
// First unknown target in a {target: value} map, or "".
juce::String unknownTargetIn(juce::AudioPluginInstance& inst,
                             const juce::var& map) {
  if (auto* o = map.getDynamicObject())
    for (auto& kv : o->getProperties())
      if (findParamIndex(inst, kv.name.toString()) < 0)
        return kv.name.toString();
  return {};
}

// Resolved bundle identity (a stale ~/Library install silently shadows a
// fresh repo build; surfacing the path + mtime saves debug cycles).
// Bundle dirs keep their own mtime across remove/reinstall cycles, so derive
// it from the newest file content instead (the executable drives DSP/params).
void setBundleProps(juce::DynamicObject* o, const juce::PluginDescription& d) {
  o->setProperty("bundle", d.fileOrIdentifier);
  juce::File bf(d.fileOrIdentifier);
  juce::Time mt;
  if (bf.isDirectory()) {
    for (auto& k : bf.findChildFiles(juce::File::findFiles, true))
      if (k.getLastModificationTime() > mt) mt = k.getLastModificationTime();
  } else if (bf.existsAsFile()) {
    mt = bf.getLastModificationTime();
  }
  if (mt.toMilliseconds() > 0) o->setProperty("bundleMtime", mt.toISO8601(true));
}

// Apply {index|name: 0..1} param map; returns deltas.
juce::var applyParams(juce::AudioPluginInstance& inst, const juce::var& map) {
  juce::Array<juce::var> arr;
  if (auto* o = map.getDynamicObject()) {
    for (auto& kv : o->getProperties()) {
      int idx = findParamIndex(inst, kv.name.toString());
      if (idx < 0) continue;
      juce::AudioProcessorParameter* p = inst.getParameters()[idx];
      float before = p->getValue();
      float after = (float)clampd((double)kv.value, 0, 1);
      p->beginChangeGesture();
      p->setValueNotifyingHost(after);
      p->endChangeGesture();
      auto* d = new juce::DynamicObject();
      d->setProperty("index", idx);
      d->setProperty("before", before);
      d->setProperty("after", p->getValue());
      arr.add(juce::var(d));
    }
  }
  return juce::var(arr);
}
#if JUCE_MAC
// Smallest AX node containing a screen point: tells a raw {x,y} click what it
// hit (or that the tree is empty there). Null var when nothing contains it.
juce::var hitNodeAt(void* hv, double x, double y) {
  double best = 1e18;
  const PlugprobeAxNode* hit = nullptr;
  // ponytail: dump-then-scan; trees are tiny (<100 nodes), no index needed.
  auto nodes = plugprobeAxDump(hv);
  for (auto& nn : nodes) {
    if (x >= nn.x && x < nn.x + nn.w && y >= nn.y && y < nn.y + nn.h) {
      double area = nn.w * nn.h;
      if (area < best) {
        best = area;
        hit = &nn;
      }
    }
  }
  if (hit == nullptr) return {};
  auto* m = new juce::DynamicObject();
  m->setProperty("id", juce::String(hit->id));
  m->setProperty("role", juce::String(hit->role));
  if (!hit->value.empty()) m->setProperty("state", juce::String(hit->value));
  return juce::var(m);
}
#endif
}  // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  quietStdout();  // quarantine before any backend can print
  if (argc < 2) {
    emitErr(errObj("ARGS", "usage: plugprobe <cmd> --json <args.json>"));
    return 1;
  }
  juce::String cmd(argv[1]);
  // `plugprobe session start` -> session-start
  if (argc > 2 && juce::String(argv[2]).startsWithChar('-') == false &&
      (cmd == "session")) {
    cmd = "session-" + juce::String(argv[2]);
    std::memmove(&argv[2], &argv[3], sizeof(char*) * (size_t)(argc - 3));
    --argc;
  }
  juce::String jsonPath;
  for (int i = 2; i + 1 < argc; ++i)
    if (juce::String(argv[i]) == "--json") jsonPath = argv[i + 1];
  juce::var args;
  if (jsonPath.isNotEmpty()) {
    juce::File f(jsonPath);
    juce::var parsed;
    if (!juce::JSON::parse(f.loadFileAsString(), parsed) || !parsed.isObject()) {
      emitErr(errObj("JSON", "cannot parse --json file"));
      return 1;
    }
    args = parsed;
  } else {
    args = juce::var(new juce::DynamicObject());
  }

  if (cmd == "scan") {
    bool explicitPaths = args.hasProperty("paths");
    juce::KnownPluginList list;
    scanInto(list, argPaths(args), jstr(args, "format"), explicitPaths);
    juce::Array<juce::var> arr;
    for (auto& t : list.getTypes()) {
      auto* o = new juce::DynamicObject();
      o->setProperty("id", t.fileOrIdentifier);
      o->setProperty("file", t.fileOrIdentifier);
      o->setProperty("format", t.pluginFormatName);
      o->setProperty("name",
                     t.descriptiveName.isNotEmpty() ? t.descriptiveName : t.name);
      o->setProperty("params", -1);  // count needs instantiate; see inspect
      arr.add(juce::var(o));
    }
    // Opt-in instantiate pass (offline, deterministic); default off because
    // instantiating every found plugin loads foreign binaries (slow).
    if ((bool)args["params_count"]) {
      double sr = jnum(args, "sr", kDefSr);
      int block = (int)jnum(args, "block", kDefBlock);
      for (int i = 0; i < std::min(arr.size(), 16); ++i) {
        juce::PluginDescription d;
        if (!findPlugin(arr[i]["id"].toString(), argPaths(args), d)) continue;
        juce::String e;
        if (auto inst = instantiate(d, sr, block, e))
          arr.getReference(i).getDynamicObject()->setProperty(
              "params", inst->getParameters().size());
      }
    }
    auto* o = new juce::DynamicObject();
    o->setProperty("plugins", juce::var(arr));
    emitOk(juce::var(o));
    return 0;
  }

  if (cmd == "inspect") {
    juce::PluginDescription d;
    if (!findPlugin(jstr(args, "plugin"), argPaths(args), d)) {
      emitErr(errObj("NOT_FOUND", "plugin not found: " + jstr(args, "plugin")));
      return 1;
    }
    double sr = jnum(args, "sr", kDefSr);
    int block = (int)jnum(args, "block", kDefBlock);
    juce::String e;
    auto inst = instantiate(d, sr, block, e);
    if (inst == nullptr) {
      emitErr(errObj("INSTANTIATE", e));
      return 1;
    }
    juce::Array<juce::var> arr;
    auto& ps = inst->getParameters();
    for (int i = 0; i < ps.size(); ++i) {
      auto* p = ps[i];
      auto* o = new juce::DynamicObject();
      o->setProperty("index", i);
      o->setProperty("name", p->getName(128));
      o->setProperty("label", p->getLabel());
      o->setProperty("value", p->getValue());
      o->setProperty("min", 0.0);
      o->setProperty("max", 1.0);
      o->setProperty("default", p->getDefaultValue());
      o->setProperty("automatable", p->isAutomatable());
      arr.add(juce::var(o));
    }
    auto* ed = new juce::DynamicObject();
    ed->setProperty("hasUI", true);
    ed->setProperty("width", 0);
    ed->setProperty("height", 0);
    if (inst->hasEditor()) {
      // Deprecated in JUCE 8 but the only public sync editor accessor.
      JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wdeprecated-declarations")
      if (auto* comp = inst->createEditorIfNeeded()) {
        ed->setProperty("width", comp->getWidth());
        ed->setProperty("height", comp->getHeight());
      }
      JUCE_END_IGNORE_WARNINGS_GCC_LIKE
    } else {
      ed->setProperty("hasUI", false);
    }
    auto* o = new juce::DynamicObject();
    o->setProperty("params", juce::var(arr));
    o->setProperty("editor", juce::var(ed));
    setBundleProps(o, d);
    juce::String sp = jstr(args, "shot");
    bool vis = (bool)args["visible"];
    if (sp.isNotEmpty() || vis) {
      juce::String shotWhy, visWhy;
      juce::String saved = saveEditorShot(d, sr, block, sp, shotWhy, visWhy,
                                          vis,
                                          (int)jnum(args, "holdMs", 2000));
      if (saved.isNotEmpty())
        o->setProperty("screenshot", saved);
      else if (sp.isNotEmpty())
        o->setProperty("shotSkipped", shotWhy);
      if (vis) {
        if (visWhy.isNotEmpty())
          o->setProperty("visibleSkipped", visWhy);
        else
          o->setProperty("visible", true);
      }
    }
    emitOk(juce::var(o));
    return 0;
  }

  if (cmd == "render") {
    juce::String inP = jstr(args, "in"), outP = jstr(args, "out");
    if (args.hasProperty("a")) inP = args["a"].toString();
    if (inP.isEmpty() && args["inputs"].isArray() && args["inputs"].size() > 0)
      inP = args["inputs"][0].toString();
    if (inP.isEmpty() || outP.isEmpty()) {
      emitErr(errObj("ARGS", "render needs {plugin,in,out}"));
      return 1;
    }
    double sr = jnum(args, "sr", kDefSr);
    int block = (int)jnum(args, "block", kDefBlock);
    bool bypass = (bool)args["bypass"];
    std::vector<std::vector<float>> ch;
    double fileSr = sr;
    if (!readWav(inP, ch, fileSr)) {
      emitErr(errObj("IO", "cannot read input wav: " + inP));
      return 1;
    }
    std::unique_ptr<juce::AudioPluginInstance> owned, guiOwned;
    juce::AudioPluginInstance* inst = nullptr;
    juce::AudioProcessorEditor* visEd = nullptr;
    juce::String visWhy, edWhy;
    juce::PluginDescription renderDesc;
    bool wantShot = jstr(args, "shot").isNotEmpty();
    bool wantVis = (bool)args["visible"];
    if (!bypass) {
      juce::PluginDescription d;
      if (!findPlugin(jstr(args, "plugin"), argPaths(args), d)) {
        emitErr(errObj("NOT_FOUND", "plugin not found"));
        return 1;
      }
      renderDesc = d;
      // GUI-backed processing instance when the UI will be shown/captured or
      // clicked, so the window reflects the audio being rendered (live
      // meters, real clicks). The default path stays headless (byte-identical).
      // Timeline entries are pre-scanned: the full parse happens later.
      bool wantGui = wantShot || wantVis;
      if (!wantGui) {
        if (auto* tl0 = args["timeline"].getArray()) {
          for (auto& e0 : *tl0) {
            bool coordClick = e0.hasProperty("click") &&
                              e0["click"].isObject() &&
                              e0["click"].hasProperty("x");
            if (jstr(e0, "click").isNotEmpty() || coordClick ||
                jstr(e0, "shot").isNotEmpty()) {
              wantGui = true;
              break;
            }
          }
        }
      }
      if (wantGui && guiCapable(d)) {
        juce::String ge;
        guiOwned = guiCreate(d, sr, block, ge);
        if (guiOwned != nullptr)
          inst = guiOwned.get();
        else if (wantVis)
          visWhy = juce::String("gui-instantiate: ") + ge.substring(0, 120);
      } else if (wantVis) {
        visWhy = "unsupported-format";
      }
      if (inst == nullptr) {
        juce::String e;
        owned = instantiate(d, sr, block, e);
        if (owned == nullptr) {
          emitErr(errObj("INSTANTIATE", e));
          return 1;
        }
        inst = owned.get();
      }
      // Fail loud on unknown targets: silent passthrough renders are worse
      // than errors (every param-plane entry routes through here).
      if (args.hasProperty("params")) {
        if (juce::String u = unknownTargetIn(*inst, args["params"]);
            u.isNotEmpty()) {
          emitErr(errObj("ARGS", "render: unknown param '" + u + "' (have: " +
                                     validParamNames(*inst) + ")"));
          return 1;
        }
        applyParams(*inst, args["params"]);
      }
      if (args.hasProperty("params_json")) {
        juce::File pf(args["params_json"].toString());
        juce::var pm;
        if (juce::JSON::parse(pf.loadFileAsString(), pm)) {
          juce::var map = pm.hasProperty("params") ? pm["params"] : pm;
          if (juce::String u = unknownTargetIn(*inst, map); u.isNotEmpty()) {
            emitErr(errObj("ARGS", "render: unknown param '" + u +
                                       "' (have: " + validParamNames(*inst) +
                                       ")"));
            return 1;
          }
          applyParams(*inst, map);
        }
      }
      // ui_script: juce-plane steps only (param targets); os-only skipped.
      if (auto* sc = args["ui_script"].getArray()) {
        for (auto& s : *sc) {
          juce::String tgt = s.hasProperty("target") ? s["target"].toString() : "";
          if (tgt.isEmpty()) continue;
          if (findParamIndex(*inst, tgt) < 0) {
            emitErr(errObj("ARGS", "render: unknown ui_script target '" +
                                       tgt + "' (have: " +
                                       validParamNames(*inst) + ")"));
            return 1;
          }
          auto* m = new juce::DynamicObject();
          m->setProperty(tgt, s.hasProperty("value") ? (float)s["value"] : 1.f);
          applyParams(*inst, juce::var(m));
        }
      }
      inst->setRateAndBufferSizeDetails(sr, block);
      inst->prepareToPlay(sr, block);
    }
    int nCh = std::max(1, (int)ch.size());
    size_t n = ch[0].size();
    std::vector<std::vector<float>> out(nCh, std::vector<float>(n, 0));
    juce::String shotSaved, shotWhy;
    juce::Array<juce::var> shotsArr, clicksArr;
    bool hasEntryShot = false, hasEntryClick = false;
    if (bypass) {
      out = ch;
    } else {
      // timeline [{atMs, params, shot?, click?}] on the same instance.
      // Per-entry `shot` captures the live editor right after the switch
      // lands; per-entry `click` presses a snapshot node id — or raw
      // screen {x,y} for AX-empty custom-painted editors — on the live
      // window (native buttons no param can reach). One render call is the
      // scripted open → click → audio → click → capture session equivalent.
      struct Ev {
        size_t frame;
        double atMs;
        juce::var map;
        juce::String shot;
        juce::String click;
        bool clickIsCoord = false;
        double clickX = 0, clickY = 0;
      };
      std::vector<Ev> evs;
      if (auto* tl = args["timeline"].getArray()) {
        int ti = 0;
        for (auto& e : *tl) {
          double atMs = e.hasProperty("atMs")
                            ? (double)e["atMs"]
                            : (double)e["at_ms"];
          juce::var map = e.hasProperty("params") ? e["params"] : e["map"];
          bool clickIsCoord = e.hasProperty("click") && e["click"].isObject() &&
                              e["click"].hasProperty("x") &&
                              e["click"].hasProperty("y");
          if (e.hasProperty("click") && e["click"].isObject() &&
              !clickIsCoord) {
            emitErr(errObj("ARGS", "render: timeline click object needs {x,y} "
                                   "screen coords (e.g. {click:{x:900,y:600}})"));
            return 1;
          }
          double clickX = clickIsCoord ? (double)e["click"]["x"] : 0;
          double clickY = clickIsCoord ? (double)e["click"]["y"] : 0;
          if (!map.isObject()) {
            if (jstr(e, "click").isEmpty() && !clickIsCoord &&
                jstr(e, "shot").isEmpty()) {
              emitErr(errObj("ARGS", "render: timeline entry needs {atMs,params}"));
              return 1;
            }
            map = juce::var(new juce::DynamicObject());  // click/shot only
          }
          if (juce::String u = unknownTargetIn(*inst, map); u.isNotEmpty()) {
            emitErr(errObj("ARGS", "render: timeline[" +
                                       juce::String(ti) + "] unknown param '" +
                                       u + "' (have: " +
                                       validParamNames(*inst) + ")"));
            return 1;
          }
          size_t f = (size_t)std::max(0.0, atMs / 1000.0 * sr);
          juce::String eshot = jstr(e, "shot");
          juce::String eclick = clickIsCoord ? juce::String("") : jstr(e, "click");
          if (eshot.isNotEmpty()) hasEntryShot = true;
          if (eclick.isNotEmpty() || clickIsCoord) hasEntryClick = true;
#if !JUCE_MAC
          if (eclick.isNotEmpty() || clickIsCoord) {
            emitErr(errObj("UNIMPLEMENTED_M1", "render: timeline clicks are "
                                               "macOS-only"));
            return 1;
          }
#endif
          evs.push_back({f, atMs, map, eshot, eclick, clickIsCoord, clickX,
                         clickY});
          ++ti;
        }
        std::sort(evs.begin(), evs.end(),
                  [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
      }
      // Editor for mid-render shots/clicks: hidden unless visible. Only
      // opened when captures or clicks were requested, so pure-headless
      // renders stay byte-identical.
      if (guiOwned != nullptr && visEd == nullptr &&
          (wantVis || wantShot || hasEntryShot || hasEntryClick)) {
        juce::String why;
        visEd = openEditor(*guiOwned, wantVis, why);
        if (visEd == nullptr) {
          edWhy = why;
          if (wantShot) shotWhy = why;
          if (wantVis) visWhy = why;
        } else if (wantVis) {
          pumpMessages();  // let the fresh window paint before audio starts
        }
      } else if (hasEntryClick && guiOwned == nullptr) {
        edWhy =
            "no GUI-backed instance (unsupported format or instantiate failed)";
      }
      if (hasEntryClick && visEd == nullptr) {
        emitErr(errObj("ARGS", "render: timeline clicks need a live editor "
                               "window: " + edWhy));
        return 1;
      }
      size_t ei = 0, bc = 0;
      juce::AudioBuffer<float> blk(std::max(2, nCh), block);
      juce::MidiBuffer midi;
      // Wall-clock anchor for 1x pacing while the live window is open.
      double paceT0 = juce::Time::getMillisecondCounterHiRes();
      for (size_t pos = 0; pos < n; pos += (size_t)block) {
        bool moved = false;
        while (ei < evs.size() && evs[ei].frame <= pos) {
          applyParams(*inst, evs[ei].map);
          moved = true;
          if (evs[ei].click.isNotEmpty() || evs[ei].clickIsCoord) {
            // Native click on the live window (buttons no param can reach).
            auto* k = new juce::DynamicObject();
            k->setProperty("atMs", evs[ei].atMs);
#if JUCE_MAC
            if (evs[ei].clickIsCoord) {
              // HID click: needs the on-screen window + Accessibility grant.
              if (!wantVis) {
                emitErr(errObj("ARGS", "render: timeline coordinate clicks need "
                                       "\"visible\":true (HID events cannot "
                                       "reach the offscreen window; node-id "
                                       "clicks work headless via AX)"));
                return 1;
              }
              if (!AXIsProcessTrusted()) {
                emitErr(errObj("AX_UNTRUSTED",
                               "render: timeline coordinate clicks need the "
                               "Accessibility grant for THIS plugprobe binary"));
                return 1;
              }
              plugprobeFocusWindow(visEd->getWindowHandle());  // else click only activates
              if (!plugprobeAxClickAt(evs[ei].clickX, evs[ei].clickY)) {
                emitErr(errObj("AX_CLICK",
                               "render: timeline coordinate click failed"));
                return 1;
              }
              auto* kc = new juce::DynamicObject();
              kc->setProperty("x", evs[ei].clickX);
              kc->setProperty("y", evs[ei].clickY);
              kc->setProperty("pressed", true);
              kc->setProperty("fragile", true);  // SPEC §5: raw coords
              if (juce::var hit =
                      hitNodeAt(visEd->getWindowHandle(), evs[ei].clickX,
                                evs[ei].clickY);
                  hit.isObject())
                kc->setProperty("hit", hit);
              k->setProperty("click", juce::var(kc));
            } else {
            double cx = 0, cy = 0;
            int rc = plugprobeAxPressById(evs[ei].click.toRawUTF8(),
                                      visEd->getWindowHandle(), &cx, &cy);
            if (rc <= 0) {
              emitErr(errObj("ARGS", juce::String("render: timeline click ") +
                                         (rc == 0 ? "unknown node '"
                                                  : "press failed on '") +
                                         evs[ei].click +
                                         "' (snapshot lists ids)"));
              return 1;
            }
            auto* kc = new juce::DynamicObject();
            kc->setProperty("node", evs[ei].click);
            kc->setProperty("pressed", true);
            auto* kp = new juce::DynamicObject();
            kp->setProperty("x", cx);
            kp->setProperty("y", cy);
            kc->setProperty("center", juce::var(kp));
#if JUCE_MAC
            for (auto& nn : plugprobeAxDump(visEd->getWindowHandle())) {
              if (juce::String(nn.id) == evs[ei].click) {
                if (!nn.value.empty())
                  kc->setProperty("state", juce::String(nn.value));
                break;
              }
            }
#endif
            k->setProperty("click", juce::var(kc));
            }
#else
            k->setProperty("clickSkipped", "no-editor-or-headless");
#endif
            clicksArr.add(juce::var(k));
            pumpMessages();  // let the press dispatch before audio resumes
          }
          if (evs[ei].shot.isNotEmpty()) {
            auto* s = new juce::DynamicObject();
            s->setProperty("atMs", evs[ei].atMs);
            if (visEd != nullptr) {
#if JUCE_MAC
              juce::String saved, why;
              if (noteCapture(plugprobeSaveNSViewShot(visEd->getWindowHandle(),
                                                  evs[ei].shot.toRawUTF8(),
                                                  nullptr, nullptr),
                              evs[ei].shot, saved, why))
                s->setProperty("screenshot", saved);
              else
                s->setProperty("shotSkipped", why);
#else
              s->setProperty("shotSkipped", "no-editor-or-headless");
#endif
            } else {
              s->setProperty("shotSkipped",
                             visWhy.isNotEmpty() ? visWhy : "no-editor");
            }
            shotsArr.add(juce::var(s));
          }
          ++ei;
        }
        if (moved) pumpMessages();  // flush plugin-side deferred updates
        if (visEd != nullptr && (bc++ % 4) == 0) pumpMessages();  // live UI
        int m = (int)std::min((size_t)block, n - pos);
        blk.clear();
        for (int c = 0; c < blk.getNumChannels(); ++c) {
          float* w = blk.getWritePointer(c);
          const auto& src = ch[(size_t)c % ch.size()];
          for (int i = 0; i < m; ++i) w[i] = src[pos + (size_t)i];
        }
        midi.clear();
        inst->processBlock(blk, midi);
        for (int c = 0; c < nCh; ++c) {
          const float* r = blk.getReadPointer(c % blk.getNumChannels());
          for (int i = 0; i < m; ++i) out[(size_t)c][pos + (size_t)i] = r[i];
        }
        if (visEd != nullptr) paceToRealtime(paceT0, pos + (size_t)m, sr);
      }
      int tailN = (int)(jnum(args, "tail_ms", 500.0) / 1000.0 * sr);
      if (tailN > 0) {
        size_t base = n;
        for (auto& c : out) c.resize(n + (size_t)tailN, 0);
        juce::AudioBuffer<float> blk(std::max(2, nCh), block);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < tailN; pos += block) {
          int m = std::min(block, tailN - pos);
          blk.clear();
          midi.clear();
          inst->processBlock(blk, midi);
          for (int c = 0; c < nCh; ++c) {
            const float* r = blk.getReadPointer(c % blk.getNumChannels());
            for (int i = 0; i < m; ++i)
              out[(size_t)c][base + (size_t)pos + (size_t)i] = r[i];
          }
          if (visEd != nullptr)
            paceToRealtime(paceT0, n + (size_t)(pos + m), sr);
        }
      }
      inst->releaseResources();
      juce::String rsp = jstr(args, "shot");
      if (guiOwned != nullptr && (rsp.isNotEmpty() || visEd != nullptr)) {
        // Capture the instance that actually rendered (post-render state).
        if (visEd == nullptr) {
          juce::String why;
          visEd = openEditor(*guiOwned, wantVis, why);
          if (visEd == nullptr) {
            if (rsp.isNotEmpty()) shotWhy = why;
            if (wantVis) visWhy = why;
          }
        }
        if (visEd != nullptr && rsp.isNotEmpty()) {
#if JUCE_MAC
          noteCapture(plugprobeSaveNSViewShot(visEd->getWindowHandle(),
                                          rsp.toRawUTF8(), nullptr, nullptr),
                      rsp, shotSaved, shotWhy);
#else
          shotWhy = "no-editor-or-headless";
#endif
        }
      } else if (rsp.isNotEmpty()) {
        juce::String why, visDummy;
        shotSaved =
            saveEditorShot(renderDesc, sr, block, rsp, why, visDummy, false, 0);
        if (shotSaved.isEmpty()) shotWhy = why;
      }
      if (visEd != nullptr) {
        if (wantVis) holdUi((int)jnum(args, "holdMs", 0));
        if (visEd->isOnDesktop()) visEd->removeFromDesktop();
        visEd = nullptr;
      }
    }
    if (!writeWav(outP, out, sr)) {
      emitErr(errObj("IO", "cannot write output wav"));
      return 1;
    }
    auto* o = new juce::DynamicObject();
    o->setProperty("out", outP);
    o->setProperty("sr", sr);
    o->setProperty("nFrames", (double)out[0].size());
    o->setProperty("inFrames", (double)n);
    o->setProperty("outFrames", (double)out[0].size());
    o->setProperty("tailFrames", (double)(out[0].size() - n));
    o->setProperty("tailNote", "in-aligned; flush tail appended at end");
    if (hasEntryShot) o->setProperty("shots", juce::var(shotsArr));
    if (hasEntryClick) o->setProperty("clicks", juce::var(clicksArr));
    o->setProperty("peakDb", peakDbOf(out));
    o->setProperty("hash", hashHex(out));
    if (!bypass) setBundleProps(o, renderDesc);
    if (jstr(args, "shot").isNotEmpty()) {
      if (shotSaved.isNotEmpty())
        o->setProperty("screenshot", shotSaved);
      else
        o->setProperty("shotSkipped", bypass ? "bypass-no-instance"
                                             : shotWhy.isNotEmpty()
                                                   ? shotWhy
                                                   : "no-editor-or-headless");
    }
    if (wantVis && bypass && visWhy.isEmpty()) visWhy = "bypass-no-instance";
    if (wantVis) {
      if (visWhy.isNotEmpty())
        o->setProperty("visibleSkipped", visWhy);
      else
        o->setProperty("visible", true);
    }
    emitOk(juce::var(o));
    return 0;
  }

  if (cmd == "compare") {
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

  if (cmd == "snapshot") {
    int limit = (int)jnum(args, "limit", 50);
    int offset = std::max(0, (int)jnum(args, "offset", 0));
    juce::String pq = jstr(args, "plugin");
    if (pq.isEmpty()) {
      emitErr(errObj("ARGS", "snapshot needs {plugin}"));
      return 1;
    }
    juce::PluginDescription sd;
    if (!findPlugin(pq, argPaths(args), sd)) {
      emitErr(errObj("NOT_FOUND", "plugin not found: " + pq));
      return 1;
    }
    bool vis = (bool)args["visible"];
    juce::String sp = jstr(args, "shot");
    auto* o = new juce::DynamicObject();
    // GUI instance carrying the window whose AX tree we dump (the same
    // window shot/visible paths would show).
    juce::String ge;
    std::unique_ptr<juce::AudioPluginInstance> gui;
    if (guiCapable(sd))
      gui = guiCreate(sd, jnum(args, "sr", kDefSr),
                      (int)jnum(args, "block", kDefBlock), ge);
    if (gui == nullptr) {
      juce::Array<juce::var> empty;
      o->setProperty("nodes", juce::var(empty));
      o->setProperty("total", 0);
      o->setProperty("limit", limit);
      o->setProperty("offset", offset);
      o->setProperty("unavailable", guiCapable(sd) ? "gui-instantiate"
                                                   : "unsupported-format");
      if (sp.isNotEmpty())
        o->setProperty("shotSkipped", guiCapable(sd) ? "gui-instantiate"
                                                     : "unsupported-format");
      if (vis) o->setProperty("visibleSkipped", "no-window");
      setBundleProps(o, sd);
      emitOk(juce::var(o));
      return 0;
    }
    juce::String why;
    auto* ed = openEditor(*gui, vis, why);
    if (ed == nullptr) {
      juce::Array<juce::var> empty;
      o->setProperty("nodes", juce::var(empty));
      o->setProperty("total", 0);
      o->setProperty("limit", limit);
      o->setProperty("offset", offset);
      o->setProperty("unavailable", why);
      if (sp.isNotEmpty()) o->setProperty("shotSkipped", why);
      if (vis) o->setProperty("visibleSkipped", why);
      setBundleProps(o, sd);
      emitOk(juce::var(o));
      return 0;
    }
    juce::Array<juce::var> nodes;
#if JUCE_MAC
    for (auto& nn : plugprobeAxDump(ed->getWindowHandle())) {
      auto* m = new juce::DynamicObject();
      m->setProperty("id", juce::String(nn.id));
      m->setProperty("role", juce::String(nn.role));
      m->setProperty("name", juce::String(nn.name));
      m->setProperty("enabled", nn.enabled);
      if (!nn.value.empty()) m->setProperty("value", juce::String(nn.value));
      auto* b = new juce::DynamicObject();
      b->setProperty("x", nn.x);
      b->setProperty("y", nn.y);
      b->setProperty("w", nn.w);
      b->setProperty("h", nn.h);
      m->setProperty("bounds", juce::var(b));
      nodes.add(juce::var(m));
    }
#endif
    int total = nodes.size();
    // AX-empty boundary (Issue F): custom-painted editors (RX) expose only
    // containers (window/groups), zero controls — node-id clicks have nothing
    // to target. Label it so agents stop re-discovering the wall, with the
    // fallback right in the response.
    int nInteractive = 0;
    for (auto& nv : nodes) {
      juce::String rr = nv["role"].toString();
      if (rr != "AXWindow" && rr != "AXGroup" && rr != "AXStaticText" &&
          rr != "AXUnknown" && !rr.isEmpty())
        ++nInteractive;
    }
    if (nInteractive == 0 && total > 0) {
      o->setProperty("axEmpty", true);
      o->setProperty("fallback", "AX-empty custom-painted editor: no node ids "
                                 "to click. Ground raw screen-coordinate "
                                 "clicks ({target:{x:..,y:..},op:press}, "
                                 "visible:true, screenshot) against the "
                                 "window bounds above, or drive automatable "
                                 "params (inspect lists them)");
    }
    juce::String roleF = jstr(args, "role");
    juce::Array<juce::var> scoped = nodes;
    if (roleF.isNotEmpty()) {
      scoped.clear();
      for (auto& nv : nodes)
        if (nv["role"].toString().equalsIgnoreCase(roleF)) scoped.add(nv);
    }
    total = scoped.size();
    juce::Array<juce::var> page;
    for (int i = offset; i < std::min(offset + limit, total); ++i)
      page.add(scoped.getReference(i));
    o->setProperty("nodes", juce::var(page));
    o->setProperty("total", total);
    o->setProperty("limit", limit);
    o->setProperty("offset", offset);
    setBundleProps(o, sd);
    if (sp.isNotEmpty()) {
#if JUCE_MAC
      juce::String saved, why2;
      if (noteCapture(plugprobeSaveNSViewShot(ed->getWindowHandle(),
                                          sp.toRawUTF8(), nullptr, nullptr),
                      sp, saved, why2))
        o->setProperty("screenshot", saved);
      else
        o->setProperty("shotSkipped", why2);
#else
      o->setProperty("shotSkipped", "no-editor-or-headless");
#endif
    }
    if (vis) {
      holdUi((int)jnum(args, "holdMs", 2000));
      o->setProperty("visible", true);
    }
    if (ed->isOnDesktop()) ed->removeFromDesktop();
    emitOk(juce::var(o));
    return 0;
  }

  if (cmd == "act") {
    juce::String via = jstr(args, "via", "os");
    juce::var steps =
        args.hasProperty("action") ? args["action"] : args["actions"];
    if (!steps.isArray() && args.hasProperty("ui_script"))
      steps = args["ui_script"];
    if (!steps.isArray() && !steps.isObject()) {
      emitErr(errObj("ARGS",
                     "act needs {action:{target:<index|name>,value:0..1}}; "
                     "empty action is a no-op error (no params touched)"));
      return 1;
    }
    if (auto* sa = steps.getArray()) {
      if (sa->size() == 0) {
        emitErr(errObj("ARGS", "act: empty actions array is a no-op error"));
        return 1;
      }
    }
    if (via == "os") {
#if !JUCE_MAC
      emitErr(errObj("UNIMPLEMENTED_M1", "os driver is macOS-only"));
      return 1;
#else
      // Standalone os actions run against a probe window (no audio flows):
      // labeled detached, like the juce probe. Real effect only happens for
      // timeline clicks inside a render, on the rendering instance.
      // Shape validation first (no instance needed): malformed steps fail
      // without touching any plugin. Targets are node ids (SPEC §5 priority)
      // or raw screen coords {x,y} for AX-empty custom-painted editors.
      auto coordOf = [&](const juce::var& s, double* x,
                         double* y) -> bool {
        if (!s.hasProperty("target")) return false;
        juce::var t = s["target"];
        if (!t.isObject()) return false;
        if (!t.hasProperty("x") || !t.hasProperty("y")) return false;
        *x = (double)t["x"];
        *y = (double)t["y"];
        return true;
      };
      auto checkShape = [&](const juce::var& s) -> bool {
        juce::String op = s.hasProperty("op") ? s["op"].toString() : "press";
        double cx0 = 0, cy0 = 0;
        bool isCoord = coordOf(s, &cx0, &cy0);
        if (!isCoord && s.hasProperty("target") && s["target"].isObject()) {
          emitErr(errObj("ARGS", "os act: object target needs {x,y} screen "
                                 "coords (e.g. {target:{x:900,y:600}})"));
          return false;
        }
        juce::String tgt =
            s.hasProperty("target") ? s["target"].toString() : "";
        if (!isCoord && tgt.isEmpty()) {
          emitErr(errObj("ARGS", "os act: step needs {target:<node-id>|{x,y},"
                                 "op:press|drag|type}; empty target is a "
                                 "no-op error (use snapshot to list ids, or "
                                 "raw {x,y} grounded by a visible screenshot)"));
          return false;
        }
        if (op != "press" && op != "click" && op != "drag" && op != "type") {
          emitErr(errObj("ARGS", "os act: unknown op '" + op +
                                   "' (press|drag|type)"));
          return false;
        }
        if (op == "type" &&
            (!s.hasProperty("text") || s["text"].toString().isEmpty())) {
          emitErr(errObj("ARGS", "os act: type needs {text}"));
          return false;
        }
        return true;
      };
      if (steps.isArray()) {
        for (auto& s : *steps.getArray())
          if (!checkShape(s)) return 1;
      } else if (!checkShape(steps)) {
        return 1;
      }
      juce::PluginDescription d;
      if (!findPlugin(jstr(args, "plugin"), argPaths(args), d)) {
        emitErr(errObj("NOT_FOUND", "plugin not found"));
        return 1;
      }
      if (!guiCapable(d)) {
        emitErr(errObj("UNIMPLEMENTED_M1", "os driver needs a GUI-hosted "
                                           "plugin (VST3/AU); LV2/etc expose "
                                           "no native window to click"));
        return 1;
      }
      juce::String ge;
      auto gui = guiCreate(d, jnum(args, "sr", kDefSr),
                           (int)jnum(args, "block", kDefBlock), ge);
      if (gui == nullptr) {
        emitErr(errObj("INSTANTIATE",
                       juce::String("gui-instantiate: ") + ge.substring(0, 120)));
        return 1;
      }
      bool wantVis = (bool)args["visible"];
      juce::String asp = args.hasProperty("shotAfter")
                             ? args["shotAfter"].toString()
                             : jstr(args, "shot");
      juce::String visWhy;
      juce::String why;
      auto* ed = openEditor(*gui, wantVis, why);
      if (ed == nullptr) {
        emitErr(errObj("NO_EDITOR",
                       juce::String("os act needs a live editor window: ") + why));
        return 1;
      }
      void* hv = ed->getWindowHandle();
      juce::Array<juce::var> res;
      auto hidGates = [&](const char* what) -> bool {
        if (!wantVis) {
          emitErr(errObj("ARGS", juce::String("os act ") + what +
                                   " at raw {x,y} needs \"visible\":true — HID "
                                   "events land in screen coordinates and the "
                                   "offscreen probe window cannot receive "
                                   "them (node-id press works headless via AX)"));
          return false;
        }
        if (!AXIsProcessTrusted()) {
          emitErr(errObj("AX_UNTRUSTED",
                         juce::String("os act ") + what +
                             " needs the Accessibility grant for THIS plugprobe "
                             "binary; ad-hoc rebuilds change its hash and "
                             "invalidate the grant (re-grant the current binary "
                             "in Settings, or sign it with a stable identity)"));
          return false;
        }
        return true;
      };
      auto doStep = [&](const juce::var& s) -> bool {
        auto* r = new juce::DynamicObject();
        double qx = 0, qy = 0;
        bool isCoord = coordOf(s, &qx, &qy);
        juce::String tgt =
            isCoord ? juce::String("(" + juce::String(qx, 1) + "," +
                                   juce::String(qy, 1) + ")")
                    : s["target"].toString();  // validated above
        juce::String op = s.hasProperty("op") ? s["op"].toString() : "press";
        if (isCoord) r->setProperty("fragile", true);  // SPEC §5: raw coords
        if (op == "press" || op == "click") {
          if (isCoord) {
            if (!hidGates("press")) return false;
            plugprobeFocusWindow(hv);  // else this click only activates the window
            if (!plugprobeAxClickAt(qx, qy)) {
              emitErr(errObj("AX_CLICK",
                             "os act: click failed at '" + tgt + "'"));
              return false;
            }
            r->setProperty("x", qx);
            r->setProperty("y", qy);
            r->setProperty("pressed", true);
            if (juce::var hit = hitNodeAt(hv, qx, qy); hit.isObject())
              r->setProperty("hit", hit);
          } else {
            double cx = 0, cy = 0;
            int rc = plugprobeAxPressById(tgt.toRawUTF8(), hv, &cx, &cy);
            if (rc == 0) {
              emitErr(errObj("ARGS", "os act: unknown node '" + tgt +
                                       "' (snapshot lists ids)"));
              return false;
            }
            if (rc < 0) {
              emitErr(errObj("AX_PRESS",
                             "os act: press failed on '" + tgt + "'"));
              return false;
            }
            pumpMessages();
            r->setProperty("node", tgt);
            auto* c = new juce::DynamicObject();
            c->setProperty("x", cx);
            c->setProperty("y", cy);
            r->setProperty("center", juce::var(c));
            r->setProperty("pressed", true);
            // Re-read post-press state: capture-independent proof it landed.
            for (auto& nn : plugprobeAxDump(hv)) {
              if (juce::String(nn.id) == tgt) {
                if (!nn.value.empty())
                  r->setProperty("state", juce::String(nn.value));
                r->setProperty("enabled", nn.enabled);
                break;
              }
            }
          }
        } else if (op == "drag") {
          double dx = (double)s["dx"], dy = (double)s["dy"];
          double cx = 0, cy = 0;
          if (isCoord) {
            if (!hidGates("drag")) return false;
            cx = qx;
            cy = qy;
          } else {
            if (!wantVis) {
              emitErr(errObj("ARGS", "os act drag needs \"visible\":true — HID "
                                     "events land in screen coordinates and the "
                                     "offscreen probe window cannot receive "
                                     "them (press works headless via AX)"));
              return false;
            }
            if (!AXIsProcessTrusted()) {
              emitErr(errObj("AX_UNTRUSTED",
                             "os act drag needs the Accessibility grant for THIS "
                             "plugprobe binary; ad-hoc rebuilds change its hash and "
                             "invalidate the grant (re-grant the current binary "
                             "in Settings, or sign it with a stable identity)"));
              return false;
            }
            bool found = false;  // resolve center without pressing
            for (auto& nn : plugprobeAxDump(hv)) {
              if (juce::String(nn.id) == tgt) {
                cx = nn.x + nn.w / 2;
                cy = nn.y + nn.h / 2;
                found = true;
                break;
              }
            }
            if (!found) {
              emitErr(errObj("ARGS", "os act: unknown node '" + tgt +
                                       "' (snapshot lists ids)"));
              return false;
            }
          }
          plugprobeFocusWindow(hv);  // else the down-stroke only activates
          if (!plugprobeAxDragAt(cx, cy, dx, dy)) {
            emitErr(errObj("AX_DRAG", "os act: drag failed on '" + tgt + "'"));
            return false;
          }
          // HID events queue in our own event loop: settle before re-reading.
          holdUi(300);
          if (isCoord) {
            r->setProperty("x", qx);
            r->setProperty("y", qy);
          } else {
            r->setProperty("node", tgt);
          }
          r->setProperty("dragged", true);
          if (isCoord) {
            if (juce::var hit = hitNodeAt(hv, cx, cy); hit.isObject())
              r->setProperty("hit", hit);
          } else {
            for (auto& nn : plugprobeAxDump(hv)) {
              if (juce::String(nn.id) == tgt) {
                if (!nn.value.empty())
                  r->setProperty("state", juce::String(nn.value));
                break;
              }
            }
          }
        } else if (op == "type") {
          juce::String text = s["text"].toString();  // validated above
          if (isCoord) {
            if (!hidGates("type")) return false;
            plugprobeFocusWindow(hv);
            if (!plugprobeAxClickAt(qx, qy)) {  // focus first, then type
              emitErr(errObj("AX_CLICK",
                             "os act: click failed at '" + tgt + "'"));
              return false;
            }
            pumpMessages();
            if (!plugprobeAxTypeText(text.toRawUTF8())) {
              emitErr(errObj("AX_TYPE", "os act: type failed"));
              return false;
            }
            holdUi(300);
            r->setProperty("x", qx);
            r->setProperty("y", qy);
            r->setProperty("typed", text);
          } else {
            if (!wantVis) {
              emitErr(errObj("ARGS", "os act type needs \"visible\":true — key "
                                     "events need a focused on-screen window "
                                     "(press works headless via AX)"));
              return false;
            }
            if (!AXIsProcessTrusted()) {
              emitErr(errObj("AX_UNTRUSTED",
                             "os act type needs the Accessibility grant for THIS "
                             "plugprobe binary; ad-hoc rebuilds change its hash and "
                             "invalidate the grant (re-grant the current binary "
                             "in Settings, or sign it with a stable identity)"));
              return false;
            }
            double cx = 0, cy = 0;  // focus first: click the field, then type
            plugprobeFocusWindow(hv);
            if (plugprobeAxPressById(tgt.toRawUTF8(), hv, &cx, &cy) <= 0) {
              emitErr(errObj("ARGS", "os act: unknown node '" + tgt +
                                       "' (snapshot lists ids)"));
              return false;
            }
            pumpMessages();
            if (!plugprobeAxTypeText(text.toRawUTF8())) {
              emitErr(errObj("AX_TYPE", "os act: type failed"));
              return false;
            }
            holdUi(300);
            r->setProperty("node", tgt);
            r->setProperty("typed", text);
          }
        }
        r->setProperty("op", op);
        res.add(juce::var(r));
        return true;
      };
      if (steps.isArray()) {
        for (auto& s : *steps.getArray())
          if (!doStep(s)) {
            if (ed->isOnDesktop()) ed->removeFromDesktop();
            return 1;
          }
      } else if (!doStep(steps)) {
        if (ed->isOnDesktop()) ed->removeFromDesktop();
        return 1;
      }
      auto* o = new juce::DynamicObject();
      o->setProperty("via", "os");
      o->setProperty("results", juce::var(res));
      o->setProperty("detached", true);
      o->setProperty("detachedNote", "probe window processes no audio; clicks "
                                     "with real effect happen as render "
                                     "timeline clicks on the rendering "
                                     "instance");
      if (asp.isNotEmpty()) {
        juce::String why2, saved;
        if (noteCapture(plugprobeSaveNSViewShot(hv, asp.toRawUTF8(), nullptr,
                                            nullptr),
                        asp, saved, why2))
          o->setProperty("shotAfter", saved);
        else
          o->setProperty("shotSkipped", why2);
      }
      if (wantVis) holdUi((int)jnum(args, "holdMs", 2000));
      if (ed->isOnDesktop()) ed->removeFromDesktop();
      if (wantVis) o->setProperty("visible", true);
      emitOk(juce::var(o));
      return 0;
#endif
    }
    // --via juce: param-plane only, headless-safe.
    juce::PluginDescription d;
    if (!findPlugin(jstr(args, "plugin"), argPaths(args), d)) {
      emitErr(errObj("NOT_FOUND", "plugin not found"));
      return 1;
    }
    juce::String asp = args.hasProperty("shotAfter")
                           ? args["shotAfter"].toString()
                           : jstr(args, "shot");
    bool wantVis = (bool)args["visible"];
    // GUI instance when the UI will be shown/captured, so the window shows
    // the acted state (same instance the deltas come from).
    std::unique_ptr<juce::AudioPluginInstance> guiOwned;
    juce::String visWhy;
    if ((asp.isNotEmpty() || wantVis) && guiCapable(d)) {
      juce::String ge;
      guiOwned = guiCreate(d, jnum(args, "sr", kDefSr),
                           (int)jnum(args, "block", kDefBlock), ge);
      if (guiOwned == nullptr && wantVis)
        visWhy = juce::String("gui-instantiate: ") + ge.substring(0, 120);
    } else if (wantVis) {
      visWhy = "unsupported-format";
    }
    juce::AudioPluginInstance* inst = nullptr;
    std::unique_ptr<juce::AudioPluginInstance> headOwned;
    if (guiOwned != nullptr) {
      inst = guiOwned.get();
    } else {
      juce::String e;
      headOwned = instantiate(d, jnum(args, "sr", kDefSr),
                              (int)jnum(args, "block", kDefBlock), e);
      if (headOwned == nullptr) {
        emitErr(errObj("INSTANTIATE", e));
        return 1;
      }
      inst = headOwned.get();
    }
    // Fail loud: every juce-plane step must name a real param. Effect-free
    // clicks are ARGS errors, never ok:true.
    auto checkStep = [&](const juce::var& s) -> bool {
      juce::String tgt = s.hasProperty("target") ? s["target"].toString() : "";
      if (tgt.isEmpty()) {
        emitErr(errObj("ARGS", "act: step needs {target:<index|name>,"
                               "value:0..1}; empty target is a no-op error"));
        return false;
      }
      if (findParamIndex(*inst, tgt) < 0) {
        emitErr(errObj("ARGS", "act: unknown param '" + tgt + "' (have: " +
                                   validParamNames(*inst) + ")"));
        return false;
      }
      return true;
    };
    if (steps.isArray()) {
      for (auto& s : *steps.getArray())
        if (!checkStep(s)) return 1;
    } else if (!checkStep(steps)) {
      return 1;
    }
    juce::Array<juce::var> res;
    auto one = [&](const juce::var& s) {
      auto* r = new juce::DynamicObject();
      juce::String tgt = s.hasProperty("target") ? s["target"].toString() : "";
      auto* m = new juce::DynamicObject();
      m->setProperty(tgt, s.hasProperty("value") ? (float)s["value"] : 1.f);
      auto deltas = applyParams(*inst, juce::var(m));
      r->setProperty("paramDelta", deltas);
      r->setProperty("uiOnly", deltas.size() == 0);
      res.add(juce::var(r));
    };
    if (steps.isArray()) {
      for (auto& s : *steps.getArray()) one(s);
    } else if (steps.isObject()) {
      one(steps);
    }
    auto* o = new juce::DynamicObject();
    o->setProperty("via", "juce");
    o->setProperty("results", juce::var(res));
    o->setProperty("detached", true);
    o->setProperty("detachedNote", "probe instance processes no audio; route "
                                   "params through render params/timeline to "
                                   "affect audio");
    if (guiOwned != nullptr && (asp.isNotEmpty() || wantVis)) {
      // Capture the instance that was acted on (post-action state).
      juce::String why;
      auto* ed = openEditor(*guiOwned, wantVis, why);
      if (ed == nullptr) {
        if (asp.isNotEmpty()) o->setProperty("shotSkipped", why);
        if (wantVis) visWhy = why;
      } else {
        if (asp.isNotEmpty()) {
#if JUCE_MAC
          juce::String saved, why;
          if (noteCapture(plugprobeSaveNSViewShot(ed->getWindowHandle(),
                                              asp.toRawUTF8(), nullptr,
                                              nullptr),
                          asp, saved, why))
            o->setProperty("shotAfter", saved);
          else
            o->setProperty("shotSkipped", why);
#else
          o->setProperty("shotSkipped", "no-editor-or-headless");
#endif
        }
        if (wantVis) holdUi((int)jnum(args, "holdMs", 2000));
        if (ed->isOnDesktop()) ed->removeFromDesktop();
      }
    } else if (asp.isNotEmpty()) {
      juce::String why, visDummy;
      juce::String saved = saveEditorShot(d, jnum(args, "sr", kDefSr),
                                          (int)jnum(args, "block", kDefBlock),
                                          asp, why, visDummy, false, 0);
      if (saved.isNotEmpty())
        o->setProperty("shotAfter", saved);
      else
        o->setProperty("shotSkipped", why);
    }
    if (wantVis) {
      if (visWhy.isNotEmpty())
        o->setProperty("visibleSkipped", visWhy);
      else
        o->setProperty("visible", true);
    }
    emitOk(juce::var(o));
    return 0;
  }

  if (cmd == "session-start" || cmd == "session-act" ||
      cmd == "session-stop" || cmd == "meters") {
    emitErr(errObj("UNIMPLEMENTED_M2",
                   cmd + ": no live session in M0+M3 (cannot host an editor "
                         "on a rendering instance with audio running). "
                         "Interim click path: render timeline automating "
                         "a toggle on one instance, e.g. timeline "
                         "[{atMs:0,params:{6:1}},{atMs:8000,params:{6:0}}] "
                         "for an automatable switch, or timeline clicks "
                         "[{atMs:0,click:'<node-id>'}] for native "
                         "buttons no param can reach (snapshot lists ids); "
                         "verify the UI with shot"));
    return 1;
  }

  emitErr(errObj("CMD", "unknown command: " + cmd));
  return 1;
}
