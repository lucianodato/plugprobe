// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_session.cpp: `session start|act|stop` + `meters`. See session.h.
#include "cmds.h"
#include "gui.h"
#include "session.h"

#include <algorithm>

namespace pp {
namespace {
double actAtMs(const juce::var& args, double startWallMs) {
  if (args.hasProperty("at_ms")) return (double)args["at_ms"];
  if (args.hasProperty("atMs")) return (double)args["atMs"];
  return std::max(
      0.0, juce::Time::getMillisecondCounterHiRes() - startWallMs);
}
bool saveSessionVar(const juce::var& s) {
  juce::File f = resolveSessionFile(s["dir"].toString());
  f.deleteFile();
  std::unique_ptr<juce::OutputStream> out(f.createOutputStream());
  if (out == nullptr) return false;
  out->writeText(juce::JSON::toString(s, true), false, false, "\n");
  out->flush();
  return true;
}
}  // namespace

int runSessionStart(const juce::var& args) {
  juce::String loop = jstr(args, "loop");
  if (loop.isEmpty()) loop = jstr(args, "in");
  juce::String outP = jstr(args, "out");
  bool bypass = (bool)args["bypass"];
  juce::String pq = jstr(args, "plugin");
  if (loop.isEmpty() || outP.isEmpty() || (!bypass && pq.isEmpty())) {
    emitErr(errObj("ARGS", "session start needs {plugin,loop,out} "
                           "(or {loop,out,bypass:true} for a passthrough)"));
    return 1;
  }
  std::vector<std::vector<float>> ch;
  double loopSr = kDefSr;
  if (!readWav(loop, ch, loopSr) || ch.empty()) {
    emitErr(errObj("IO", "cannot read loop wav: " + loop));
    return 1;
  }
  double sr = jnum(args, "sr", kDefSr);
  int block = (int)jnum(args, "block", kDefBlock);
  juce::PluginDescription d;
  double t0 = juce::Time::getMillisecondCounterHiRes();
  if (!bypass) {
    if (!findPlugin(pq, argPaths(args), d)) {
      emitErr(errObj("NOT_FOUND", "plugin not found: " + pq));
      return 1;
    }
    juce::String e;  // validate now: instantiate + check base params
    auto inst = instantiate(d, sr, block, e);
    if (inst == nullptr) {
      emitErr(errObj("INSTANTIATE", e));
      return 1;
    }
    if (args.hasProperty("params")) {
      if (juce::String u = unknownTargetIn(*inst, args["params"]);
          u.isNotEmpty()) {
        emitErr(errObj("ARGS", "session start: unknown param '" + u +
                                   "' (have: " + validParamNames(*inst) +
                                   ")"));
        return 1;
      }
    }
  }
  double latencyMs = juce::Time::getMillisecondCounterHiRes() - t0;
  // Customizable home for the session: CI passes dir=renders/<date>/session
  // so session.json + eventLog are uploadable artifacts alongside the take.
  juce::String dir = jstr(args, "dir");
  if (dir.isNotEmpty()) {
    juce::File dd(dir);
    if (!juce::File::isAbsolutePath(dir))
      dd = juce::File::getCurrentWorkingDirectory().getChildFile(dir);
    if (dd.getChildFile("session.json").existsAsFile()) {
      emitErr(errObj("ARGS", "session start: dir already holds a session: " +
                                 dd.getFullPathName()));
      return 1;
    }
    dd.createDirectory();
    dir = dd.getFullPathName();
  } else {
    dir = newSessionDir();
  }
  auto* s = new juce::DynamicObject();
  s->setProperty("version", 1);
  s->setProperty("dir", dir);
  s->setProperty("plugin", pq);
  s->setProperty("bypass", bypass);
  if (!bypass) setBundleProps(s, d);
  auto abspath = [](const juce::String& p) {
    return juce::File::isAbsolutePath(p)
               ? p
               : juce::File::getCurrentWorkingDirectory()
                     .getChildFile(p)
                     .getFullPathName();
  };
  s->setProperty("loop", abspath(loop));
  s->setProperty("out", abspath(outP));
  if (jstr(args, "video").isNotEmpty())
    s->setProperty("video", abspath(jstr(args, "video")));
  s->setProperty("sr", sr);
  s->setProperty("block", block);
  s->setProperty("visible", (bool)args["visible"]);
  s->setProperty("loopFrames", (double)ch[0].size());
  s->setProperty("loopSr", loopSr);
  s->setProperty("startedAt",
                 juce::Time::getCurrentTime().toISO8601(true));
  s->setProperty("startWallMs", t0);
  s->setProperty("params", args.hasProperty("params")
                               ? args["params"]
                               : juce::var(new juce::DynamicObject()));
  s->setProperty("events", juce::var(juce::Array<juce::var>()));
  juce::var sv(s);
  if (!saveSessionVar(sv)) {
    emitErr(errObj("IO", "cannot write session dir: " + dir));
    return 1;
  }
  auto* o = new juce::DynamicObject();
  o->setProperty("session", resolveSessionFile(dir).getFullPathName());
  o->setProperty("outFile", s->getProperty("out"));
  o->setProperty("startedAt", s->getProperty("startedAt"));
  o->setProperty("latencyMs", latencyMs);
  o->setProperty("loopFrames", (double)ch[0].size());
  if (!bypass) setBundleProps(o, d);
  emitOk(juce::var(o));
  return 0;
}

int runSessionAct(const juce::var& args) {
  juce::var s;
  juce::String se;
  if (!loadSession(jstr(args, "session"), s, se)) {
    emitErr(errObj("ARGS", se));
    return 1;
  }
  bool bypass = (bool)s["bypass"];
  double sr = (double)s["sr"], loopSr = (double)s["loopSr"];
  int block = (int)s["block"];
  double atMs = actAtMs(args, (double)s["startWallMs"]);
  bool hasParams = args.hasProperty("params");
  juce::String clickId = jstr(args, "click");
  bool coordClick = args.hasProperty("click") && args["click"].isObject();
  if (!hasParams && clickId.isEmpty() && !coordClick) {
    emitErr(errObj("ARGS", "session act needs {params:{...}} and/or "
                           "{click:<node-id>|{x,y}} (snapshot lists ids)"));
    return 1;
  }
  auto* ev = new juce::DynamicObject();
  ev->setProperty("atMs", atMs);
  juce::var paramDelta;
  if (hasParams) {
    if (!args["params"].isObject() ||
        args["params"].getDynamicObject()->getProperties().size() == 0) {
      emitErr(errObj("ARGS", "session act: params needs {<index|name>:0..1} "
                             "(empty map is a no-op error)"));
      return 1;
    }
    if (bypass) {
      emitErr(errObj("ARGS", "session act: bypass session has no params"));
      return 1;
    }
    juce::PluginDescription d;
    if (!findPlugin(s["plugin"].toString(), argPaths(s), d)) {
      emitErr(errObj("NOT_FOUND", "plugin not found"));
      return 1;
    }
    juce::String e;
    auto inst = instantiate(d, sr, block, e);
    if (inst == nullptr) {
      emitErr(errObj("INSTANTIATE", e));
      return 1;
    }
    applyParams(*inst, s["params"]);  // current state first
    if (juce::String u = unknownTargetIn(*inst, args["params"]);
        u.isNotEmpty()) {
      emitErr(errObj("ARGS", "session act: unknown param '" + u +
                                 "' (have: " + validParamNames(*inst) +
                                 ")"));
      return 1;
    }
    paramDelta = applyParams(*inst, args["params"]);
    auto* so = s.getDynamicObject();
    so->setProperty("params", mergeParams(s["params"], args["params"]));
    ev->setProperty("params", args["params"]);
    ev->setProperty("deltaCount", paramDelta.size());
  }
  if (clickId.isNotEmpty() || coordClick) {
    if (bypass) {
      emitErr(errObj("ARGS", "session act: bypass session has no editor"));
      return 1;
    }
#if !JUCE_MAC
    emitErr(errObj("NO_OS_DRIVER", "session act clicks are macOS-only"));
    return 1;
#else
    juce::PluginDescription d;
    if (!findPlugin(s["plugin"].toString(), argPaths(s), d) ||
        !guiCapable(d)) {
      emitErr(errObj("NO_OS_DRIVER", "session act clicks need a "
                                        "GUI-hosted plugin (VST3/AU)"));
      return 1;
    }
    if (coordClick && !(bool)s["visible"]) {
      emitErr(errObj("ARGS", "session act: coordinate clicks need a session "
                             "started with \"visible\":true (HID events "
                             "cannot reach the offscreen window)"));
      return 1;
    }
    // Read-only verify on a probe window: the single real press happens at
    // stop, on the rendering instance (verifying by pressing here would
    // double-toggle Learn-type buttons).
    juce::String ge;
    auto gui = guiCreate(d, sr, block, ge);
    if (gui == nullptr) {
      emitErr(errObj("INSTANTIATE",
                     juce::String("gui-instantiate: ") + ge.substring(0, 120)));
      return 1;
    }
    juce::String why;
    auto* ed = openEditor(*gui, false, why);
    if (ed == nullptr) {
      emitErr(errObj("NO_EDITOR",
                     juce::String("session act click needs an editor: ") + why));
      return 1;
    }
    if (clickId.isNotEmpty()) {
      bool found = false;
      for (auto& nn : plugprobeAxDump(ed->getWindowHandle()))
        if (juce::String(nn.id) == clickId) {
          found = true;
          if (!nn.value.empty()) ev->setProperty("nodeState", juce::String(nn.value));
          break;
        }
      if (ed->isOnDesktop()) ed->removeFromDesktop();
      if (!found) {
        emitErr(errObj("ARGS", "session act: unknown node '" + clickId +
                                   "' (snapshot lists ids)"));
        return 1;
      }
      ev->setProperty("click", clickId);
    } else {
      if (ed->isOnDesktop()) ed->removeFromDesktop();
      auto* cc = new juce::DynamicObject();
      cc->setProperty("x", (double)args["click"]["x"]);
      cc->setProperty("y", (double)args["click"]["y"]);
      ev->setProperty("click", juce::var(cc));
      ev->setProperty("fragile", true);
    }
    ev->setProperty("replayAtStop", true);
#endif
  }
  auto* so = s.getDynamicObject();
  juce::Array<juce::var> events(*s["events"].getArray());
  events.add(juce::var(ev));
  so->setProperty("events", juce::var(events));
  if (!saveSessionVar(s)) {
    emitErr(errObj("IO", "cannot update session"));
    return 1;
  }
  // Ears: meters over the loop with post-act params (params plane; clicks
  // land at stop replay, so meters here reflect params only).
  std::vector<std::vector<float>> ch;
  double fsr = sr;
  readWav(s["loop"].toString(), ch, fsr);
  std::vector<std::vector<float>> rendered;
  juce::String re;
  if (!bypass) {
    juce::PluginDescription d;
    findPlugin(s["plugin"].toString(), argPaths(s), d);
    if (!renderLoopOffline(&d, ch, sr, block, s["params"], rendered, re)) {
      emitErr(errObj("INSTANTIATE", re));
      return 1;
    }
  } else {
    rendered = ch;
  }
  auto* m = new juce::DynamicObject();
  if (!rendered.empty())
    metersOf(rendered, sr, 0, rendered[0].size(), m);
  auto* o = new juce::DynamicObject();
  o->setProperty("atMs", atMs);
  if (hasParams) {
    o->setProperty("paramDelta", paramDelta);
    o->setProperty("uiOnly", paramDelta.size() == 0);
  } else {
    o->setProperty("uiOnly", true);
    o->setProperty("replayAtStop", true);
    if (clickId.isNotEmpty()) o->setProperty("node", clickId);
  }
  o->setProperty("meters",
                 juce::var(m));  // compact deltas, never full state
  (void)loopSr;
  emitOk(juce::var(o));
  return 0;
}

int runMeters(const juce::var& args) {
  juce::var s;
  juce::String se;
  if (!loadSession(jstr(args, "session"), s, se)) {
    emitErr(errObj("ARGS", se));
    return 1;
  }
  double sr = (double)s["sr"];
  int block = (int)s["block"];
  double windowMs = jnum(args, "window_ms", 1000.0);
  if (args.hasProperty("windowMs")) windowMs = (double)args["windowMs"];
  std::vector<std::vector<float>> ch;
  double fsr = sr;
  if (!readWav(s["loop"].toString(), ch, fsr) || ch.empty()) {
    emitErr(errObj("IO", "cannot read session loop"));
    return 1;
  }
  std::vector<std::vector<float>> rendered;
  juce::String re;
  if (!(bool)s["bypass"]) {
    juce::PluginDescription d;
    if (!findPlugin(s["plugin"].toString(), argPaths(s), d)) {
      emitErr(errObj("NOT_FOUND", "plugin not found"));
      return 1;
    }
    if (!renderLoopOffline(&d, ch, sr, block, s["params"], rendered, re)) {
      emitErr(errObj("INSTANTIATE", re));
      return 1;
    }
  } else {
    rendered = ch;
  }
  size_t win = (size_t)(windowMs / 1000.0 * sr);
  size_t from = rendered[0].size() > win ? rendered[0].size() - win : 0;
  auto* o = new juce::DynamicObject();
  metersOf(rendered, sr, from, rendered[0].size() - from, o);
  o->setProperty("windowMs", windowMs);
  emitOk(juce::var(o));
  return 0;
}

int runSessionStop(const juce::var& args) {
  juce::var s;
  juce::String se;
  if (!loadSession(jstr(args, "session"), s, se)) {
    emitErr(errObj("ARGS", se));
    return 1;
  }
  double sr = (double)s["sr"], loopSr = (double)s["loopSr"];
  int block = (int)s["block"];
  juce::String outP =
      args.hasProperty("out") ? args["out"].toString() : s["out"].toString();
  // Timeline replay of every act, in order: one render call is the scripted
  // open → tweak → capture session equivalent.
  juce::Array<juce::var> tl;
  juce::Array<juce::var> log;
  for (auto& e : *s["events"].getArray()) {
    auto* t = new juce::DynamicObject();
    t->setProperty("atMs", (double)e["atMs"]);
    if (e.hasProperty("params")) t->setProperty("params", e["params"]);
    if (e.hasProperty("click")) t->setProperty("click", e["click"]);
    tl.add(juce::var(t));
    auto* l = new juce::DynamicObject();
    l->setProperty("atMs", (double)e["atMs"]);
    l->setProperty("samplePos",
                   (double)(int64_t)((double)e["atMs"] / 1000.0 * sr));
    if (e.hasProperty("params")) {
      l->setProperty("params", e["params"]);
      l->setProperty("deltaCount", (int)e["deltaCount"]);
    }
    if (e.hasProperty("click")) l->setProperty("click", e["click"]);
    log.add(juce::var(l));
  }
  // ponytail: re-exec this binary for the take — one render engine, zero
  // duplication. In-process render if stop ever needs sub-ms event latency.
  juce::File tmp =
      juce::File::getSpecialLocation(juce::File::tempDirectory)
          .getChildFile("pp-stop-" + juce::Uuid().toString() + ".json");
  auto* ra = new juce::DynamicObject();
  ra->setProperty("plugin", s["plugin"]);
  ra->setProperty("in", s["loop"]);
  ra->setProperty("out", outP);
  ra->setProperty("sr", sr);
  ra->setProperty("block", block);
  ra->setProperty("bypass", (bool)s["bypass"]);
  ra->setProperty("params", s["params"]);
  ra->setProperty("timeline", juce::var(tl));
  ra->setProperty("tail_ms", 0);
  if (jstr(s, "video").isNotEmpty()) ra->setProperty("video", s["video"]);
  if ((bool)s["visible"]) ra->setProperty("visible", true);
  tmp.replaceWithText(juce::JSON::toString(juce::var(ra), true));
  juce::String exe = juce::File::getSpecialLocation(
                         juce::File::currentExecutableFile)
                         .getFullPathName();
  juce::ChildProcess cp;
  juce::StringArray argv;
  argv.add(exe);
  argv.add("render");
  argv.add("--json");
  argv.add(tmp.getFullPathName());
  if (!cp.start(argv)) {
    tmp.deleteFile();
    emitErr(errObj("IO", "session stop: cannot re-exec render"));
    return 1;
  }
  // Drain while waiting: plugin chatter on stderr would deadlock a blind
  // wait once the pipe fills (same lesson as the crash-isolation parent).
  juce::String all;
  int waited = 0;
  while (cp.isRunning() && waited < 300000) {
    all += cp.readAllProcessOutput();
    juce::Thread::sleep(5);
    waited += 5;
  }
  all += cp.readAllProcessOutput();
  juce::String last;
  for (auto& ln : juce::StringArray::fromLines(all))
    if (ln.trim().isNotEmpty()) last = ln.trim();
  juce::var cr;
  if (!juce::JSON::parse(last, cr) || !cr.isObject() || !(bool)cr["ok"]) {
    juce::String msg =
        cr.isObject() && cr.hasProperty("error")
            ? cr["error"]["message"].toString()
            : last.substring(0, 200);
    tmp.deleteFile();
    emitErr(errObj("RENDER", "session stop: take render failed: " + msg));
    return 1;
  }
  double durS = (double)s["loopFrames"] / (loopSr > 0 ? loopSr : sr);
  auto* o = new juce::DynamicObject();
  o->setProperty("outFile", outP);
  o->setProperty("durS", durS);
  o->setProperty("eventLog", juce::var(log));
  o->setProperty("hash", cr["data"]["hash"].toString());
  o->setProperty("peakDb", (double)cr["data"]["peakDb"]);
  // Pass through the replay's video verdict (path or videoSkipped reason).
  if (cr["data"].hasProperty("video"))
    o->setProperty("video", cr["data"]["video"]);
  if (cr["data"].hasProperty("videoSkipped"))
    o->setProperty("videoSkipped", cr["data"]["videoSkipped"]);
  o->setProperty("xruns", 0);
  o->setProperty("timing", "agent-paced offline (v1): single loop pass, "
                           "events aligned by samplePos; realtime loop + "
                           "monitor mirror are the documented ceiling");
  (void)block;
  emitOk(juce::var(o));
  return 0;
}
}  // namespace pp
