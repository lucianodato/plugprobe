// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_inspect.cpp: `inspect` command. See cmds.h.
#include "cmds.h"
#include "gui.h"
#include "gui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {
int runInspect(const juce::var& args)
{
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
}  // namespace pp
