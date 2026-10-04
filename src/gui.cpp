// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// gui.cpp: live-editor helpers implementation. See gui.h.
#include "gui.h"

namespace pp {
// --- Opt-in live UI (visible windows) + editor screenshots ---
// Diagnostic-only: never fail the command; off by default (headless path is
// byte-identical without them). macOS-only capture (plugprobe_os_mac.mm).
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


#if JUCE_MAC
// Poll the AX tree until a node id appears (or timeout): third-party views
// attach asynchronously after addToDesktop, so a single synchronous dump
// races them — especially cold in CI. Pumps both runloops while waiting.
bool axWaitForId(void* hv, const juce::String& nodeId, int timeoutMs) {
  bool triedActivate = false;
  int waited = 0;
  while (waited <= timeoutMs) {
    auto nodes = plugprobeAxDump(hv);
    for (auto& nn : nodes)
      if (juce::String(nn.id) == nodeId) return true;
    // Headless CI sessions expose zero AX windows until the app activates:
    // one attempt, only when the tree is empty (local runs with a visible
    // tree never touch this — no dock bounce, no focus steal).
    if (!triedActivate && nodes.empty()) {
      triedActivate = true;
      plugprobeTryActivate();
    }
    pumpMessages();
    plugprobePumpApp(0.05);
    waited += 100;
    pumpMessages();
  }
  return false;
}
#endif
}  // namespace pp
