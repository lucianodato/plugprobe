// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// core.cpp: shared backend implementation. See core.h.
#include "core.h"
#include "plugprobe_os.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#if !JUCE_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif
#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace pp {
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
juce::String jstr(const juce::var& v, const char* k, const char* d) {
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

// Opt-in scan cache (scan {cache} / PLUGPROBE_SCAN_CACHE): a KnownPluginList
// XML validated by sorted dir set + newest recursive mtime. A stat walk is
// orders of magnitude cheaper than loading every plugin binary, and it makes
// repeated agent loops (scan, then render/inspect per plugin) usable.
static juce::String scanDirKey(std::vector<juce::String> dirs) {
  std::sort(dirs.begin(), dirs.end());
  dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
  juce::String k;
  for (auto& d : dirs) k += d + "\n";
  return k;
}
static double scanMaxMtime(const std::vector<juce::String>& dirs) {
  double m = 0;
  for (auto& d : dirs) {
    juce::File root(d);
    if (!root.isDirectory()) continue;
    for (auto& f :
         root.findChildFiles(juce::File::findFilesAndDirectories, true)) {
      double t = (double)f.getLastModificationTime().toMilliseconds();
      if (t > m) m = t;
    }
  }
  return m;
}
juce::String scanCachePath(const juce::var& a) {
  juce::String p = jstr(a, "cache");
  if (p.isNotEmpty()) return p;
  return juce::SystemStats::getEnvironmentVariable("PLUGPROBE_SCAN_CACHE", "");
}
bool loadScanCache(const juce::String& path,
                   const std::vector<juce::String>& dirs,
                   juce::KnownPluginList& list) {
  juce::XmlDocument doc(juce::File(path).loadFileAsString());
  auto xml = doc.getDocumentElement();
  if (xml == nullptr) return false;
  if (xml->getStringAttribute("plugprobe_dirs") != scanDirKey(dirs))
    return false;
  if (scanMaxMtime(dirs) > xml->getDoubleAttribute("plugprobe_maxmtime"))
    return false;
  list.recreateFromXml(*xml);
  return true;
}
void saveScanCache(const juce::String& path,
                   const std::vector<juce::String>& dirs,
                   const juce::KnownPluginList& list) {
  if (auto xml = list.createXml()) {
    xml->setAttribute("plugprobe_dirs", scanDirKey(dirs));
    xml->setAttribute("plugprobe_maxmtime", scanMaxMtime(dirs));
    juce::File cf(path);
    cf.getParentDirectory().createDirectory();
    cf.replaceWithText(xml->toString());
  }
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
    // A bundle path contributes its PARENT: bundles are discovered as
    // children of search dirs, never as search dirs themselves. A plain
    // directory additionally scans inside itself.
    ds.push_back(qf.getParentDirectory().getFullPathName());
    if (qf.isDirectory()) ds.push_back(qf.getFullPathName());
  }
  juce::KnownPluginList list;
  // Same cache as `scan {cache}` via PLUGPROBE_SCAN_CACHE, so repeated agent
  // loops skip re-loading every plugin binary on each command.
  juce::String cacheP =
      juce::SystemStats::getEnvironmentVariable("PLUGPROBE_SCAN_CACHE", "");
  if (cacheP.isEmpty() || !loadScanCache(cacheP, ds, list)) {
    scanInto(list, ds, "", true);
    if (cacheP.isNotEmpty()) saveScanCache(cacheP, ds, list);
  }
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

// Pace an offline render to wall-clock (1x) while a visible window is open, so
// the UI can be followed in real time. Sleeps in short pumped slices to keep
// the window live. Never slows headless renders; never catches up when DSP is
// slower than realtime.
void paceToRealtime(double t0ms, size_t doneFrames, double sr) {
  double target = t0ms + doneFrames / sr * 1000.0;
  for (;;) {
    double left = target - juce::Time::getMillisecondCounterHiRes();
    if (left <= 0) break;
#if JUCE_MAC
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::min(left / 1000.0, 0.05),
                       false);
#else
    plugprobePumpApp(std::min(left / 1000.0, 0.05));
#endif
  }
}

// Flush plugin-side deferred (AsyncUpdater) param updates during offline renders.
// Headless render loops never re-enter the event loop, so mid-render timeline
// switches would otherwise sit in the plugin's message queue forever.
void pumpMessages() {
  if (!plugprobeOsCaps().editor) return;
#if JUCE_MAC
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);
#else
  plugprobePumpApp(0.005);
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
}  // namespace pp
