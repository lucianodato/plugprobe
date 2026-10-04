// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_act.cpp: `act` command. See cmds.h.
#include "cmds.h"
#include "gui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {
int runAct(const juce::var& args)
{
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
      // without touching any plugin. Targets are node ids (nodeId → name
      // → role+index → raw x,y, least to most fragile)
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
        if (isCoord) r->setProperty("fragile", true);  // raw coords: re-ground on replay
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
}  // namespace pp
