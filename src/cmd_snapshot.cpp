// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_snapshot.cpp: `snapshot` command. See cmds.h.
#include "cmds.h"
#include "gui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {
int runSnapshot(const juce::var& args)
{
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
    if (!plugprobeOsCaps().tree) {
      emitErr(errObj("NO_OS_DRIVER", "snapshot needs the OS UI-tree driver, "
                                     "unavailable on this OS"));
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
      if (!plugprobeOsCaps().editor) {
        o->setProperty("shotSkipped", "NO_OS_DRIVER");
      } else {
        juce::String saved, why2;
        if (noteCapture(plugprobeSaveNSViewShot(ed->getWindowHandle(),
                                            sp.toRawUTF8(), nullptr, nullptr),
                        sp, saved, why2))
          o->setProperty("screenshot", saved);
        else
          o->setProperty("shotSkipped", why2);
      }
    }
    if (vis) {
      holdUi((int)jnum(args, "holdMs", 2000));
      o->setProperty("visible", true);
    }
    if (ed->isOnDesktop()) ed->removeFromDesktop();
    emitOk(juce::var(o));
    return 0;
}
}  // namespace pp
