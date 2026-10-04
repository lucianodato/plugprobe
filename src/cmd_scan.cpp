// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_scan.cpp: `scan` command. See cmds.h.
#include "cmds.h"
#include "gui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {
int runScan(const juce::var& args)
{
    bool explicitPaths = args.hasProperty("paths");
    auto dirs = argPaths(args);
    juce::String cacheP = scanCachePath(args);
    bool cached = false;
    juce::KnownPluginList list;
    // Opt-in cache, validated by dir set + recursive mtime. rescan:true
    // forces a fresh pass. Stale-clock risk is the known ceiling.
    if (cacheP.isNotEmpty() && !(bool)args["rescan"] &&
        jstr(args, "format").isEmpty() && loadScanCache(cacheP, dirs, list))
      cached = true;
    if (!cached) {
      scanInto(list, dirs, jstr(args, "format"), explicitPaths);
      if (cacheP.isNotEmpty() && jstr(args, "format").isEmpty())
        saveScanCache(cacheP, dirs, list);
    }
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
    o->setProperty("cached", cached);
    emitOk(juce::var(o));
    return 0;
}
}  // namespace pp
