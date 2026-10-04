// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_manual.cpp: `manual` command. See cmds.h.
#include "cmds.h"

namespace pp {
// RTFM helper: locates the plugin's documentation so the calling agent can
// READ IT ITSELF (agents parse PDFs natively — plugprobe ships no PDF parser,
// no network fetcher, no new deps). Two modes:
//   {plugin} — scan the plugin bundle for bundled docs (*.pdf, *manual*,
//     *guide*, *help*, readme, ...). Empty list + note when the vendor ships
//     none (common): download the vendor PDF yourself and read it directly.
//   {path} — explicit manual file-or-dir (e.g. an already-downloaded vendor
//     manual); returned as-is after an existence check.
// Either way the agent grounds control semantics (what each snapshot node id
// DOES) before clicking, instead of guessing from labels.
static bool isDocFile(const juce::File& f) {
  juce::String ext = f.getFileExtension().toLowerCase();
  if (ext == ".pdf" || ext == ".html" || ext == ".htm" || ext == ".txt" ||
      ext == ".md" || ext == ".markdown" || ext == ".rtf")
    return true;
  juce::String n = f.getFileNameWithoutExtension().toLowerCase();
  return n.contains("manual") || n.contains("guide") || n.contains("help") ||
         n.contains("handbook") || n == "readme" || n == "read me" ||
         n.contains("doc");
}
static juce::var docEntry(const juce::File& f) {
  auto* o = new juce::DynamicObject();
  o->setProperty("path", f.getFullPathName());
  o->setProperty("name", f.getFileName());
  o->setProperty("bytes", (double)f.getSize());
  return juce::var(o);
}
int runManual(const juce::var& args) {
  juce::Array<juce::var> found;
  auto* o = new juce::DynamicObject();
  juce::String given = jstr(args, "path");
  if (given.isNotEmpty()) {
    juce::File p(given);
    if (!p.exists()) {
      emitErr(errObj("ARGS", "manual: path does not exist: " + given));
      return 1;
    }
    if (p.isDirectory()) {
      for (auto& f : p.findChildFiles(juce::File::findFiles, true))
        if (isDocFile(f)) found.add(docEntry(f));
    } else {
      found.add(docEntry(p));
    }
  } else {
    juce::PluginDescription d;
    if (!findPlugin(jstr(args, "plugin"), argPaths(args), d)) {
      emitErr(errObj("NOT_FOUND", "plugin not found"));
      return 1;
    }
    setBundleProps(o, d);
    juce::File bundle(d.fileOrIdentifier);
    juce::File root = bundle.isDirectory() ? bundle : bundle.getParentDirectory();
    for (auto& f : root.findChildFiles(juce::File::findFiles, true)) {
      if (found.size() >= 25) break;
      if (f.getFileName().startsWithChar('.')) continue;
      if (isDocFile(f)) found.add(docEntry(f));
    }
  }
  found.sort();
  o->setProperty("manuals", juce::var(found));
  if (found.size() == 0)
    o->setProperty("note", "no bundled docs found; download the vendor manual "
                           "yourself and read it with your own tools, then "
                           "drive controls via snapshot node ids");
  emitOk(juce::var(o));
  return 0;
}
}  // namespace pp
